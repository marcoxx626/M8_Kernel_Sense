/* 
 * Emagic EMI 2|6 usb audio interface firmware loader.
 * Copyright (C) 2002
 * 	Tapio Laxström (tapio.laxstrom@iptime.fi)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, as published by
 * the Free Software Foundation, version 2.
 * 
 * emi26.c,v 1.13 2002/03/08 13:10:26 tapio Exp
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/ihex.h>

#define EMI26_VENDOR_ID 		0x086a  
#define EMI26_PRODUCT_ID		0x0100	
#define EMI26B_PRODUCT_ID		0x0102	

#define ANCHOR_LOAD_INTERNAL	0xA0	
#define ANCHOR_LOAD_EXTERNAL	0xA3	
#define ANCHOR_LOAD_FPGA	0xA5	
#define MAX_INTERNAL_ADDRESS	0x1B3F	
#define CPUCS_REG		0x7F92   
#define INTERNAL_RAM(address)   (address <= MAX_INTERNAL_ADDRESS)

static int emi26_writememory( struct usb_device *dev, int address,
			      const unsigned char *data, int length,
			      __u8 bRequest);
static int emi26_set_reset(struct usb_device *dev, unsigned char reset_bit);
static int emi26_load_firmware (struct usb_device *dev);
static int emi26_probe(struct usb_interface *intf, const struct usb_device_id *id);
static void emi26_disconnect(struct usb_interface *intf);

static int emi26_writememory (struct usb_device *dev, int address,
			      const unsigned char *data, int length,
			      __u8 request)
{
	int result;
	unsigned char *buffer =  kmemdup(data, length, GFP_KERNEL);

	if (!buffer) {
		dev_err(&dev->dev, "kmalloc(%d) failed.\n", length);
		return -ENOMEM;
	}
	/* Note: usb_control_msg returns negative value on error or length of the
	 * 		 data that was written! */
	result = usb_control_msg (dev, usb_sndctrlpipe(dev, 0), request, 0x40, address, 0, buffer, length, 300);
	kfree (buffer);
	return result;
}

static int emi26_set_reset (struct usb_device *dev, unsigned char reset_bit)
{
	int response;
	dev_info(&dev->dev, "%s - %d\n", __func__, reset_bit);
	
	response = emi26_writememory (dev, CPUCS_REG, &reset_bit, 1, 0xa0);
	if (response < 0) {
		dev_err(&dev->dev, "set_reset (%d) failed\n", reset_bit);
	}
	return response;
}

#define FW_LOAD_SIZE		1023

