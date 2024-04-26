#include <linux/slab.h>
#include <sound/pcm.h>

#include "pcm.h"
#include "chip.h"
#include "../common/ploytec.h"

#define PCM_OUT_EP							5
#define PCM_IN_EP							6

#define PCM_N_URBS							8

#define PCM_N_PLAYBACK_CHANNELS				8
#define PCM_N_CAPTURE_CHANNELS				8

#define XDB4_PCM_PACKET_SIZE				512
#define XDB4_PLAYBACK_SAMPLES_PER_PACKET	20
#define XDB4_CAPTURE_SAMPLES_PER_PACKET		16

#define ALSA_BYTES_PER_SAMPLE				3 // S24_3LE

#define ALSA_PCM_PLAYBACK_PACKET_SIZE		PCM_N_PLAYBACK_CHANNELS * ALSA_BYTES_PER_SAMPLE * (XDB4_PLAYBACK_SAMPLES_PER_PACKET / 2)
#define ALSA_PCM_CAPTURE_PACKET_SIZE		PCM_N_CAPTURE_CHANNELS * ALSA_BYTES_PER_SAMPLE * (XDB4_CAPTURE_SAMPLES_PER_PACKET / 2)
#define ALSA_MIN_BUFSIZE					20 * ALSA_PCM_PLAYBACK_PACKET_SIZE
#define ALSA_MAX_BUFSIZE					2000 * ALSA_PCM_PLAYBACK_PACKET_SIZE

struct pcm_urb {
	struct xonedb4_chip *chip;
	struct urb instance;
	struct usb_anchor submitted;
	uint8_t *buffer;
};

struct pcm_substream {
	spinlock_t lock;
	struct snd_pcm_substream *instance;

	bool active;

	snd_pcm_uframes_t dma_off; /* current position in alsa dma_area */
	snd_pcm_uframes_t period_off; /* current position in current period */
};

enum { /* pcm streaming states */
	STREAM_DISABLED, /* no pcm streaming */
	STREAM_STARTING, /* pcm streaming requested, waiting to become ready */
	STREAM_RUNNING,  /* pcm streaming running */
	STREAM_STOPPING
};

struct pcm_runtime {
	struct xonedb4_chip *chip;
	struct snd_pcm *instance;

	struct pcm_substream playback;
	struct pcm_substream capture;
	bool panic; /* if set driver won't do anymore pcm on device */

	struct pcm_urb pcm_out_urbs[PCM_N_URBS];
	struct pcm_urb pcm_in_urbs[PCM_N_URBS];

	struct mutex stream_mutex;
	uint8_t stream_state; /* one of STREAM_XXX */
	uint8_t rate; /* one of PCM_RATE_XXX */
};

static const int rates[] = { 44100, 48000, 88200, 96000 };
static const int rates_alsaid[] = {	SNDRV_PCM_RATE_44100, SNDRV_PCM_RATE_48000,	SNDRV_PCM_RATE_88200, SNDRV_PCM_RATE_96000 };

static const struct snd_pcm_hardware pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_MMAP_VALID,

	.formats = SNDRV_PCM_FMTBIT_S24_3LE,

	.rates = SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000,

	.rate_min = 44100,
	.rate_max = 96000,
	.channels_min = PCM_N_PLAYBACK_CHANNELS,
	.channels_max = PCM_N_PLAYBACK_CHANNELS,
	.buffer_bytes_max = ALSA_MAX_BUFSIZE,
	.period_bytes_min = ALSA_MIN_BUFSIZE,
	.period_bytes_max = ALSA_MAX_BUFSIZE,
	.periods_min = 2,
	.periods_max = 1024
};

static struct pcm_substream *xonedb4_pcm_get_substream(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		return &rt->playback;
	} else if (alsa_sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
		return &rt->capture;
	}

	dev_err(&rt->chip->dev->dev, "Error getting pcm substream slot.\n");
	return NULL;
}

