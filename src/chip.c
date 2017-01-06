/*
 * Linux driver for Sinn7 Status 24|96 compatible devices
 *
 * Copyright 2016-2017 (C) Marc Streckfuß
 *
 * Authors:
 *           Marc Streckfuß <marc.streckfuss@gmail.com>
 *
 * The driver is based on the work done in the M2Tech hiFace Driver which
 * in turn is based on TerraTec DMX 6Fire USB.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include "linux/usb.h"
#include <sound/initval.h>

#include "chip.h"
#include "pcm.h"

MODULE_AUTHOR("Marc Streckfuß <marc.streckfuss@gmail.com>");
MODULE_DESCRIPTION("Sinn7 Status 24|96 usb audio driver");
MODULE_LICENSE("GPL v2");
MODULE_SUPPORTED_DEVICE("{{Sinn7,Status 24|96}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX; /* Index 0-max */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR; /* Id for card */
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP; /* Enable this card */

#define DRIVER_NAME "snd-usb-sinn7"
#define CARD_NAME "Status 24|96"

/* Timeout is set to a high value, could probably be reduced. Need more tests */
#define USB_TIMEOUT 1000

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");

static DEFINE_MUTEX(register_mutex);

struct sinn7_vendor_quirk {
	const char *device_name;
	u8 extra_freq;
};

static int sinn7_chip_create(struct usb_interface *intf,
			      struct usb_device *device, int idx,
			      const struct sinn7_vendor_quirk *quirk,
			      struct sinn7_chip **rchip)
{
	struct snd_card *card = NULL;
	struct sinn7_chip *chip;
	int ret;
	int len;

	*rchip = NULL;

	/* if we are here, card can be registered in alsa. */
	ret = snd_card_new(&intf->dev, index[idx], id[idx], THIS_MODULE,
			   sizeof(*chip), &card);
	if (ret < 0) {
		dev_err(&device->dev, "cannot create alsa card.\n");
		return ret;
	}

	strlcpy(card->driver, DRIVER_NAME, sizeof(card->driver));

	if (quirk && quirk->device_name)
		strlcpy(card->shortname, quirk->device_name, sizeof(card->shortname));
	else
		strlcpy(card->shortname, "Sinn7 Status 24|96", sizeof(card->shortname));

	strlcat(card->longname, card->shortname, sizeof(card->longname));
	len = strlcat(card->longname, " at ", sizeof(card->longname));
	if (len < sizeof(card->longname))
		usb_make_path(device, card->longname + len,
			      sizeof(card->longname) - len);

	chip = card->private_data;
	chip->dev = device;
	chip->card = card;

	*rchip = chip;
	return 0;
}

