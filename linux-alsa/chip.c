// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for the Allen & Heath Xone:DB4
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 *
 * The driver is based on the work done in TerraTec DMX 6Fire USB
 */

#include <sound/initval.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <sound/initval.h>
#include <sound/core.h>
#include <linux/usb.h>
#include <linux/dev_printk.h>

#include "chip.h"
#include "pcm.h"
#include "midi.h"

MODULE_AUTHOR("Marcel Bierling <marcel@hackerman.art>");
MODULE_DESCRIPTION("Allen&Heath Xone:DB4 driver");
MODULE_LICENSE("GPL v2");

// 0.2 = ??? ASYNC
// 0.3 = MIDI IN BULK
// 0.5 = PCM OUT BULK
// 1.6 = PCM IN BULK

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

int cardindex;
bool justresetting = false;

#define DRIVER_NAME "snd-usb-xonedb4"
#define CARD_NAME "xonedb4"

static DEFINE_MUTEX(register_mutex);

int xonedb4_get_firmware_ver(struct xonedb4_chip *chip)
{
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x56, 0xC0, 0x0000, 0, chip->firmwarever, 15, 2000);

	dev_notice(&chip->dev->dev, "firmware: %02X%02X%02X\n", chip->firmwarever[0], chip->firmwarever[1], chip->firmwarever[2]);

	return ret;
}

int xonedb4_send_allgood(struct xonedb4_chip *chip)
{
	int ret;
	
	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0), 0x49, 0x40, 0xFFB2, 0x0000, NULL, 0, 2000);

	return ret;
}

int xonedb4_send_resets(struct xonedb4_chip *chip)
{
	printk("sending resets...\n");
	
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x1, 0x02, 0x0000, 0x06, NULL, 0, 2000);
	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x1, 0x02, 0x0000, 0x05, NULL, 0, 2000);
	
	return ret;
}

int xonedb4_reset(struct xonedb4_chip *chip)
{
	printk("xonedb4_reset...\n");

	int ret;

	justresetting = true;

	ret = usb_reset_device(chip->dev);
	if (ret < 0) {
		dev_err(&chip->dev->dev, "can't reset.\n");
		return ret;
	}

	return 0;
}

int xonedb4_set_samplerate(struct xonedb4_chip *chip)
{
	printk("xonedb4_set_samplerate...\n");

	int ret;

	switch (chip->alsarate)
	{
		case 0:
			chip->sampleratebytes[0] = 0x44;
			chip->sampleratebytes[1] = 0xAC;
			chip->sampleratebytes[2] = 0x00;
			break;
		case 1:
			chip->sampleratebytes[0] = 0x80;
			chip->sampleratebytes[1] = 0xBB;
			chip->sampleratebytes[2] = 0x00;
			break;
		case 2:
			chip->sampleratebytes[0] = 0x88;
			chip->sampleratebytes[1] = 0x58;
			chip->sampleratebytes[2] = 0x01;
			break;
		case 3:
			chip->sampleratebytes[0] = 0x00;
			chip->sampleratebytes[1] = 0x77;
			chip->sampleratebytes[2] = 0x01;
			break;
		default:
			// unsupported sample rate
			return -1;
	}
	
	// set samplerate
	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0), 0x01, 0x22, 0x0100, 0x0086, chip->sampleratebytes, 3, 2000);
	if (ret < 0) {
		return ret;
	}
	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0), 0x01, 0x22, 0x0100, 0x0005, chip->sampleratebytes, 3, 2000);
	if (ret < 0) {
		return ret;
	}
	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0), 0x01, 0x22, 0x0100, 0x0086, chip->sampleratebytes, 3, 2000);
	if (ret < 0) {
		return ret;
	}
	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0), 0x01, 0x22, 0x0100, 0x0005, chip->sampleratebytes, 3, 2000);
	if (ret < 0) {
		return ret;
	}
	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0), 0x01, 0x22, 0x0100, 0x0086, chip->sampleratebytes, 3, 2000);
	if (ret < 0) {
		return ret;
	}

	chip->devicerate = chip->alsarate;

	dev_notice(&chip->dev->dev, "set samplerate: %02X%02X%02X\n", chip->sampleratebytes[0], chip->sampleratebytes[1], chip->sampleratebytes[2]);

	return ret;
}

int xonedb4_get_status(struct xonedb4_chip *chip)
{
	int ret;
	
	// status
	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x49, 0xC0, 0x0000, 0, chip->status, 1, 2000);

	dev_notice(&chip->dev->dev, "status: %02X\n", chip->status[0]);

	return ret;
}

int xonedb4_get_samplerate(struct xonedb4_chip *chip)
{
	printk("xonedb4_get_samplerate...\n");

	int ret;

	// current samplerate
	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x81, 0xA2, 0x0100, 0, chip->sampleratebytes, 3, 2000);
	if (ret < 0) {
		return ret;
	}
	dev_notice(&chip->dev->dev, "current samplerate: %02X%02X%02X\n", chip->sampleratebytes[0], chip->sampleratebytes[1], chip->sampleratebytes[2]);

	if ((chip->sampleratebytes[0] == 0x44) && (chip->sampleratebytes[1] == 0xAC) && (chip->sampleratebytes[2] == 0x00)) {
		chip->devicerate = 0;
	} else if ((chip->sampleratebytes[0] == 0x80) && (chip->sampleratebytes[1] == 0xBB) && (chip->sampleratebytes[2] == 0x00)) {
		chip->devicerate = 1;
	} else if ((chip->sampleratebytes[0] == 0x88) && (chip->sampleratebytes[1] == 0x58) && (chip->sampleratebytes[2] == 0x01)) {
		chip->devicerate = 2;
	} else if ((chip->sampleratebytes[0] == 0x00) && (chip->sampleratebytes[1] == 0x77) && (chip->sampleratebytes[2] == 0x01)) {
		chip->devicerate = 3;
	} else {
		return -1;
	}

	return ret;
}