static void xonedb4_pcm_stream_stop(struct pcm_runtime *rt)
{
	printk("xonedb4_pcm_stream_stop...\n");

	if (rt->stream_state != STREAM_DISABLED) {
		rt->stream_state = STREAM_STOPPING;
		rt->stream_state = STREAM_DISABLED;
	}
}

static void xonedb4_pcm_kill_urbs(struct pcm_runtime *rt)
{
	printk("xonedb4_pcm_kill_urbs...\n");
	int i, time;

	for (i = 0; i < PCM_N_URBS; i++) {
		time = usb_wait_anchor_empty_timeout(&rt->pcm_in_urbs[i].submitted, 100);
		if (!time) {
			usb_kill_anchored_urbs(&rt->pcm_in_urbs[i].submitted);
		}
		time = usb_wait_anchor_empty_timeout(&rt->pcm_out_urbs[i].submitted, 100);
		if (!time) {
			usb_kill_anchored_urbs(&rt->pcm_out_urbs[i].submitted);
		}
		usb_kill_urb(&rt->pcm_in_urbs[i].instance);
		usb_kill_urb(&rt->pcm_out_urbs[i].instance);
	}
}

static void xonedb4_pcm_poison_urbs(struct pcm_runtime *rt)
{
	printk("xonedb4_pcm_poison_urbs...\n");
	
	int i, time;

	for (i = 0; i < PCM_N_URBS; i++) {
		time = usb_wait_anchor_empty_timeout(&rt->pcm_in_urbs[i].submitted, 100);
		if (!time) {
			usb_kill_anchored_urbs(&rt->pcm_in_urbs[i].submitted);
		}
		time = usb_wait_anchor_empty_timeout(&rt->pcm_out_urbs[i].submitted, 100);
		if (!time) {
			usb_kill_anchored_urbs(&rt->pcm_out_urbs[i].submitted);
		}
		usb_poison_urb(&rt->pcm_in_urbs[i].instance);
		usb_poison_urb(&rt->pcm_out_urbs[i].instance);
	}
}

/* call with stream_mutex locked */
static int xonedb4_pcm_stream_start(struct pcm_runtime *rt)
{
	printk("xonedb4_pcm_stream_start...\n");

	int ret = 0;
	
	if (rt->stream_state == STREAM_DISABLED) {
		/* reset panic state when starting a new stream */
		rt->panic = false;
		rt->stream_state = STREAM_STARTING;
		rt->stream_state = STREAM_RUNNING;
	}
	return ret;
}

static int xonedb4_pcm_set_rate(struct pcm_runtime *rt)
{
	printk("xonedb4_pcm_set_rate...\n");

	rt->chip->alsarate = rt->rate;

	dev_notice(&rt->chip->dev->dev, "ALSARATE: %d\n", rt->chip->alsarate);
	dev_notice(&rt->chip->dev->dev, "DEVICERATE: %d\n", rt->chip->devicerate);

	if (rt->chip->alsarate != rt->chip->devicerate) {
		mutex_unlock(&rt->stream_mutex);
		xonedb4_reset(rt->chip);
		mutex_lock(&rt->stream_mutex);
	}

	return 0;
}

static void convert_capture(uint8_t *dest, uint8_t *src1, unsigned int n, uint8_t *src2, unsigned int len)
{
	unsigned int i;
	
	if (src2 == NULL) {
		for (i = 0; i < (XDB4_CAPTURE_SAMPLES_PER_PACKET / 2); i++) {
			ploytec_convert_to_s24_3le(dest + (i * (ALSA_BYTES_PER_SAMPLE * PCM_N_PLAYBACK_CHANNELS)), src1 + (i * 64));
		}
	} else {
		/* The ALSA buffer always seems to align with the samples, but I want to keep this here just in case */
		/*
		if ((len % (ALSA_BYTES_PER_SAMPLE * PCM_N_CAPTURE_CHANNELS)) != 0) {
			printk("helpppp\n");
		}
		*/
		for (i = 0; i < (len / (ALSA_BYTES_PER_SAMPLE * PCM_N_CAPTURE_CHANNELS)); i++) {
			ploytec_convert_to_s24_3le(dest + (i * (ALSA_BYTES_PER_SAMPLE * PCM_N_CAPTURE_CHANNELS)), src1 + (i * 64));
		}
		for (i = i; i < ((n - len) / (ALSA_BYTES_PER_SAMPLE * PCM_N_CAPTURE_CHANNELS)); i++) {
			ploytec_convert_to_s24_3le(dest + (i * (ALSA_BYTES_PER_SAMPLE * PCM_N_CAPTURE_CHANNELS)), src2 + (i * 64));
		}
	}
}

