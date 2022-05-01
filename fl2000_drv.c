// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include <drm/drm_drv.h>

#include <linux/module.h>
#include <linux/usb.h>

#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>

#include "fl2000.h"

#define USB_DRIVER_NAME "fl2000_usb"

#define USB_CLASS_AV 0x10
#define USB_SUBCLASS_AV_CONTROL 0x01
#define USB_SUBCLASS_AV_VIDEO 0x02
#define USB_SUBCLASS_AV_AUDIO 0x03

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000 0x2000

static struct usb_driver fl2000_driver;

static int fl2000_probe(struct usb_interface *interface,
			const struct usb_device_id *usb_dev_id)
{
	int ret = 0;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct usb_interface *if_stream, *if_interrupt;
	struct fl2000 *fl2000_dev;

	if (iface_num != FL2000_USBIF_AVCONTROL)
		return -ENODEV;

	if (usb_dev->speed < USB_SPEED_HIGH) {
		dev_err(&usb_dev->dev, "USB 1.1 is not supported!");
		return -ENODEV;
	}

	if (usb_dev->speed == USB_SPEED_HIGH) {
		dev_err(&usb_dev->dev, "Using USB 2.0, resolutions may be limited");
	}

	fl2000_dev = devm_drm_dev_alloc(&usb_dev->dev, &fl2000_drm_driver,
					struct fl2000, drm);
	if (IS_ERR(fl2000_dev)) {
		dev_err(&usb_dev->dev, "Cannot allocate DRM structure (%ld)",
			PTR_ERR(fl2000_dev));
		return PTR_ERR(fl2000_dev);
	}

	fl2000_dev->regmap = fl2000_regmap_init(usb_dev);
	if (IS_ERR(fl2000_dev->regmap))
		return PTR_ERR(fl2000_dev->regmap);

	fl2000_dev->adapter = fl2000_i2c_init(usb_dev);
	if (IS_ERR(fl2000_dev->adapter))
		return PTR_ERR(fl2000_dev->adapter);

	dev_set_drvdata(&usb_dev->dev, fl2000_dev);

	fl2000_dev->usb_dev = usb_dev;

	/* Claim the other interfaces */
	fl2000_dev->intf[FL2000_USBIF_AVCONTROL] = interface;

	if_stream = usb_ifnum_to_if(usb_dev, FL2000_USBIF_STREAMING);
	if (!if_stream) {
		dev_err(&usb_dev->dev, "interface %d not found",
			FL2000_USBIF_STREAMING);
		return -ENXIO;
	}
	ret = usb_driver_claim_interface(&fl2000_driver, if_stream, fl2000_dev);
	if (ret < 0) {
		ret = -EBUSY;
	}
	fl2000_dev->intf[FL2000_USBIF_STREAMING] = if_stream;

	if_interrupt = usb_ifnum_to_if(usb_dev, FL2000_USBIF_INTERRUPT);
	if (!if_interrupt) {
		dev_err(&usb_dev->dev, "interface %d not found",
			FL2000_USBIF_INTERRUPT);
		ret = -ENXIO;
		goto err_unclaim_stream_interface;
	}
	ret = usb_driver_claim_interface(&fl2000_driver, if_interrupt,
					 fl2000_dev);
	if (ret < 0) {
		ret = -EBUSY;
		goto err_unclaim_stream_interface;
	}
	fl2000_dev->intf[FL2000_USBIF_INTERRUPT] = if_interrupt;

	ret = fl2000_drm_init(fl2000_dev);
	if (ret) {
		goto err_unclaim_interrupt_interface;
	}
	return 0;

err_unclaim_interrupt_interface:
	usb_driver_release_interface(&fl2000_driver, if_interrupt);
err_unclaim_stream_interface:
	usb_driver_release_interface(&fl2000_driver, if_stream);
	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000 *fl2000_dev = dev_get_drvdata(&usb_dev->dev);

	if (!fl2000_dev)
		return;

	dev_set_drvdata(&usb_dev->dev, NULL);

	fl2000_drm_release(fl2000_dev);
}

static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000 *fl2000_dev = dev_get_drvdata(&usb_dev->dev);

	return drm_mode_config_helper_suspend(&fl2000_dev->drm);
}

static int fl2000_resume(struct usb_interface *interface)
{
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000 *fl2000_dev = dev_get_drvdata(&usb_dev->dev);

	return drm_mode_config_helper_resume(&fl2000_dev->drm);
}

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE(USB_VENDOR_FRESCO_LOGIC, USB_PRODUCT_FL2000) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

static struct usb_driver fl2000_driver = {
	.name = USB_DRIVER_NAME,
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.id_table = fl2000_id_table,
	.supports_autosuspend = false,
	.disable_hub_initiated_lpm = true,
#ifdef CONFIG_PM
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
#endif
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
