// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for the Allen & Heath Xone:DB4/DB2
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

#include "chip.h"
#include "pcm.h"
#include "midi.h"

MODULE_AUTHOR("Marcel Bierling <marcel@hackerman.art>");
MODULE_DESCRIPTION("Allen&Heath Xone:DB4/DB2 driver");
MODULE_LICENSE("GPL v2");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

int cardindex;
bool justresetting = false;

#define DRIVER_NAME "snd-usb-xonedb4"

static DEFINE_MUTEX(register_mutex);

int xonedb4_get_firmware_ver(struct xonedb4_chip *chip)
{
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x56, 0xC0, 0x0000, 0, chip->firmwarever, 15, 2000);

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
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x1, 0x02, 0x0000, 0x06, NULL, 0, 2000);
	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x1, 0x02, 0x0000, 0x05, NULL, 0, 2000);
	
	return ret;
}

int xonedb4_reset(struct xonedb4_chip *chip)
{
	dev_notice(&chip->dev->dev, "%s: Resetting device", __func__);

	int ret;

	justresetting = true;

	ret = usb_reset_device(chip->dev);
	if (ret < 0) {
		dev_err(&chip->dev->dev, "%s: Reset failed!\n", __func__);
		return ret;
	}

	return 0;
}

int xonedb4_set_samplerate(struct xonedb4_chip *chip)
{
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

	dev_dbg(&chip->dev->dev, "%s: Set hardware samplerate: %02X%02X%02X\n", __func__, chip->sampleratebytes[0], chip->sampleratebytes[1], chip->sampleratebytes[2]);

	return ret;
}

int xonedb4_get_status(struct xonedb4_chip *chip)
{
	int ret;
	
	// status
	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x49, 0xC0, 0x0000, 0, chip->status, 1, 2000);

	return ret;
}

int xonedb4_get_samplerate(struct xonedb4_chip *chip)
{
	int ret;

	// current samplerate
	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0), 0x81, 0xA2, 0x0100, 0, chip->sampleratebytes, 3, 2000);
	if (ret < 0) {
		return ret;
	}
	dev_dbg(&chip->dev->dev, "%s: Got hardware samplerate: %02X%02X%02X\n", __func__, chip->sampleratebytes[0], chip->sampleratebytes[1], chip->sampleratebytes[2]);

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
		// skip init for second interface
		return 0;
	}
	
	int i;
    int ret;
	struct usb_device *device = interface_to_usbdev(intf);

	ret = usb_set_interface(device, 0, 1);
	if (ret != 0) {
		dev_err(&device->dev, "%s: Can't set interface 0!\n", __func__);
		return -EIO;
	}

	ret = usb_set_interface(device, 1, 1);
	if (ret != 0) {
		dev_err(&device->dev, "%s: Can't set interface 1!\n", __func__);
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

		ret = xonedb4_midi_init_bulk_urbs(chip);
		if (ret < 0) {
			dev_err(&device->dev, "%s: MIDI fail!\n", __func__);
			goto err_chip_destroy;
		}
		ret = xonedb4_pcm_init_int_urbs(chip);
		if (ret < 0) {
			dev_err(&device->dev, "%s: PCM fail!\n", __func__);
			goto err_chip_destroy;
		}

		usb_set_intfdata(intf, chip);

		return 0;
	}
	
	dev_info(&device->dev, "%s: Found device: %s\n", __func__, device->product);

	mutex_lock(&register_mutex);

	/* see if the soundcard is already created */
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (enable[i]) {
			break;
		}
	}
	if (i >= SNDRV_CARDS) {
		dev_err(&device->dev, "%s: No available audio device!\n", __func__);
		ret = -ENODEV;
		goto err;
	}

	mutex_unlock(&register_mutex);
				
	ret = snd_card_new(&intf->dev, index[i], id[i], THIS_MODULE, sizeof(struct xonedb4_chip), &card);
	if (ret < 0) {
		dev_err(&device->dev, "%s: Cannot create ALSA card!\n", __func__);
		return ret;
	}

	cardindex = card->number;

	strscpy(card->driver, DRIVER_NAME, sizeof(card->driver));
	strscpy(card->shortname, device->product, sizeof(card->shortname));
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

	dev_info(&device->dev, "%s: Ploytec firmware version: 1.%d.%d\n", __func__, chip->firmwarever[2]/10, chip->firmwarever[2]%10);

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
		dev_err(&device->dev, "%s: PCM fail!\n", __func__);
		goto err_chip_destroy;
	}
	ret = xonedb4_midi_init(chip);
	if (ret < 0) {
		goto err_chip_destroy;
	}
	ret = snd_card_register(chip->card);
	if (ret < 0) {
		dev_err(&device->dev, "%s: Cannot register card!\n", __func__);
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
	if (intf->cur_altsetting->desc.bInterfaceNumber == 1) {
		// nothing created, nothing to break down
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
	xonedb4_midi_abort(chip);
	if (justresetting == false) {
		snd_card_disconnect(chip->card);
		snd_card_free_when_closed(chip->card);
	}
}

static const struct usb_device_id device_table[] = {
	// Allen&Heath Xone:DB4
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE,
		.idVendor = 0x0a4a,
		.idProduct = 0xffdb
	},
	// Allen&Heath Xone:DB2
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE,
		.idVendor = 0x0a4a,
		.idProduct = 0xffd2
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