static void convert_playback(uint8_t *dest, uint8_t *src1, unsigned int n, uint8_t *src2, unsigned int len)
{
	unsigned int i;

	if (src2 == NULL) {
		for (i = 0; i < (XDB4_PLAYBACK_SAMPLES_PER_PACKET / 2); i++) {
			ploytec_convert_from_s24_3le(dest + (i * 48), src1 + (i * (ALSA_BYTES_PER_SAMPLE * PCM_N_PLAYBACK_CHANNELS)));
		}
	} else {
		/* The ALSA buffer always seems to align with the samples, but I want to keep this here just in case */
		/*
		if ((len % (ALSA_BYTES_PER_SAMPLE * PCM_N_PLAYBACK_CHANNELS)) != 0) {
			printk("helpppp\n");
		}
		*/
		for (i = 0; i < (len / (ALSA_BYTES_PER_SAMPLE * PCM_N_PLAYBACK_CHANNELS)); i++) {
			ploytec_convert_from_s24_3le(dest + (i * 48), src1 + (i * (ALSA_BYTES_PER_SAMPLE * PCM_N_PLAYBACK_CHANNELS)));
		}
		for (i = i; i < ((n - len) / (ALSA_BYTES_PER_SAMPLE * PCM_N_PLAYBACK_CHANNELS)); i++) {
			ploytec_convert_from_s24_3le(dest + (i * 48), src2 + (i * (ALSA_BYTES_PER_SAMPLE * PCM_N_PLAYBACK_CHANNELS)));
		}
	}
	ploytec_sync_bytes(dest + 480);
}

/* call with substream locked */
/* returns true if a period elapsed */
static bool xonedb4_pcm_capture(struct pcm_substream *sub, struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	uint8_t *dest;
	uint8_t *dest2;
	unsigned int pcm_buffer_size;

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	if (sub->dma_off + ALSA_PCM_CAPTURE_PACKET_SIZE <= pcm_buffer_size) {
		dev_dbg(&urb->chip->dev->dev, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);
		dest = alsa_rt->dma_area + sub->dma_off;
		convert_capture(dest, urb->buffer, ALSA_PCM_CAPTURE_PACKET_SIZE, NULL, 0);
	} else {
		/* wrap around at end of ring buffer */
		unsigned int len;
		len = pcm_buffer_size - sub->dma_off;
		dev_dbg(&urb->chip->dev->dev, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);
		dest = alsa_rt->dma_area + sub->dma_off;
		dest2 = alsa_rt->dma_area;
		convert_capture(dest, urb->buffer, ALSA_PCM_CAPTURE_PACKET_SIZE, dest2, len);
	}
	sub->dma_off += ALSA_PCM_CAPTURE_PACKET_SIZE;
	if (sub->dma_off >= pcm_buffer_size) {
		sub->dma_off -= pcm_buffer_size;
	}

	sub->period_off += ALSA_PCM_CAPTURE_PACKET_SIZE;
	if (sub->period_off >= alsa_rt->period_size) {
		sub->period_off %= alsa_rt->period_size;
		return true;
	}

	return false;
}

