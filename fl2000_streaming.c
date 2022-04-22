// SPDX-License-Identifier: GPL-2.0
/*
 * Original driver uses default altsetting (#0) of streaming interface, which allows bursts of bulk
 * transfers of 15x1024 bytes on output. But the HW actually works incorrectly here: it uses same
 * endpoint #1 across interfaces 1 and 2, which is not allowed by USB specification: endpoint
 * addresses can be shared only between alternate settings, not interfaces. In order to workaround
 * this we use isochronous transfers instead of bulk. There is a possibility that we still can use
 * bulk transfers with interface 0, but this is yet to be checked.
 *
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

/* Triple buffering:
 *  - one buffer for HDMI rendering
 *  - one buffer for USB transmission
 *  - one buffer for DRM/KMS data copy
 */
#define FL2000_SB_MIN 3
#define FL2000_SB_NUM (FL2000_SB_MIN + 1)

#define FL2000_URB_TIMEOUT 100


struct fl2000_stream_buf {
	struct list_head list;
	struct fl2000* parent;
	struct sg_table sgt;
	struct page **pages;
	int nr_pages;
	size_t size;
	void *vaddr;
	int in_flight;
};

static void fl2000_free_sb(struct fl2000_stream_buf *sb)
{
	int i;
	struct fl2000* fl2000_dev = sb->parent;

	vunmap(sb->vaddr);

	sg_free_table(&sb->sgt);

	for (i = 0; i < sb->nr_pages && sb->pages[i]; i++)
		__free_page(sb->pages[i]);

	drmm_kfree(&fl2000_dev->drm, sb->pages);

	drmm_kfree(&fl2000_dev->drm, sb);
}

static struct fl2000_stream_buf *fl2000_alloc_sb(struct fl2000* fl2000_dev, size_t size)
{
	int i, ret;
	struct fl2000_stream_buf *sb;
	int nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;

