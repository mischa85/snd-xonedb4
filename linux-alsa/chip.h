#ifndef XONEDB4_CHIP_H
#define XONEDB4_CHIP_H

#include <linux/usb.h>
#include <sound/core.h>

struct pcm_runtime;

struct xonedb4_chip {
	unsigned char firmwarever[15];
	unsigned char sampleratebytes[3];
	unsigned char currentsampleratebytes[3];
	unsigned char status[1];
	unsigned int devicerate;
	unsigned int alsarate;
	struct usb_device *dev;
	struct snd_card *card;
	struct pcm_runtime *pcm;
	struct midi_runtime *midi;
};

int xonedb4_get_firmware_ver(struct xonedb4_chip *chip);
int xonedb4_reset(struct xonedb4_chip *chip);
int xonedb4_set_samplerate(struct xonedb4_chip *chip);
int xonedb4_send_allgood(struct xonedb4_chip *chip);
int xonedb4_send_resets(struct xonedb4_chip *chip);
int xonedb4_get_status(struct xonedb4_chip *chip);
int xonedb4_get_samplerate(struct xonedb4_chip *chip);
#endif /* XONEDB4_CHIP_H */