/* call with substream locked */
/* returns true if a period elapsed */
static bool xonedb4_pcm_playback(struct pcm_substream *sub, struct pcm_urb *urb)
{
	// printk("xonedb4_pcm_playback...\n");

	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	uint8_t *source;
	uint8_t *source2;
	unsigned int pcm_buffer_size;

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	if (sub->dma_off + ALSA_PCM_PLAYBACK_PACKET_SIZE <= pcm_buffer_size) {
		dev_dbg(&urb->chip->dev->dev, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);
		source = alsa_rt->dma_area + sub->dma_off;
		convert_playback(urb->buffer, source, ALSA_PCM_PLAYBACK_PACKET_SIZE, NULL, 0);
	} else {
		/* wrap around at end of ring buffer */
		unsigned int len;
		len = pcm_buffer_size - sub->dma_off;
		dev_dbg(&urb->chip->dev->dev, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);
		source = alsa_rt->dma_area + sub->dma_off;
		source2 = alsa_rt->dma_area;
		convert_playback(urb->buffer, source, ALSA_PCM_PLAYBACK_PACKET_SIZE, source2, len);
	}
	sub->dma_off += ALSA_PCM_PLAYBACK_PACKET_SIZE;
	if (sub->dma_off >= pcm_buffer_size) {
		sub->dma_off -= pcm_buffer_size;
	}

	sub->period_off += ALSA_PCM_PLAYBACK_PACKET_SIZE;
	if (sub->period_off >= alsa_rt->period_size) {
		sub->period_off %= alsa_rt->period_size;
		return true;
	}

	return false;
}

static void xonedb4_pcm_in_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *in_urb = usb_urb->context;
	struct pcm_runtime *rt = in_urb->chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	int ret;
	uint16_t i;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(usb_urb->status == -ENOENT ||	/* unlinked */
		     usb_urb->status == -ENODEV ||	/* device removed */
		     usb_urb->status == -ECONNRESET ||	/* unlinked */
		     usb_urb->status == -ESHUTDOWN)) {	/* device disabled */
		goto in_fail;
	}

	sub = &rt->capture;
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		do_period_elapsed = xonedb4_pcm_capture(sub, in_urb);
	} else {
		memset(in_urb->buffer + i, 0, XDB4_PCM_PACKET_SIZE);
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed) {
		snd_pcm_period_elapsed(sub->instance);
	}

	ret = usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
	
	if (ret < 0)
		goto in_fail;

	return;

in_fail:
	dev_err(&in_urb->chip->dev->dev, "%s: in_fail...\n", __func__);
	rt->panic = true;
}

static void xonedb4_pcm_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *out_urb = usb_urb->context;
	struct pcm_runtime *rt = out_urb->chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	int ret;
	int i;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(usb_urb->status == -ENOENT || usb_urb->status == -ENODEV || usb_urb->status == -ECONNRESET || usb_urb->status == -ESHUTDOWN)) {
		goto out_fail;
	}

	sub = &rt->playback;
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		do_period_elapsed = xonedb4_pcm_playback(sub, out_urb);
	} else {
		memset(out_urb->buffer + i, 0, 480);
		out_urb->buffer[i + 480] = 0xFD;
		out_urb->buffer[i + 481] = 0xFF;
		memset(out_urb->buffer + i + 482, 0, 30);
	}

	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed) {
		snd_pcm_period_elapsed(sub->instance);
	}

	ret = usb_submit_urb(&out_urb->instance, GFP_ATOMIC);
	
	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	dev_err(&out_urb->chip->dev->dev, "%s: out_fail...\n", __func__);
	rt->panic = true;
}

