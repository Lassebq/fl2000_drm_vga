/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#ifndef __FL2000_DRM_H__
#define __FL2000_DRM_H__

#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <linux/usb.h>

#include <drm/drm_modes.h>
#include <drm/drm_simple_kms_helper.h>

#include "fl2000_registers.h"

/* Known USB interfaces of FL2000 */
enum fl2000_interface {
	FL2000_USBIF_AVCONTROL = 0,
	FL2000_USBIF_STREAMING = 1,
	FL2000_USBIF_INTERRUPT = 2,
};

/**
 * fl2000_add_bitmask - Set bitmask for structure field
 *
 * @__mask: Variable to set mask to (assumed u32)
 * @__type: Structure type to use with bitfield (assumed size equal to u32)
 * @__field: Field to set mask for in the '__type' structure
 *
 * Sets bits to 1 in '__mask' variable that correspond to field '__field' of
 * structure type '__type'. Tested only with u32 data types
 */
#define fl2000_add_bitmask(__mask, __type, __field)                            \
	({                                                                     \
		union {                                                        \
			__type __umask;                                        \
			typeof(__mask) __val;                                  \
		} __aligned(4) __data;                                         \
		__data.__umask.__field = ~0;                                   \
		(__mask) |= __data.__val;                                      \
	})

static inline int fl2000_submit_urb(struct urb *urb)
{
	int ret;
	int attempts = 10;

	do {
		ret = usb_submit_urb(urb, GFP_KERNEL);
		switch (ret) {
		case -ENXIO:
		case -ENOMEM:
			if (attempts--) {
				cond_resched();
				ret = -EAGAIN;
			}
			printk("usb submit again\n");
			break;
		default:
			break;
		}
	} while (ret == -EAGAIN);

	return ret;
}

static inline int fl2000_urb_status(struct usb_device *usb_dev, int status,
				    int pipe)
{
	int ret = status;

	switch (status) {
	/* Stalled endpoint */
	case -EPIPE:
		ret = usb_clear_halt(usb_dev, pipe);
		break;
	default:
		break;
	}

	return ret;
}

struct fl2000_timings {
	u32 hactive;
	u32 htotal;
	u32 hsync_width;
	u32 hstart;
	u32 vactive;
	u32 vtotal;
	u32 vsync_width;
	u32 vstart;
};

struct fl2000_pll {
	u32 prescaler;
	u32 multiplier;
	u32 divisor;
	u32 function;
	u32 min_ppm_err;
};

/* Devices that are independent of interfaces, created for the lifetime of USB device instance */
struct fl2000 {
	/* USB device properties */
	struct usb_device *usb_dev;
	struct usb_interface *intf[3];
	struct regmap *regmap;

	/* DDC I2C Bus */
	struct i2c_adapter *adapter;

	/* DRM driver */
	struct device *dmadev;
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	/* Framebuffer streaming */
	struct list_head render_list;
	struct list_head transmit_list;
	struct list_head wait_list;
	spinlock_t list_lock;

	size_t buf_size;
	int bytes_pix;

	struct work_struct stream_work;
	struct workqueue_struct *stream_work_queue;
	struct completion stream_complete;
	bool enabled;

	struct usb_anchor anchor;

	int print_complete;
	
	/* Interrupt handling */
	u8 poll_interval;
	struct urb *intr_urb;
	u8 *intr_buf;
	dma_addr_t transfer_dma;
	struct work_struct intr_work;
	struct workqueue_struct *intr_work_queue;
};

/* Timeout in us for I2C read/write operations */
#define I2C_RDWR_INTERVAL (200)
#define I2C_RDWR_TIMEOUT (256 * 1000)

/* Streaming transfer task */
int fl2000_stream_create(struct fl2000 *fl2000_dev);
void fl2000_stream_release(struct fl2000 *fl2000_dev);

/* Streaming interface */
int fl2000_stream_mode_set(struct fl2000 *fl2000_dev, int pixels,
			   u32 bytes_pix);
void fl2000_stream_compress(struct fl2000 *fl2000_dev, void *src,
			    unsigned int height, unsigned int width,
			    unsigned int pitch);
int fl2000_stream_enable(struct fl2000 *fl2000_dev);
void fl2000_stream_disable(struct fl2000 *fl2000_dev);

/* Interrupt polling task */
int fl2000_intr_create(struct fl2000 *fl2000_dev);
void fl2000_intr_release(struct fl2000 *fl2000_dev);

/* I2C adapter interface creation */
struct i2c_adapter *fl2000_i2c_init(struct usb_device *usb_dev);

/* I2C adapter functions */
int fl2000_i2c_read_dword(struct usb_device *usb_dev, u16 addr, u8 offset,
			  u32 *data);
int fl2000_i2c_write_dword(struct usb_device *usb_dev, u16 addr, u8 offset,
			   u32 *data);

/* Connector functions */
int fl2000_connector_init(struct fl2000 *fl2000_dev);

/* Register map creation */
struct regmap *fl2000_regmap_init(struct usb_device *usb_dev);

/* Registers interface */
int fl2000_reset(struct usb_device *usb_dev);
int fl2000_usb_magic(struct usb_device *usb_dev);
int fl2000_afe_magic(struct usb_device *usb_dev);
int fl2000_set_transfers(struct usb_device *usb_dev);
int fl2000_set_pixfmt(struct usb_device *usb_dev, u32 bytes_pix);
int fl2000_set_timings(struct usb_device *usb_dev,
		       struct fl2000_timings *timings);
int fl2000_set_pll(struct usb_device *usb_dev, struct fl2000_pll *pll);
int fl2000_enable_interrupts(struct usb_device *usb_dev);
int fl2000_check_interrupt(struct usb_device *usb_dev);
int fl2000_i2c_dword(struct usb_device *usb_dev, bool read, u16 addr, u8 offset,
		     u32 *data);

/* DRM device creation */
extern const struct drm_driver fl2000_drm_driver;
int fl2000_drm_init(struct fl2000 *fl2000_dev);
void fl2000_drm_release(struct fl2000 *fl2000_dev);

#endif /* __FL2000_DRM_H__ */