	sb = drmm_kzalloc(&fl2000_dev->drm, sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return NULL;

	sb->nr_pages = nr_pages;
	sb->size = size;
	sb->in_flight = 0;
	sb->parent = fl2000_dev;

	sb->pages = drmm_kcalloc(&fl2000_dev->drm, nr_pages, sizeof(*sb->pages), GFP_KERNEL);
	if (!sb->pages)
		goto error;

	for (i = 0; i < nr_pages; i++) {
		sb->pages[i] = alloc_page(GFP_KERNEL);
		if (!sb->pages[i])
			goto error;
	}

	ret = sg_alloc_table_from_pages(&sb->sgt, sb->pages, nr_pages, 0, size, GFP_KERNEL);
	if (ret != 0)
		goto error;

	sb->vaddr = vmap(sb->pages, nr_pages, VM_MAP, PAGE_KERNEL);
	if (!sb->vaddr)
		goto error;

	INIT_LIST_HEAD(&sb->list);
	memset(sb->vaddr, 0, nr_pages << PAGE_SHIFT);
	return sb;

error:
	fl2000_free_sb(sb);

	return NULL;
}

static void fl2000_stream_put_buffers(struct fl2000 *fl2000_dev)
{
	struct fl2000_stream_buf *cur_sb, *temp_sb;

	list_for_each_entry_safe (cur_sb, temp_sb, &fl2000_dev->render_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
	list_for_each_entry_safe (cur_sb, temp_sb, &fl2000_dev->transmit_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
	list_for_each_entry_safe (cur_sb, temp_sb, &fl2000_dev->wait_list, list) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);
	}
}

static int fl2000_stream_get_buffers(struct fl2000 *fl2000_dev, size_t size)
{
	int i, ret;
	struct fl2000_stream_buf *cur_sb;

	BUG_ON(!list_empty(&fl2000_dev->render_list));

	for (i = 0; i < FL2000_SB_NUM; i++) {
		cur_sb = fl2000_alloc_sb(fl2000_dev, size);
		if (!cur_sb) {
			ret = -ENOMEM;
			goto error;
		}

		list_add(&cur_sb->list, &fl2000_dev->render_list);
	}

	return 0;

error:
	fl2000_stream_put_buffers(fl2000_dev);
	return ret;
}

void fl2000_stream_release(struct fl2000 *fl2000_dev)
{
	fl2000_stream_disable(fl2000_dev);
	destroy_workqueue(fl2000_dev->stream_work_queue);
	fl2000_stream_put_buffers(fl2000_dev);
}

static void fl2000_stream_data_completion(struct urb *urb)
{
	struct fl2000_stream_buf *cur_sb = urb->context;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000 *fl2000_dev = cur_sb->parent;

	if (fl2000_dev) {
		spin_lock_irq(&fl2000_dev->list_lock);
		cur_sb->in_flight--;
		/* Move back to render_list if completed */
		if (!cur_sb->in_flight) {
			list_move_tail(&cur_sb->list, &fl2000_dev->render_list);
		}
		spin_unlock(&fl2000_dev->list_lock);
		drm_crtc_handle_vblank(&fl2000_dev->pipe.crtc);

		/* Kick transmit workqueue */
		up(&fl2000_dev->stream_work_sem);

		fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	}

	usb_free_urb(urb);
}

/* TODO: convert to tasklet */
static void fl2000_stream_work(struct work_struct *work)
{
	int ret;
	struct fl2000 *fl2000_dev = container_of(work, struct fl2000, stream_work);
	struct usb_device *usb_dev = fl2000_dev->usb_dev;
	struct fl2000_stream_buf *cur_sb;
	struct urb *data_urb;

	while (fl2000_dev->enabled) {
		ret = down_interruptible(&fl2000_dev->stream_work_sem);
		if (ret) {
			dev_err(&usb_dev->dev, "Work interrupt error %d", ret);
			break;
		}

		spin_lock_irq(&fl2000_dev->list_lock);

		/* If no buffers are available for immediate transmission - then copy latest
		 * transmission data
		 */
		if (list_empty(&fl2000_dev->transmit_list)) {
			if (list_empty(&fl2000_dev->wait_list))
				cur_sb = list_last_entry(&fl2000_dev->render_list,
						          struct fl2000_stream_buf, list);
			else
				cur_sb = list_last_entry(&fl2000_dev->wait_list,
						          struct fl2000_stream_buf, list);
		} else {
			cur_sb = list_first_entry(&fl2000_dev->transmit_list, struct fl2000_stream_buf,
						  list);
		}

		/* Don't send wrong size buffers */
		if (cur_sb->size != fl2000_dev->buf_size) {
			list_move_tail(&cur_sb->list, &fl2000_dev->render_list);
			spin_unlock(&fl2000_dev->list_lock);
			up(&fl2000_dev->stream_work_sem);
			continue;
		}
		cur_sb->in_flight++;
		list_move_tail(&cur_sb->list, &fl2000_dev->wait_list);
		spin_unlock(&fl2000_dev->list_lock);

		data_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!data_urb) {
			dev_err(&usb_dev->dev, "Data URB allocation error");
			break;
		}

		/* Endpoint 1 bulk out. We store pointer to current stream buffer structure in
		 * transfer_buffer field of URB which is unused due to SGT
		 */
		//size_t buf_size = (stream->buf_size - 50*1024) & ~(size_t)7;
		usb_fill_bulk_urb(data_urb, usb_dev, usb_sndbulkpipe(usb_dev, 1), NULL,
				  fl2000_dev->buf_size, fl2000_stream_data_completion, cur_sb);
		data_urb->interval = 0;
		data_urb->sg = cur_sb->sgt.sgl;
		data_urb->num_sgs = cur_sb->sgt.nents;
		data_urb->transfer_flags |= URB_ZERO_PACKET;

		usb_anchor_urb(data_urb, &fl2000_dev->anchor);
		ret = fl2000_submit_urb(data_urb);
		if (ret) {
			dev_err(&usb_dev->dev, "Data URB error %d", ret);
			usb_unanchor_urb(data_urb);
			usb_free_urb(data_urb);
			dev_info(&usb_dev->dev, "USB free urb");
			fl2000_dev->enabled = false;
			dev_info(&usb_dev->dev, "Stream false");
		}
	}
	dev_info(&usb_dev->dev, "Work ended");
}

static void fl2000_xrgb888_to_rgb888_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int x, xx = 0;

	for (x = 0; x < pixels; x++) {
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x000000FF) >> 0;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x0000FF00) >> 8;
		dbuf[xx++ ^ 4] = (sbuf[x] & 0x00FF0000) >> 16;
	}
}

static void fl2000_xrgb888_to_rgb565_line(u16 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int x;

	for (x = 0; x < pixels; x++) {
		u16 val565 = ((sbuf[x] & 0x00F80000) >> 8) | ((sbuf[x] & 0x0000FC00) >> 5) |
			     ((sbuf[x] & 0x000000F8) >> 3);
		dbuf[x ^ 2] = val565;
	}
}

static void fl2000_xrgb888_to_rgb332_line(u8 *dbuf, u32 *sbuf, u32 pixels)
{
	unsigned int x;

	for (x = 0; x < pixels; x++) {
		u8 val332 = ((sbuf[x] & 0x00e00000) >> 16) | ((sbuf[x] & 0x0000e000) >> 11) |
			     ((sbuf[x] & 0x000000c0) >> 6);
		dbuf[x ^ 4] = val332;
	}
}