static int sinn7_chip_probe(struct usb_interface *intf,
			     const struct usb_device_id *usb_id)
{
	const struct sinn7_vendor_quirk *quirk = (struct sinn7_vendor_quirk *)usb_id->driver_info;
	int ret;
	int i;
	struct sinn7_chip *chip;
	struct usb_device *device = interface_to_usbdev(intf);
	int ifnum;
	
	ifnum = intf->altsetting[0].desc.bInterfaceNumber;
	if (ifnum != 0) {
		dev_warn(&device->dev, "invalid interface number %d", ifnum);
		return -EINVAL;
	}

	//ret = usb_driver_set_configuration(device, 1);
	void *buffer = kzalloc(15, GFP_KERNEL);
	ret = usb_control_msg(device, usb_rcvctrlpipe(device, 0), 0x56, 0xC0, 0x0, 0x0, buffer, 15, USB_TIMEOUT);
	
	uint8_t *intBuffer = (uint8_t*)buffer; /* to reduce the cast count */
	
	if (intBuffer[0] != 0x31 || intBuffer[1] != 0x01 || intBuffer[2] != 0x08) {
	  dev_err(&device->dev, "received unexpected answer. possibly the firmware version changed? received: %x %x %x, expected: 0x31 0x01 0x08\n", intBuffer[0], intBuffer[1], intBuffer[2]);
	  kfree(buffer);
	  return -ENODEV; /* TODO: Find suitable error */
// 	} else {
	  kfree(buffer);
	}
	
	ret = usb_set_interface(device, 0, 1);
	if (ret != 0) {
	  dev_err(&device->dev, "can't set interface 0 for " CARD_NAME " device.\n");
	  return -EIO;
	}
	
	ret = usb_set_interface(device, 1, 1);
	if (ret != 0) {
	  dev_err(&device->dev, "can't set interface 1 for " CARD_NAME " device.\n");
	  return -EIO;
	}
	
	buffer = kzalloc(1, GFP_KERNEL);
	intBuffer = (uint8_t*)buffer;
	
	ret = usb_control_msg(device, usb_rcvctrlpipe(device, 0), 0x49, 0xC0, 0x0, 0X0, buffer, 1, USB_TIMEOUT);
	
	if (intBuffer[0] != 0x32 && intBuffer[0] != 0x12) {
	  dev_err(&device->dev, "received unexpected answer. possibly the firmware has been changed? received: %x, expected: 0x32 or 0x12\n", intBuffer[0]);
	  kfree(buffer);
	  return -ENODEV; /* TODO: Find suitable error */
	} else {
	  kfree(buffer);
	}
	
	buffer = kzalloc(3, GFP_KERNEL);
	intBuffer = (uint8_t*)buffer;
	
	ret = usb_control_msg(device, usb_rcvctrlpipe(device, 0), 0x81, 0xA2, 0x100, 0x0, buffer, 3, USB_TIMEOUT);
	
	if (intBuffer[0] != 0x44 || intBuffer[1] != 0xAC || intBuffer[2] != 0x00) {
	  dev_err(&device->dev, "received unexpected answer. possibly the firmware has been changed? received: %x %x %x, expected: 0x44 0xAC 0x00\n", intBuffer[0], intBuffer[1], intBuffer[2]);
	  kfree(buffer);
	  return -ENODEV; /* TODO: Find suitable error */
	} /* Intentionally no else!! */
	
	ret = usb_control_msg(device, usb_sndctrlpipe(device, 0), 0x1, 0x22, 0x100, 0x86, buffer, 3, USB_TIMEOUT);
	ret = usb_control_msg(device, usb_sndctrlpipe(device, 0), 0x1, 0x22, 0x100, 0x05, buffer, 3, USB_TIMEOUT);
	
	// REUSE: kfree(buffer);
	ret = usb_control_msg(device, usb_rcvctrlpipe(device, 0), 0x81, 0xA2, 0x100, 0x86, buffer, 3, USB_TIMEOUT);
	if (intBuffer[0] != 0x44 || intBuffer[1] != 0xAC || intBuffer[2] != 0x00) {
	  dev_err(&device->dev, "received unexpected answer. possibly the firmware has been changed? received: %x %x %x, expected: 0x44 0xAC 0x00\n", intBuffer[0], intBuffer[1], intBuffer[2]);
	  kfree(buffer);
	  return -ENODEV; /* TODO: Find suitable error */
	}
	
	ret = usb_control_msg(device, usb_rcvctrlpipe(device, 0), 0x49, 0xC0, 0x0, 0x0, buffer, 1, USB_TIMEOUT);
	if (intBuffer[0] != 0x32 && intBuffer[0] != 0x12) {
	  dev_err(&device->dev, "received unexpected answer. possibly the firmware has been changed? received: %x, expected: 0x32 or 0x12\n", intBuffer[0]);
	  kfree(buffer);
	  return -ENODEV; /* TODO: Find suitable error */
	}
	
	ret = usb_control_msg(device, usb_sndctrlpipe(device, 0), 0x49, 0x40, 0x32, 0x0, buffer, 0, USB_TIMEOUT);
	
	/* halt the endpoints */
	ret = usb_control_msg(device, usb_sndctrlpipe(device, 0), 0x01, 0x02, 0x0, 0x86, buffer, 0, USB_TIMEOUT);
	ret = usb_control_msg(device, usb_sndctrlpipe(device, 0), 0x01, 0x02, 0x0, 0x05, buffer, 0, USB_TIMEOUT);
	

	/* check whether the card is already registered */
	chip = NULL;
	mutex_lock(&register_mutex);

	for (i = 0; i < SNDRV_CARDS; i++)
		if (enable[i])
			break;

	if (i >= SNDRV_CARDS) {
		dev_err(&device->dev, "no available " CARD_NAME " audio device\n");
		ret = -ENODEV;
		goto err;
	}

	ret = sinn7_chip_create(intf, device, i, quirk, &chip);
	if (ret < 0)
		goto err;

	ret = sinn7_pcm_init(chip, quirk ? quirk->extra_freq : 0);
	if (ret < 0)
		goto err_chip_destroy;

	ret = snd_card_register(chip->card);
	if (ret < 0) {
		dev_err(&device->dev, "cannot register " CARD_NAME " card\n");
		goto err_chip_destroy;
	}

	mutex_unlock(&register_mutex);

	usb_set_intfdata(intf, chip);
	return 0;

err_chip_destroy:
	snd_card_free(chip->card);
err:
	mutex_unlock(&register_mutex);
	return ret;
}

static void sinn7_chip_disconnect(struct usb_interface *intf)
{
	struct sinn7_chip *chip;
	struct snd_card *card;

	chip = usb_get_intfdata(intf);
	if (!chip)
		return;

	card = chip->card;

	/* Make sure that the userspace cannot create new request */
	snd_card_disconnect(card);

	sinn7_pcm_abort(chip);
	snd_card_free_when_closed(card);
}

static const struct usb_device_id device_table[] = {
	{
		USB_DEVICE_INTERFACE_NUMBER(0x200c, 0x1006, 0),
		.driver_info = (unsigned long)&(const struct sinn7_vendor_quirk) {
			.device_name = "Status 24|96",
		},
	},
	{}
};

MODULE_DEVICE_TABLE(usb, device_table);

static struct usb_driver sinn7_usb_driver = {
	.name = DRIVER_NAME,
	.probe = sinn7_chip_probe,
	.disconnect = sinn7_chip_disconnect,
	.id_table = device_table,
};

module_usb_driver(sinn7_usb_driver);
