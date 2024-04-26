#include <sound/rawmidi.h>

#include "midi.h"
#include "chip.h"

#define MIDI_IN_EP							3
#define MIDI_N_URBS							1

#define PCM_ISO_EP							2
#define ISO_BYTES_PER_FRAME					4

#define PCM_N_ISO_URBS						8
#define PCM_N_ISO_URBS_PKTS					1

#define XDB4_MIDI_PACKET_SIZE				512

uint32_t clockcounter;

struct midi_urb {
	struct xonedb4_chip *chip;
	struct urb instance;
	struct usb_anchor submitted;
	u8 *buffer;
};

/*
struct iso_urb {
	struct xonedb4_chip *chip;
	// BEGIN DO NOT SEPERATE
	struct urb instance;
	struct usb_iso_packet_descriptor packets[PCM_N_ISO_URBS];
	// END DO NOT SEPERATE
	struct usb_anchor submitted;
};
*/

enum {
	MIDI_BUFSIZE = 64
};

struct midi_runtime {
	struct xonedb4_chip *chip;
	struct snd_rawmidi *instance;

	struct snd_rawmidi_substream *in;
	bool active;

	// struct iso_urb iso_urbs[PCM_N_ISO_URBS];
    struct midi_urb midi_in_urbs[MIDI_N_URBS];

	spinlock_t in_lock;
};

/*
static void xonedb4_iso_urb_handler(struct urb *usb_urb)
{
	// printk("xonedb4_iso_urb_handler...\n");

	int ret;

	struct iso_urb *iso_urb = usb_urb->context;
	
	if (unlikely(usb_urb->status == -ENOENT ||
		     usb_urb->status == -ENODEV ||
		     usb_urb->status == -ECONNRESET ||
		     usb_urb->status == -ESHUTDOWN)) {
		goto out_fail;
	}

	clockcounter++;

	if (clockcounter == 44100) {
		dev_err(&iso_urb->chip->dev->dev, "%s: got 44100 samples\n", __func__);
		clockcounter = 0;
	}

	ret = usb_submit_urb(&iso_urb->instance, GFP_ATOMIC);
	
	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	printk("wtffffff\n");
}
*/

static void xonedb4_midi_in_urb_handler(struct urb *usb_urb)
{
	// printk("mini urb handler\n");
	
	struct midi_urb *in_urb = usb_urb->context;
	struct midi_runtime *rt = in_urb->chip->midi;

	unsigned long flags;
	int ret;

	if (unlikely(usb_urb->status == -ENOENT ||	/* unlinked */
		     usb_urb->status == -ENODEV ||	/* device removed */
		     usb_urb->status == -ECONNRESET ||	/* unlinked */
		     usb_urb->status == -ESHUTDOWN)) {	/* device disabled */
		goto in_fail;
	}

	spin_lock_irqsave(&rt->in_lock, flags);
	if (rt->in) {
		snd_rawmidi_receive(rt->in, in_urb->buffer, in_urb->instance.actual_length);
    }
	spin_unlock_irqrestore(&rt->in_lock, flags);

	// wake_up(&rt->stream_wait_queue);

	ret = usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
	
	if (ret < 0)
		goto in_fail;

	return;

in_fail:
	dev_err(&in_urb->chip->dev->dev, "%s: midi_fail...\n", __func__);
}