/* In case of the Xone DB4, this actually gets called twice as the device announced 2 interfaces */
static int xonedb4_probe(struct usb_interface *intf, const struct usb_device_id *usb_id)
{
	struct snd_card *card = NULL;
	struct xonedb4_chip *chip = NULL;

	if (intf->cur_altsetting->desc.bInterfaceNumber == 1)
	{
		// int ret;
		
		printk(KERN_ALERT "nope\n");
		/*
		dev_err(&intf->dev, "blah blah blah %d\n", cardindex);

		card = snd_card_ref(cardindex);
		chip = card->private_data;

		usb_set_intfdata(intf, chip);
		*/
		return 0;
	}
	
	printk(KERN_ALERT "XONEDB4 CONNECT\n");

	int i;
    int ret;
	struct usb_device *device = interface_to_usbdev(intf);
    	
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

	if (justresetting == true) {
		card = snd_card_ref(cardindex);
		chip = card->private_data;
		chip->card = card;
		chip->dev = device;
		justresetting = false;
	
		// get status
		ret = xonedb4_get_status(chip);
		if (ret < 0) {
			goto err_chip_destroy;
		}
		// get samplerate
		ret = xonedb4_get_samplerate(chip);
		if (ret < 0) {
			goto err_chip_destroy;
		}
		// set samplerate
		ret = xonedb4_set_samplerate(chip);
		if (ret < 0) {
			goto err_chip_destroy;
		}
		// get samplerate
		ret = xonedb4_get_samplerate(chip);
		if (ret < 0) {
			goto err_chip_destroy;
		}
		// get status
		ret = xonedb4_get_status(chip);
		if (ret < 0) {
			goto err_chip_destroy;
		}
		// send allgood
		ret = xonedb4_send_allgood(chip);
		if (ret < 0) {
			goto err_chip_destroy;
		}

		ret = xonedb4_pcm_init_bulk_urbs(chip);
		if (ret < 0) {
			dev_err(&device->dev, "pcm out fail " CARD_NAME " card\n");
			goto err_chip_destroy;
		}

		usb_set_intfdata(intf, chip);

		return 0;
	}
	
	mutex_lock(&register_mutex);

	/* see if the soundcard is already created */
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (enable[i]) {
			break;
		}
	}
	if (i >= SNDRV_CARDS) {
		dev_err(&device->dev, "no available " CARD_NAME " audio device\n");
		ret = -ENODEV;
		goto err;
	}

	mutex_unlock(&register_mutex);
				
	ret = snd_card_new(&intf->dev, index[i], id[i], THIS_MODULE, sizeof(struct xonedb4_chip), &card);
	if (ret < 0) {
		dev_err(&device->dev, "cannot create alsa card.\n");
		return ret;
	}

	cardindex = card->number;

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, "Allen&Heath Xone:DB4", sizeof(card->shortname));
	sprintf(card->longname, "%s at %d:%d", card->shortname, device->bus->busnum, device->devnum);

	chip = card->private_data;
	chip->card = card;
	chip->dev = device;

	chip->alsarate = 3;

	/* get firmware */
	ret = xonedb4_get_firmware_ver(chip);
	if (ret < 0) {
		goto err;
	}
	// get status
	ret = xonedb4_get_status(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}
	// get samplerate
	ret = xonedb4_get_samplerate(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}
	// set samplerate
	ret = xonedb4_set_samplerate(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}
	// get samplerate
	ret = xonedb4_get_samplerate(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}
	// get status
	ret = xonedb4_get_status(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}
	// send allgood
	ret = xonedb4_send_allgood(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}

	ret = xonedb4_pcm_init(chip);
	if (ret < 0) {
		dev_err(&device->dev, "pcm out fail " CARD_NAME " card\n");
		goto err_chip_destroy;
	}
	/*
	ret = xonedb4_midi_init(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}
	*/
	ret = snd_card_register(chip->card);
	if (ret < 0) {
		dev_err(&device->dev, "cannot register " CARD_NAME " card\n");
		goto err_chip_destroy;
	}

	usb_set_intfdata(intf, chip);
	return 0;

err_chip_destroy:
	snd_card_free(chip->card);
err:
	mutex_unlock(&register_mutex);
	return ret;
}

static void xonedb4_disconnect(struct usb_interface *intf)
{
    printk(KERN_ALERT "XONEDB4 DISCONNECT\n");

	if (intf->cur_altsetting->desc.bInterfaceNumber == 1) {
		printk(KERN_ALERT "NOTHING TO BREAK DOWN ON INTERFACE 1\n");
		return;
	}

	struct xonedb4_chip *chip;
	struct snd_card *card;

	chip = usb_get_intfdata(intf);
	if (!chip)
		return;

	card = chip->card;

	/* Make sure that the userspace cannot create new request */
	xonedb4_pcm_abort(chip);
	// xonedb4_midi_abort(chip);
	if (justresetting == false) {
		snd_card_disconnect(chip->card);
		snd_card_free_when_closed(chip->card);
	}
}

static const struct usb_device_id device_table[] = {
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE,
		.idVendor = 0x0a4a,
		.idProduct = 0x0ffdb
	},
	{}
};

MODULE_DEVICE_TABLE(usb, device_table);

static struct usb_driver snd_xonedb4_driver = {
	.name 		= "snd-usb-xonedb4",
	.probe 		= xonedb4_probe,
    .disconnect = xonedb4_disconnect,
    .id_table   = device_table,
};

module_usb_driver(snd_xonedb4_driver);