static int xonedb4_pcm_open(struct snd_pcm_substream *alsa_sub)
{
	printk("xonedb4_pcm_open...\n");

	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = NULL;
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;

	if (rt->panic)
		return -EPIPE;

	mutex_lock(&rt->stream_mutex);
	alsa_rt->hw = pcm_hw;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		sub = &rt->playback;
	} else if (alsa_sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
		sub = &rt->capture;
	}

	rt->rate = ARRAY_SIZE(rates);
	alsa_rt->hw.rates = rates_alsaid[rt->rate];

	if (!sub) {
		mutex_unlock(&rt->stream_mutex);
		dev_err(&rt->chip->dev->dev, "Invalid stream type\n");
		return -EINVAL;
	}

	sub->instance = alsa_sub;
	sub->active = false;
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int xonedb4_pcm_close(struct snd_pcm_substream *alsa_sub)
{
	printk("xonedb4_pcm_close...\n");

	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = xonedb4_pcm_get_substream(alsa_sub);
	unsigned long flags;

	if (rt->panic)
		return 0;

	mutex_lock(&rt->stream_mutex);
	if (sub) {
		/* deactivate substream */
		spin_lock_irqsave(&sub->lock, flags);
		sub->instance = NULL;
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);

		/* all substreams closed? if so, stop streaming */
		if (!rt->playback.instance && !rt->capture.instance) {
			xonedb4_pcm_stream_stop(rt);
			rt->rate = ARRAY_SIZE(rates);
		}
	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int xonedb4_pcm_prepare(struct snd_pcm_substream *alsa_sub)
{
	printk("xonedb4_pcm_prepare...\n");

	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = xonedb4_pcm_get_substream(alsa_sub);
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;
	int ret;

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	mutex_lock(&rt->stream_mutex);

	if (alsa_rt->format != SNDRV_PCM_FORMAT_S24_3LE) {
		return -EINVAL;
	}

	sub->dma_off = 0;
	sub->period_off = 0;

	if (rt->stream_state == STREAM_DISABLED) {
		for (rt->rate = 0; rt->rate < ARRAY_SIZE(rates); rt->rate++)
			if (alsa_rt->rate == rates[rt->rate])
				break;
		if (rt->rate == ARRAY_SIZE(rates)) {
			mutex_unlock(&rt->stream_mutex);
			dev_err(&rt->chip->dev->dev, "invalid rate %d in prepare.\n", alsa_rt->rate);
			return -EINVAL;
		}

		dev_notice(&rt->chip->dev->dev, "rate in pcm_prepare: %d\n", alsa_rt->rate);

		ret = xonedb4_pcm_set_rate(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}

		ret = xonedb4_pcm_stream_start(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			dev_err(&rt->chip->dev->dev, "could not start pcm stream.\n");
			return ret;
		}
	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int xonedb4_pcm_trigger(struct snd_pcm_substream *alsa_sub, int cmd)
{
	printk("xonedb4_pcm_trigger...\n");
	
	struct pcm_substream *sub = xonedb4_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irq(&sub->lock);
		sub->active = true;
		spin_unlock_irq(&sub->lock);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irq(&sub->lock);
		sub->active = false;
		spin_unlock_irq(&sub->lock);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t xonedb4_pcm_pointer(struct snd_pcm_substream *alsa_sub)
{
	// printk("xonedb4_pcm_pointer...\n");

	struct pcm_substream *sub = xonedb4_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;
	snd_pcm_uframes_t dma_offset;

	if (rt->panic || !sub) {
		dev_err(&rt->chip->dev->dev, "%s: XRUN.......\n", __func__);
		return SNDRV_PCM_POS_XRUN;
	}

	spin_lock_irqsave(&sub->lock, flags);
	dma_offset = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);
	return bytes_to_frames(alsa_sub->runtime, dma_offset);
}

void xonedb4_pcm_abort(struct xonedb4_chip *chip)
{
	printk("xonedb4_pcm_abort...\n");
	struct pcm_runtime *rt = chip->pcm;

	if (rt) {
		rt->panic = true;

		xonedb4_pcm_stream_stop(rt);
		xonedb4_pcm_poison_urbs(rt);
	}
}

static const struct snd_pcm_ops pcm_ops = {
	.open = xonedb4_pcm_open,
	.close = xonedb4_pcm_close,
	.prepare = xonedb4_pcm_prepare,
	.trigger = xonedb4_pcm_trigger,
	.pointer = xonedb4_pcm_pointer,
};

static int xonedb4_pcm_init_bulk_out_urb(struct pcm_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	printk("xonedb4_pcm_init_bulk_out_urb");

	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(XDB4_PCM_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer) {
		return -ENOMEM;
	}

	usb_fill_bulk_urb(&urb->instance, chip->dev, usb_sndbulkpipe(chip->dev, ep), (void *)urb->buffer, XDB4_PCM_PACKET_SIZE, handler, urb);
	if (usb_urb_ep_type_check(&urb->instance)) {
		printk("sanity check failed.....\n");
		return -EINVAL;
	}
	init_usb_anchor(&urb->submitted);

	return 0;
}

static int xonedb4_pcm_init_bulk_in_urb(struct pcm_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	printk("xonedb4_pcm_init_bulk_in_urb");
	
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(XDB4_PCM_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer) {
		return -ENOMEM;
	}

	usb_fill_bulk_urb(&urb->instance, chip->dev, usb_rcvbulkpipe(chip->dev, ep), (void *)urb->buffer, XDB4_PCM_PACKET_SIZE, handler, urb);
	if (usb_urb_ep_type_check(&urb->instance)) {
		printk("sanity check failed.....\n");
		return -EINVAL;
	}
	init_usb_anchor(&urb->submitted);

	return 0;
}

int xonedb4_pcm_init_bulk_urbs(struct xonedb4_chip *chip)
{
	printk("xonedb4_pcm_init_bulk_urbs...\n");
	int i;
	int ret;

	struct pcm_runtime *rt = chip->pcm;
	rt->chip = chip;

	for (i = 0; i < PCM_N_URBS; i++) {
		ret = xonedb4_pcm_init_bulk_in_urb(&rt->pcm_in_urbs[i], chip, PCM_IN_EP, xonedb4_pcm_in_urb_handler);
		if (ret < 0) {
			goto error;
		}
	}

	for (i = 0; i < PCM_N_URBS; i++) {
		ret = xonedb4_pcm_init_bulk_out_urb(&rt->pcm_out_urbs[i], chip, PCM_OUT_EP, xonedb4_pcm_out_urb_handler);
		if (ret < 0)
			goto error;
	}

	mutex_lock(&rt->stream_mutex);
	for (i = 0; i < PCM_N_URBS; i++) {
		usb_anchor_urb(&rt->pcm_in_urbs[i].instance, &rt->pcm_in_urbs[i].submitted);
		ret = usb_submit_urb(&rt->pcm_in_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			xonedb4_pcm_stream_stop(rt);
			xonedb4_pcm_kill_urbs(rt);
			goto error;
		}
	}

	for (i = 0; i < PCM_N_URBS; i++) {
		memset(rt->pcm_out_urbs[i].buffer, 0, 480);
		ploytec_sync_bytes(rt->pcm_out_urbs[i].buffer + 480);

		usb_anchor_urb(&rt->pcm_out_urbs[i].instance, &rt->pcm_out_urbs[i].submitted);
		ret = usb_submit_urb(&rt->pcm_out_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			xonedb4_pcm_stream_stop(rt);
			xonedb4_pcm_kill_urbs(rt);
			goto error;
		}
	}
	mutex_unlock(&rt->stream_mutex);
	
	return 0;

	error:
	printk("ERRRORRRR");
	mutex_unlock(&rt->stream_mutex);
	for (i = 0; i < PCM_N_URBS; i++)
		kfree(rt->pcm_out_urbs[i].buffer);
	return ret;
}


int xonedb4_pcm_init(struct xonedb4_chip *chip)
{
	printk("xonedb4_pcm_init...\n");
	int ret;

	struct snd_pcm *pcm;
	struct pcm_runtime *rt = kzalloc(sizeof(struct pcm_runtime), GFP_KERNEL);

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;

	mutex_init(&rt->stream_mutex);
	spin_lock_init(&rt->playback.lock);

	ret = snd_pcm_new(chip->card, "XoneDB4PCM", 0, 1, 1, &pcm);
	if (ret < 0) {
		kfree(rt);
		dev_err(&chip->dev->dev, "Cannot create pcm instance\n");
		return ret;
	}

	pcm->private_data = rt;

	strscpy(pcm->name, "Xone:DB4 PCM", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	rt->instance = pcm;
	chip->pcm = rt;

	ret = xonedb4_pcm_init_bulk_urbs(chip);
	if (ret < 0) {
		goto error;
	}
	
	return 0;

	error:
	printk("ERRRORRRR");
	kfree(rt);
	return ret;
}
