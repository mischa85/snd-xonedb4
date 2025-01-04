#include <sound/rawmidi.h>

#include "midi.h"
#include "chip.h"

#define MIDI_IN_EP		3
#define MIDI_N_URBS		1

#define XDB4_MIDI_PACKET_SIZE		512
#define XDB4_MIDI_SEND_BUFFER_SIZE	512

uint8_t uart_send_count = 0;
uint8_t uart_to_sent = 0;
u8 *uart_send_buffer;

struct midi_urb {
	struct xonedb4_chip *chip;
	struct urb instance;
	struct usb_anchor submitted;
	u8 *buffer;
};

struct midi_runtime {
	struct xonedb4_chip *chip;
	struct snd_rawmidi *instance;

	struct snd_rawmidi_substream *in;
	struct snd_rawmidi_substream *out;

	bool active;

	spinlock_t in_lock;
	spinlock_t out_lock;

	struct midi_urb midi_in_urbs[MIDI_N_URBS];
	u8 *out_buffer;
};

static void xonedb4_midi_in_urb_handler(struct urb *usb_urb)
{
	struct midi_urb *in_urb = usb_urb->context;
	struct midi_runtime *rt = in_urb->chip->midi;

	unsigned long flags;
	int ret;

	if (unlikely(usb_urb->status == -ENOENT || usb_urb->status == -ENODEV || usb_urb->status == -ECONNRESET || usb_urb->status == -ESHUTDOWN)) {
		goto in_fail;
	}

	spin_lock_irqsave(&rt->in_lock, flags);
	if (rt->in) {
		snd_rawmidi_receive(rt->in, in_urb->buffer, in_urb->instance.actual_length);
    }
	spin_unlock_irqrestore(&rt->in_lock, flags);

	ret = usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
	
	if (ret < 0)
		goto in_fail;

	return;

in_fail:
	dev_err(&in_urb->chip->dev->dev, "%s: MIDI FAIL\n", __func__);
}

static int xonedb4_midi_in_open(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int xonedb4_midi_in_close(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int xonedb4_midi_out_open(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int xonedb4_midi_out_close(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

void xonedb4_get_midi_output(u8 *buffer, int count)
{
	uint8_t i;

	for (i = 0; i < count; i++) {
		if (uart_to_sent > 0) {
			buffer[i] = uart_send_buffer[uart_send_count];
			uart_send_count++;
			uart_to_sent--;
		} else {
			uart_send_count = 0;
			buffer[i] = 0xFD;
		}
	}
}

static void xonedb4_midi_out_trigger(struct snd_rawmidi_substream *alsa_sub, int up)
{
	struct midi_runtime *rt = alsa_sub->rmidi->private_data;
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&rt->out_lock, flags);
	if (up) { /* start transfer */
		if (rt->out) { /* we are already transmitting so just return */
			spin_unlock_irqrestore(&rt->out_lock, flags);
			return;
		}

		ret = snd_rawmidi_transmit(alsa_sub, rt->out_buffer, 64);
		if (ret > 0) {
			if (uart_to_sent > (XDB4_MIDI_SEND_BUFFER_SIZE - ret)) {
				dev_notice(&rt->chip->dev->dev, "%s: MIDI SEND BUFFER OVERFLOW\n", __func__);
			} else {
				memcpy(uart_send_buffer + uart_to_sent, rt->out_buffer, ret);
				uart_to_sent += ret;
			}
		}
	} else if (rt->out == alsa_sub)
		rt->out = NULL;
	spin_unlock_irqrestore(&rt->out_lock, flags);
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
	struct midi_runtime *rt = chip->midi;
	
	if (rt->active) {
		xonedb4_midi_kill_urbs(rt);
	}
}

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
		dev_err(&chip->dev->dev, "%s: Sanity check failed!\n", __func__);
		return -EINVAL;
	}
	init_usb_anchor(&urb->submitted);

	return 0;
}

int xonedb4_midi_init_bulk_urbs(struct xonedb4_chip *chip)
{
	uint8_t i;
	int ret;

	struct midi_runtime *rt = chip->midi;
	rt->chip = chip;

	for (i = 0; i < MIDI_N_URBS; i++) {
		ret = xonedb4_midi_init_bulk_in_urb(&rt->midi_in_urbs[i], chip, MIDI_IN_EP, xonedb4_midi_in_urb_handler);
		if (ret < 0) {
			goto error;
		}
	}

	for (i = 0; i < MIDI_N_URBS; i++) {
		usb_anchor_urb(&rt->midi_in_urbs[i].instance, &rt->midi_in_urbs[i].submitted);
		ret = usb_submit_urb(&rt->midi_in_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			xonedb4_midi_kill_urbs(rt);
			goto error;
		}
	}

	return 0;

	error:
	for (i = 0; i < MIDI_N_URBS; i++)
		kfree(rt->midi_in_urbs[i].buffer);
	kfree(rt);
	return ret;
}

static const struct snd_rawmidi_ops out_ops = {
	.open = xonedb4_midi_out_open,
	.close = xonedb4_midi_out_close,
	.trigger = xonedb4_midi_out_trigger
};

static const struct snd_rawmidi_ops in_ops = {
	.open = xonedb4_midi_in_open,
	.close = xonedb4_midi_in_close,
	.trigger = xonedb4_midi_in_trigger
};

int xonedb4_midi_init(struct xonedb4_chip *chip)
{
	int ret;

	struct snd_rawmidi *midi;
	struct midi_runtime *rt = kzalloc(sizeof(struct midi_runtime), GFP_KERNEL);

	if (!rt) {
		return -ENOMEM;
	}

	rt->out_buffer = kzalloc(64, GFP_KERNEL);
	if (!rt->out_buffer) {
		kfree(rt);
		return -ENOMEM;
	}

	uart_send_buffer = kzalloc(XDB4_MIDI_SEND_BUFFER_SIZE, GFP_KERNEL);
	if (!uart_send_buffer) {
		kfree(rt);
		return -ENOMEM;
	}

	rt->chip = chip;

	spin_lock_init(&rt->in_lock);
	spin_lock_init(&rt->out_lock);

	ret = snd_rawmidi_new(chip->card, chip->dev->product, 0, 1, 1, &midi);
	if (ret < 0) {
		kfree(rt);
		dev_err(&chip->dev->dev, "%s: Cannot create MIDI instance!\n", __func__);
		return ret;
	}

	midi->private_data = rt;

	strscpy(midi->name, chip->dev->product, sizeof(midi->name));
	midi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(midi, SNDRV_RAWMIDI_STREAM_OUTPUT, &out_ops);
	snd_rawmidi_set_ops(midi, SNDRV_RAWMIDI_STREAM_INPUT, &in_ops);

	rt->instance = midi;
	chip->midi = rt;
	rt->active = true;

	ret = xonedb4_midi_init_bulk_urbs(chip);
	if (ret < 0) {
		goto error;
	}

	return 0;

	error:
	dev_err(&chip->dev->dev, "%s: ERROR\n", __func__);
	kfree(rt);
	return ret;
}
