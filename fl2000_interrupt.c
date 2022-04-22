// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define INTR_BUFSIZE 1

static void fl2000_intr_work(struct work_struct *work)
{
	int event;
	struct fl2000 *fl2000_dev =
		container_of(work, struct fl2000, intr_work);

	event = fl2000_check_interrupt(fl2000_dev->usb_dev);
	if (event) {
		drm_kms_helper_hotplug_event(&fl2000_dev->drm);
		//drm_helper_hpd_irq_event(fl2000_dev->drm);
	}
}

void fl2000_intr_release(struct fl2000 *fl2000_dev)
{
	usb_poison_urb(fl2000_dev->intr_urb);
	cancel_work_sync(&fl2000_dev->intr_work);
	destroy_workqueue(fl2000_dev->intr_work_queue);
	usb_free_coherent(fl2000_dev->usb_dev, INTR_BUFSIZE,
			  fl2000_dev->intr_buf, fl2000_dev->transfer_dma);
	usb_free_urb(fl2000_dev->intr_urb);
}

static void fl2000_intr_completion(struct urb *urb)
{
	int ret;
	struct usb_device *usb_dev = urb->dev;
	struct fl2000 *fl2000_dev = urb->context;

	ret = fl2000_urb_status(usb_dev, urb->status, urb->pipe);
	if (ret) {
		dev_err(&usb_dev->dev, "Stopping interrupts");
		return;
	}

	/* This possibly involves reading I2C registers, etc. so better to schedule a work queue */
	queue_work(fl2000_dev->intr_work_queue, &fl2000_dev->intr_work);

	/* For interrupt URBs, as part of successful URB submission urb->interval is modified to
	 * reflect the actual transfer period used, so we need to restore it
	 */
	urb->interval = fl2000_dev->poll_interval;
	urb->start_frame = -1;

	/* Restart urb */
	ret = fl2000_submit_urb(urb);
	if (ret) {
		dev_err(&usb_dev->dev, "URB submission failed (%d)", ret);
		/* TODO: Signal fault to system and start shutdown of usb_dev */
	}
}

/**
 * fl2000_intr_create() - interrupt processing context creation
 * @interface:	USB interrupt transfers interface
 *
 * This function is called only on Interrupt interface probe
 *
 * Function initiates USB Interrupt transfers
 *
 * Return: Operation result
 */
int fl2000_intr_create(struct fl2000 *fl2000_dev)
{
	int ret;
	struct usb_device *usb_dev = fl2000_dev->usb_dev;
	struct usb_endpoint_descriptor *desc;
	struct usb_interface *interface =
		fl2000_dev->intf[FL2000_USBIF_INTERRUPT];

	/* There's only one altsetting (#0) and one endpoint (#3) in the interrupt interface (#2)
	 * but lets try and "find" it anyway
	 */
	ret = usb_find_int_in_endpoint(interface->cur_altsetting, &desc);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot find interrupt endpoint");
		return ret;
	}

	fl2000_dev->poll_interval = desc->bInterval;
	INIT_WORK(&fl2000_dev->intr_work, &fl2000_intr_work);

	fl2000_dev->intr_urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!fl2000_dev->intr_urb) {
		dev_err(&usb_dev->dev, "Allocate interrupt URB failed");
		fl2000_intr_release(fl2000_dev);
		return -ENOMEM;
	}

	fl2000_dev->intr_buf = usb_alloc_coherent(
		usb_dev, INTR_BUFSIZE, GFP_KERNEL, &fl2000_dev->transfer_dma);
	if (!fl2000_dev->intr_buf) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt data");
		fl2000_intr_release(fl2000_dev);
		return -ENOMEM;
	}

	fl2000_dev->intr_work_queue = create_workqueue("fl2000_interrupt");
	if (!fl2000_dev->intr_work_queue) {
		dev_err(&usb_dev->dev, "Create interrupt workqueue failed");
		fl2000_intr_release(fl2000_dev);
		return -ENOMEM;
	}

	/* Interrupt URB configuration is static, including allocated buffer */
	usb_fill_int_urb(fl2000_dev->intr_urb, usb_dev,
			 usb_rcvintpipe(usb_dev, 3), fl2000_dev->intr_buf,
			 INTR_BUFSIZE, fl2000_intr_completion, fl2000_dev,
			 fl2000_dev->poll_interval);
	fl2000_dev->intr_urb->transfer_dma = fl2000_dev->transfer_dma;
	fl2000_dev->intr_urb->transfer_flags |=
		URB_NO_TRANSFER_DMA_MAP; /* use urb->transfer_dma */

	/* Start checking for interrupts */
	ret = usb_submit_urb(fl2000_dev->intr_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&usb_dev->dev, "URB submission failed");
		fl2000_intr_release(fl2000_dev);
		return ret;
	}

	return 0;
}