static int xonedb4_midi_in_open(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int xonedb4_midi_in_close(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static void xonedb4_midi_in_trigger(struct snd_rawmidi_substream *alsa_sub, int up)
{
	struct midi_runtime *rt = alsa_sub->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&rt->in_lock, flags);
	if (up)
		rt->in = alsa_sub;
	else
		rt->in = NULL;
	spin_unlock_irqrestore(&rt->in_lock, flags);
}

static void xonedb4_midi_kill_urbs(struct midi_runtime *rt)
{
	int i, time;

	for (i = 0; i < MIDI_N_URBS; i++) {
		time = usb_wait_anchor_empty_timeout(&rt->midi_in_urbs[i].submitted, 100);
		if (!time) {
			usb_kill_anchored_urbs(&rt->midi_in_urbs[i].submitted);
		}
		usb_kill_urb(&rt->midi_in_urbs[i].instance);
	}
}

void xonedb4_midi_abort(struct xonedb4_chip *chip)
{
	printk("xonedb4_midi_abort...\n");

	struct midi_runtime *rt = chip->midi;
	
	if (rt->active) {
		xonedb4_midi_kill_urbs(rt);
	}
}

/*
static int xonedb4_pcm_init_iso_urb(struct iso_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	printk("xonedb4_pcm_init_iso_urb...\n");

	int i;

	urb->chip = chip;
	usb_init_urb(&urb->instance);

    // Allocate memory for transfer buffer
    urb->instance.transfer_buffer = kzalloc(1000, GFP_KERNEL);
    if (!urb->instance.transfer_buffer) {
        printk("Failed to allocate memory for transfer buffer\n");
        return -ENOMEM;
    }

	urb->instance.transfer_buffer_length = ISO_BYTES_PER_FRAME * PCM_N_ISO_URBS_PKTS;
	urb->instance.dev = chip->dev;
	urb->instance.pipe = usb_sndisocpipe(chip->dev, ep);
	urb->instance.interval = 1;
	urb->instance.complete = handler;
	urb->instance.context = urb;
	urb->instance.number_of_packets = PCM_N_ISO_URBS_PKTS;
	// urb->instance.transfer_flags = URB_ISO_ASAP;

	struct usb_iso_packet_descriptor *packet;
    
	for (i = 0; i < PCM_N_ISO_URBS_PKTS; i++) {
		packet = &urb->packets[i];
		packet->offset = i * 4;
		packet->length = 4;
		packet->status = 0;
	}

	if (usb_urb_ep_type_check(&urb->instance)) {
		printk("sanity check failed.....\n");
		return -EINVAL;
	}
	init_usb_anchor(&urb->submitted);

	return 0;
}
*/

static int xonedb4_midi_init_bulk_in_urb(struct midi_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(XDB4_MIDI_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer) {
		return -ENOMEM;
	}

	usb_fill_bulk_urb(&urb->instance, chip->dev, usb_rcvbulkpipe(chip->dev, ep), (void *)urb->buffer, 9, handler, urb);
	if (usb_urb_ep_type_check(&urb->instance)) {
		printk("sanity check failed.....\n");
		return -EINVAL;
	}
	init_usb_anchor(&urb->submitted);

	return 0;
}

static const struct snd_rawmidi_ops in_ops = {
	.open = xonedb4_midi_in_open,
	.close = xonedb4_midi_in_close,
	.trigger = xonedb4_midi_in_trigger
};

int xonedb4_midi_init(struct xonedb4_chip *chip)
{
	int i;
    int ret;
    struct snd_rawmidi *midi;
	struct midi_runtime *rt = kzalloc(sizeof(struct midi_runtime), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

    dev_notice(&chip->dev->dev, "xonedb4_midi_init...\n");

	rt->chip = chip;

	spin_lock_init(&rt->in_lock);

	/*
	for (i = 0; i < PCM_N_ISO_URBS; i++) {
		ret = xonedb4_pcm_init_iso_urb(&rt->iso_urbs[i], chip, PCM_ISO_EP, xonedb4_iso_urb_handler);
		if (ret < 0)
			goto error;
	}
	*/

	/*
	for (i = 0; i < PCM_N_ISO_URBS; i++) {
		usb_anchor_urb(&rt->iso_urbs[i].instance, &rt->iso_urbs[i].submitted);
		ret = usb_submit_urb(&rt->iso_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			goto error;
		}
	}
	*/

    for (i = 0; i < MIDI_N_URBS; i++) {
		ret = xonedb4_midi_init_bulk_in_urb(&rt->midi_in_urbs[i], chip, MIDI_IN_EP, xonedb4_midi_in_urb_handler);
		if (ret < 0) {
			goto error;
		}
	}

	ret = snd_rawmidi_new(chip->card, "XoneDB4MIDI", 0, 0, 1, &midi);
	if (ret < 0) {
		kfree(rt);
		dev_err(&chip->dev->dev, "unable to create midi.\n");
		return ret;
	}

	midi->private_data = rt;
    strscpy(midi->name, "Xone:DB4 MIDI", sizeof(midi->name));
	midi->info_flags = SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(midi, SNDRV_RAWMIDI_STREAM_INPUT, &in_ops);

	for (i = 0; i < MIDI_N_URBS; i++) {
		usb_anchor_urb(&rt->midi_in_urbs[i].instance, &rt->midi_in_urbs[i].submitted);
		ret = usb_submit_urb(&rt->midi_in_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			goto error;
		}
	}

    rt->instance = midi;
	chip->midi = rt;
	rt->active = true;
	return 0;

    error:
	for (i = 0; i < MIDI_N_URBS; i++)
		kfree(rt->midi_in_urbs[i].buffer);
	kfree(rt);
	return ret;
}