static int emi26_load_firmware (struct usb_device *dev)
{
	const struct firmware *loader_fw = NULL;
	const struct firmware *bitstream_fw = NULL;
	const struct firmware *firmware_fw = NULL;
	const struct ihex_binrec *rec;
	int err;
	int i;
	__u32 addr;	
	__u8 *buf;

	buf = kmalloc(FW_LOAD_SIZE, GFP_KERNEL);
	if (!buf) {
		dev_err(&dev->dev, "%s - error loading firmware: error = %d\n",
			__func__, -ENOMEM);
		err = -ENOMEM;
		goto wraperr;
	}

	err = request_ihex_firmware(&loader_fw, "emi26/loader.fw", &dev->dev);
	if (err)
		goto nofw;

	err = request_ihex_firmware(&bitstream_fw, "emi26/bitstream.fw",
				    &dev->dev);
	if (err)
		goto nofw;

	err = request_ihex_firmware(&firmware_fw, "emi26/firmware.fw",
				    &dev->dev);
	if (err) {
	nofw:
		dev_err(&dev->dev, "%s - request_firmware() failed\n",
			__func__);
		goto wraperr;
	}

	
	err = emi26_set_reset(dev,1);
	if (err < 0) {
		dev_err(&dev->dev,"%s - error loading firmware: error = %d\n",
			__func__, err);
		goto wraperr;
	}

	rec = (const struct ihex_binrec *)loader_fw->data;
	
	while (rec) {
		err = emi26_writememory(dev, be32_to_cpu(rec->addr),
					rec->data, be16_to_cpu(rec->len),
					ANCHOR_LOAD_INTERNAL);
		if (err < 0) {
			err("%s - error loading firmware: error = %d", __func__, err);
			goto wraperr;
		}
		rec = ihex_next_binrec(rec);
	}

	
	err = emi26_set_reset(dev,0);
	if (err < 0) {
		err("%s - error loading firmware: error = %d", __func__, err);
		goto wraperr;
	}
	msleep(250);	

	rec = (const struct ihex_binrec *)bitstream_fw->data;
	do {
		i = 0;
		addr = be32_to_cpu(rec->addr);

		
		while (rec && (i + be16_to_cpu(rec->len) < FW_LOAD_SIZE)) {
			memcpy(buf + i, rec->data, be16_to_cpu(rec->len));
			i += be16_to_cpu(rec->len);
			rec = ihex_next_binrec(rec);
		}
		err = emi26_writememory(dev, addr, buf, i, ANCHOR_LOAD_FPGA);
		if (err < 0) {
			err("%s - error loading firmware: error = %d", __func__, err);
			goto wraperr;
		}
	} while (rec);

	
	err = emi26_set_reset(dev,1);
	if (err < 0) {
		err("%s - error loading firmware: error = %d", __func__, err);
		goto wraperr;
	}

	
	for (rec = (const struct ihex_binrec *)loader_fw->data;
	     rec; rec = ihex_next_binrec(rec)) {
		err = emi26_writememory(dev, be32_to_cpu(rec->addr),
					rec->data, be16_to_cpu(rec->len),
					ANCHOR_LOAD_INTERNAL);
		if (err < 0) {
			err("%s - error loading firmware: error = %d", __func__, err);
			goto wraperr;
		}
	}
	msleep(250);	

	
	err = emi26_set_reset(dev,0);
	if (err < 0) {
		err("%s - error loading firmware: error = %d", __func__, err);
		goto wraperr;
	}

	

	for (rec = (const struct ihex_binrec *)firmware_fw->data;
	     rec; rec = ihex_next_binrec(rec)) {
		if (!INTERNAL_RAM(be32_to_cpu(rec->addr))) {
			err = emi26_writememory(dev, be32_to_cpu(rec->addr),
						rec->data, be16_to_cpu(rec->len),
						ANCHOR_LOAD_EXTERNAL);
			if (err < 0) {
				err("%s - error loading firmware: error = %d", __func__, err);
				goto wraperr;
			}
		}
	}
	
	
	err = emi26_set_reset(dev,1);
	if (err < 0) {
		err("%s - error loading firmware: error = %d", __func__, err);
		goto wraperr;
	}

	for (rec = (const struct ihex_binrec *)firmware_fw->data;
	     rec; rec = ihex_next_binrec(rec)) {
		if (INTERNAL_RAM(be32_to_cpu(rec->addr))) {
			err = emi26_writememory(dev, be32_to_cpu(rec->addr),
						rec->data, be16_to_cpu(rec->len),
						ANCHOR_LOAD_INTERNAL);
			if (err < 0) {
				err("%s - error loading firmware: error = %d", __func__, err);
				goto wraperr;
			}
		}
	}

	
	err = emi26_set_reset(dev,0);
	if (err < 0) {
		err("%s - error loading firmware: error = %d", __func__, err);
		goto wraperr;
	}
	msleep(250);	

	err = 1;

wraperr:
	release_firmware(loader_fw);
	release_firmware(bitstream_fw);
	release_firmware(firmware_fw);

	kfree(buf);
	return err;
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE(EMI26_VENDOR_ID, EMI26_PRODUCT_ID) },
	{ USB_DEVICE(EMI26_VENDOR_ID, EMI26B_PRODUCT_ID) },
	{ }                                             
};

MODULE_DEVICE_TABLE (usb, id_table);

static int emi26_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);

	dev_info(&intf->dev, "%s start\n", __func__);

	emi26_load_firmware(dev);

	
	return -EIO;
}

static void emi26_disconnect(struct usb_interface *intf)
{
}

static struct usb_driver emi26_driver = {
	.name		= "emi26 - firmware loader",
	.probe		= emi26_probe,
	.disconnect	= emi26_disconnect,
	.id_table	= id_table,
};

module_usb_driver(emi26_driver);

MODULE_AUTHOR("Tapio Laxström");
MODULE_DESCRIPTION("Emagic EMI 2|6 firmware loader.");
MODULE_LICENSE("GPL");

MODULE_FIRMWARE("emi26/loader.fw");
MODULE_FIRMWARE("emi26/bitstream.fw");
MODULE_FIRMWARE("emi26/firmware.fw");