void fl2000_stream_compress(struct fl2000 *fl2000_dev, void *src, unsigned int height,
			    unsigned int width, unsigned int pitch)
{
	struct fl2000_stream_buf *cur_sb;
	unsigned int y;
	void *dst;
	u32 dst_line_len;
	
	//BUG_ON(list_empty(&stream->render_list));

	spin_lock_irq(&fl2000_dev->list_lock);

	/* Drop frames if sending frames too fast */
	if (list_empty(&fl2000_dev->render_list))
		goto list_empty;

	cur_sb = list_first_entry(&fl2000_dev->render_list, struct fl2000_stream_buf, list);

	/* Reallocate buffers which are the wrong size */
	if (cur_sb->size != fl2000_dev->buf_size) {
		list_del(&cur_sb->list);
		fl2000_free_sb(cur_sb);

		cur_sb = fl2000_alloc_sb(fl2000_dev, fl2000_dev->buf_size);
		list_add(&cur_sb->list, &fl2000_dev->render_list);
	}
	dst = cur_sb->vaddr;
	dst_line_len = width * fl2000_dev->bytes_pix;
	
	for (y = 0; y < height; y++) {
		switch (fl2000_dev->bytes_pix) {
		case 1:
			fl2000_xrgb888_to_rgb332_line(dst, src, width);
			break;
		case 2:
			fl2000_xrgb888_to_rgb565_line(dst, src, width);
			break;
		case 3:
			fl2000_xrgb888_to_rgb888_line(dst, src, width);
			break;
		default: /* Shouldn't happen */
			break;
		}
		src += pitch;
		dst += dst_line_len;
	}
	list_move_tail(&cur_sb->list, &fl2000_dev->transmit_list);

list_empty:
	spin_unlock(&fl2000_dev->list_lock);
}

int fl2000_stream_mode_set(struct fl2000 *fl2000_dev, int pixels, u32 bytes_pix)
{
	size_t size;

	/* Round buffer size up to multiple of 8 to meet HW expectations */
	size = (pixels * bytes_pix + 7) & ~(size_t)7;

	fl2000_dev->bytes_pix = bytes_pix;

	spin_lock_irq(&fl2000_dev->list_lock);
	/* Queue buffers */
	if (list_empty(&fl2000_dev->render_list) && list_empty(&fl2000_dev->wait_list) && list_empty(&fl2000_dev->transmit_list)) {
		fl2000_stream_get_buffers(fl2000_dev, size);
	}
	fl2000_dev->buf_size = size;
	spin_unlock_irq(&fl2000_dev->list_lock);

	return 0;
}

int fl2000_stream_enable(struct fl2000 *fl2000_dev)
{
	int i;

	BUG_ON(list_empty(&fl2000_dev->transmit_list));

	sema_init(&fl2000_dev->stream_work_sem, 0);
	fl2000_dev->enabled = true;
	queue_work(fl2000_dev->stream_work_queue, &fl2000_dev->stream_work);

	/* Kick transmit workqueue with minimum buffers submitted */
	for (i = 0; i < FL2000_SB_MIN; i++)
		up(&fl2000_dev->stream_work_sem);

	return 0;
}

void fl2000_stream_disable(struct fl2000 *fl2000_dev)
{
	struct fl2000_stream_buf *cur_sb;

	fl2000_dev->enabled = false;

	cancel_work_sync(&fl2000_dev->stream_work);
	drain_workqueue(fl2000_dev->stream_work_queue);

	if (!usb_wait_anchor_empty_timeout(&fl2000_dev->anchor, 1000))
		usb_kill_anchored_urbs(&fl2000_dev->anchor);

	spin_lock_irq(&fl2000_dev->list_lock);
	while (!list_empty(&fl2000_dev->transmit_list)) {
		cur_sb = list_first_entry(&fl2000_dev->transmit_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &fl2000_dev->render_list);
	}
	while (!list_empty(&fl2000_dev->wait_list)) {
		cur_sb = list_first_entry(&fl2000_dev->wait_list, struct fl2000_stream_buf, list);
		list_move_tail(&cur_sb->list, &fl2000_dev->render_list);
	}
	spin_unlock(&fl2000_dev->list_lock);
}

/**
 * fl2000_stream_create() - streaming processing context creation
 * @interface:	streaming transfers interface
 *
 * This function is called only on Streaming interface probe
 *
 * It shall not initiate any USB transfers. URB is not allocated here because we do not know the
 * stream requirements yet.
 *
 * Return: Operation result
 */
int fl2000_stream_create(struct fl2000 *fl2000_dev)
{
	int ret;
	struct usb_device *usb_dev = fl2000_dev->usb_dev;

	/* Altsetting 1 on interface 0 */
	ret = usb_set_interface(usb_dev, FL2000_USBIF_AVCONTROL, 1);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot set streaming interface for bulk transfers");
		return ret;
	}

	INIT_WORK(&fl2000_dev->stream_work, &fl2000_stream_work);
	INIT_LIST_HEAD(&fl2000_dev->render_list);
	INIT_LIST_HEAD(&fl2000_dev->transmit_list);
	INIT_LIST_HEAD(&fl2000_dev->wait_list);
	spin_lock_init(&fl2000_dev->list_lock);
	init_usb_anchor(&fl2000_dev->anchor);
	sema_init(&fl2000_dev->stream_work_sem, 0);

	fl2000_dev->stream_work_queue = create_workqueue("fl2000_stream");
	if (!fl2000_dev->stream_work_queue) {
		dev_err(&usb_dev->dev, "Allocate streaming workqueue failed");
		fl2000_stream_release(fl2000_dev);
		return -ENOMEM;
	}
	return 0;
}