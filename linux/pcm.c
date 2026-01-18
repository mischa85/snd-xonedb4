#include <linux/slab.h>
#include <sound/pcm.h>

#include "pcm.h"
#include "chip.h"
#include "midi.h"
#include "../legacy/common/ploytec.h"

#define PCM_OUT_EP						5
#define PCM_IN_EP						6

#define PCM_N_URBS						4
#define PCM_N_PLAYBACK_CHANNELS			8
#define PCM_N_CAPTURE_CHANNELS			8

#define XDB4_PCM_OUT_FRAME_SIZE			48
#define XDB4_PCM_IN_FRAME_SIZE			64
#define XDB4_PCM_OUT_FRAMES_PER_PACKET	40
#define XDB4_PCM_IN_FRAMES_PER_PACKET	32
#define XDB4_UART_OUT_BYTES_PER_PACKET	8
#define XDB4_PCM_BULK_OUT_PACKET_SIZE	((XDB4_PCM_OUT_FRAMES_PER_PACKET * XDB4_PCM_OUT_FRAME_SIZE) + XDB4_UART_OUT_BYTES_PER_PACKET + ((XDB4_PCM_OUT_FRAMES_PER_PACKET / 10) * 30)) // 40 frames
#define XDB4_PCM_INT_OUT_PACKET_SIZE	((XDB4_PCM_OUT_FRAMES_PER_PACKET * XDB4_PCM_OUT_FRAME_SIZE) + XDB4_UART_OUT_BYTES_PER_PACKET) // 40 frames
#define XDB4_PCM_IN_PACKET_SIZE			XDB4_PCM_IN_FRAMES_PER_PACKET * XDB4_PCM_IN_FRAME_SIZE // 32 frames

#define ALSA_BYTES_PER_SAMPLE			3 // S24_3LE
#define ALSA_BYTES_PER_FRAME			PCM_N_PLAYBACK_CHANNELS * ALSA_BYTES_PER_SAMPLE
#define ALSA_PCM_OUT_PACKET_SIZE		PCM_N_PLAYBACK_CHANNELS * ALSA_BYTES_PER_SAMPLE * XDB4_PCM_OUT_FRAMES_PER_PACKET
#define ALSA_PCM_IN_PACKET_SIZE			PCM_N_CAPTURE_CHANNELS * ALSA_BYTES_PER_SAMPLE * XDB4_PCM_IN_FRAMES_PER_PACKET
#define ALSA_MIN_BUFSIZE				2 * ALSA_PCM_OUT_PACKET_SIZE
#define ALSA_MAX_BUFSIZE				2000 * ALSA_PCM_OUT_PACKET_SIZE

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

	dev_err(&rt->chip->dev->dev, "%s: Error getting pcm substream slot.\n", __func__);
	return NULL;
}

static void xonedb4_pcm_stream_stop(struct pcm_runtime *rt)
{
	if (rt->stream_state != STREAM_DISABLED) {
		rt->stream_state = STREAM_STOPPING;
		rt->stream_state = STREAM_DISABLED;
	}
}

static void xonedb4_pcm_kill_urbs(struct pcm_runtime *rt)
{
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
	rt->chip->alsarate = rt->rate;

	if (rt->chip->alsarate != rt->chip->devicerate) {
		dev_notice(&rt->chip->dev->dev, "%s: Resetting device for samplerate change %d -> %d\n", __func__, rates[rt->chip->devicerate], rates[rt->chip->alsarate]);
		mutex_unlock(&rt->stream_mutex);
		xonedb4_reset(rt->chip);
		mutex_lock(&rt->stream_mutex);
	}

	return 0;
}

/* call with substream locked */
/* returns true if a period elapsed */
static bool xonedb4_pcm_capture(struct pcm_substream *sub, struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	unsigned int pcm_buffer_size;

	pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	if (sub->dma_off + ALSA_PCM_IN_PACKET_SIZE <= pcm_buffer_size) {
		dev_dbg(&urb->chip->dev->dev, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);

		uint8_t curframe = 0;
		uint8_t *src = urb->buffer;
		uint8_t *dest = alsa_rt->dma_area + sub->dma_off;

		for (curframe = 0; curframe < XDB4_PCM_IN_FRAMES_PER_PACKET; curframe++) {
			ploytec_convert_to_s24_3le(dest + (curframe * ALSA_BYTES_PER_FRAME), src + (curframe * XDB4_PCM_IN_FRAME_SIZE));
		}
	} else {
		/* wrap around at end of ring buffer */
		dev_dbg(&urb->chip->dev->dev, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);

		uint8_t curframexone = 0;
		uint8_t curframealsa1 = 0;
		uint8_t curframealsa2 = 0;
		uint8_t numframesalsa1 = (pcm_buffer_size - sub->dma_off) / ((uint32_t) ALSA_BYTES_PER_FRAME);
		uint8_t numframesalsa2 = XDB4_PCM_OUT_FRAMES_PER_PACKET - numframesalsa1;
		uint8_t *src = urb->buffer;
		uint8_t *dest1 = alsa_rt->dma_area + sub->dma_off;
		uint8_t *dest2 = alsa_rt->dma_area;

		for (curframexone = 0; curframexone < XDB4_PCM_IN_FRAMES_PER_PACKET; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_to_s24_3le(dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME), src + (curframexone * XDB4_PCM_IN_FRAME_SIZE));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_to_s24_3le(dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME), src + (curframexone * XDB4_PCM_IN_FRAME_SIZE));
				curframealsa2++;
			}
		}
	}
	sub->dma_off += ALSA_PCM_IN_PACKET_SIZE;
	if (sub->dma_off >= pcm_buffer_size) {
		sub->dma_off -= pcm_buffer_size;
	}

	sub->period_off += ALSA_PCM_IN_PACKET_SIZE;
	if (sub->period_off >= alsa_rt->period_size) {
		sub->period_off %= alsa_rt->period_size;
		return true;
	}

	return false;
}

/* call with substream locked */
/* returns true if a period elapsed */
static bool xonedb4_pcm_bulk_playback(struct pcm_substream *sub, struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	uint32_t pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	if (sub->dma_off + ALSA_PCM_OUT_PACKET_SIZE <= pcm_buffer_size) {
		dev_dbg(&urb->chip->dev->dev, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);

		uint8_t curframe = 0;
		uint8_t *src = urb->buffer;
		uint8_t *dest = alsa_rt->dma_area + sub->dma_off;

		for (curframe = 0; curframe < 10; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE), dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
		for (curframe = 10; curframe < 20; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 32, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
		for (curframe = 20; curframe < 30; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 64, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
		for (curframe = 30; curframe < 40; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 96, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
	} else {
		/* wrap around at end of ring buffer */
		dev_dbg(&urb->chip->dev->dev, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);

		uint8_t curframexone = 0;
		uint8_t curframealsa1 = 0;
		uint8_t curframealsa2 = 0;
		uint8_t numframesalsa1 = (pcm_buffer_size - sub->dma_off) / ((uint32_t) ALSA_BYTES_PER_FRAME);
		uint8_t numframesalsa2 = XDB4_PCM_OUT_FRAMES_PER_PACKET - numframesalsa1;
		uint8_t *src = urb->buffer;
		uint8_t *dest1 = alsa_rt->dma_area + sub->dma_off;
		uint8_t *dest2 = alsa_rt->dma_area;

		for (curframexone = 0; curframexone < 10; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE), dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE), dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
		for (curframexone = 10; curframexone < 20; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 32, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 32, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
		for (curframexone = 20; curframexone < 30; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 64, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 64, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
		for (curframexone = 30; curframexone < 40; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 96, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 96, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
	}
	sub->dma_off += ALSA_PCM_OUT_PACKET_SIZE;
	if (sub->dma_off >= pcm_buffer_size) {
		sub->dma_off -= pcm_buffer_size;
	}

	sub->period_off += ALSA_PCM_OUT_PACKET_SIZE;
	if (sub->period_off >= alsa_rt->period_size) {
		sub->period_off %= alsa_rt->period_size;
		return true;
	}

	return false;
}

/* call with substream locked */
/* returns true if a period elapsed */
static bool xonedb4_pcm_int_playback(struct pcm_substream *sub, struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	uint32_t pcm_buffer_size = snd_pcm_lib_buffer_bytes(sub->instance);

	if (sub->dma_off + ALSA_PCM_OUT_PACKET_SIZE <= pcm_buffer_size) {
		dev_dbg(&urb->chip->dev->dev, "%s: (1) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);

		uint8_t curframe = 0;
		uint8_t *src = urb->buffer;
		uint8_t *dest = alsa_rt->dma_area + sub->dma_off;

		for (curframe = 0; curframe < 9; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 0, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
		for (curframe = 9; curframe < 19; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 2, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
		for (curframe = 19; curframe < 29; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 4, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
		for (curframe = 29; curframe < 39; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 6, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
		for (curframe = 39; curframe < 40; curframe++) {
			ploytec_convert_from_s24_3le(src + (curframe * XDB4_PCM_OUT_FRAME_SIZE) + 8, dest + (curframe * ALSA_BYTES_PER_FRAME));
		}
	} else {
		/* wrap around at end of ring buffer */
		dev_dbg(&urb->chip->dev->dev, "%s: (2) buffer_size %#x dma_offset %#x\n", __func__, (unsigned int) pcm_buffer_size, (unsigned int) sub->dma_off);

		uint8_t curframexone = 0;
		uint8_t curframealsa1 = 0;
		uint8_t curframealsa2 = 0;
		uint8_t numframesalsa1 = (pcm_buffer_size - sub->dma_off) / ((uint32_t) ALSA_BYTES_PER_FRAME);
		uint8_t numframesalsa2 = XDB4_PCM_OUT_FRAMES_PER_PACKET - numframesalsa1;
		uint8_t *src = urb->buffer;
		uint8_t *dest1 = alsa_rt->dma_area + sub->dma_off;
		uint8_t *dest2 = alsa_rt->dma_area;

		for (curframexone = 0; curframexone < 9; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 0, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 0, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
		for (curframexone = 9; curframexone < 19; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 2, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 2, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
		for (curframexone = 19; curframexone < 29; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 4, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 4, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
		for (curframexone = 29; curframexone < 39; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 6, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 6, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
		for (curframexone = 39; curframexone < 40; curframexone++) {
			if (curframealsa1 < numframesalsa1) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 8, dest1 + (curframealsa1 * ALSA_BYTES_PER_FRAME));
				curframealsa1++;
			} else if (curframealsa2 < numframesalsa2) {
				ploytec_convert_from_s24_3le(src + (curframexone * XDB4_PCM_OUT_FRAME_SIZE) + 8, dest2 + (curframealsa2 * ALSA_BYTES_PER_FRAME));
				curframealsa2++;
			}
		}
	}
	sub->dma_off += ALSA_PCM_OUT_PACKET_SIZE;
	if (sub->dma_off >= pcm_buffer_size) {
		sub->dma_off -= pcm_buffer_size;
	}

	sub->period_off += ALSA_PCM_OUT_PACKET_SIZE;
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
		memset(in_urb->buffer, 0, XDB4_PCM_IN_PACKET_SIZE);
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
	dev_err(&in_urb->chip->dev->dev, "%s: IN FAIL\n", __func__);
	rt->panic = true;
}

static void xonedb4_pcm_bulk_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *out_urb = usb_urb->context;
	struct pcm_runtime *rt = out_urb->chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(usb_urb->status == -ENOENT || usb_urb->status == -ENODEV || usb_urb->status == -ECONNRESET || usb_urb->status == -ESHUTDOWN)) {
		goto out_fail;
	}

	sub = &rt->playback;

	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		do_period_elapsed = xonedb4_pcm_bulk_playback(sub, out_urb);
	} else {
		memset(out_urb->buffer + 0, 0, 480);
		memset(out_urb->buffer + 512, 0, 480);
		memset(out_urb->buffer + 1024, 0, 480);
		memset(out_urb->buffer + 1536, 0, 480);
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed) {
		snd_pcm_period_elapsed(sub->instance);
	}

	xonedb4_get_midi_output(out_urb->buffer + 480, 1);
	xonedb4_get_midi_output(out_urb->buffer + 992, 1);
	xonedb4_get_midi_output(out_urb->buffer + 1504, 1);
	xonedb4_get_midi_output(out_urb->buffer + 2016, 1);

	ret = usb_submit_urb(&out_urb->instance, GFP_ATOMIC);

	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	dev_err(&out_urb->chip->dev->dev, "%s: OUT FAIL\n", __func__);
	rt->panic = true;
}

static void xonedb4_pcm_int_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *out_urb = usb_urb->context;
	struct pcm_runtime *rt = out_urb->chip->pcm;
	struct pcm_substream *sub;
	bool do_period_elapsed = false;
	unsigned long flags;
	int ret;

	if (rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (unlikely(usb_urb->status == -ENOENT || usb_urb->status == -ENODEV || usb_urb->status == -ECONNRESET || usb_urb->status == -ESHUTDOWN)) {
		goto out_fail;
	}

	sub = &rt->playback;

	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		do_period_elapsed = xonedb4_pcm_int_playback(sub, out_urb);
	} else {
		memset(out_urb->buffer + 0, 0, 432);
		memset(out_urb->buffer + 434, 0, 480);
		memset(out_urb->buffer + 916, 0, 480);
		memset(out_urb->buffer + 1398, 0, 480);
		memset(out_urb->buffer + 1880, 0, 48);
	}
	spin_unlock_irqrestore(&sub->lock, flags);

	if (do_period_elapsed) {
		snd_pcm_period_elapsed(sub->instance);
	}

	xonedb4_get_midi_output(out_urb->buffer + 432, 2);
	xonedb4_get_midi_output(out_urb->buffer + 914, 2);
	xonedb4_get_midi_output(out_urb->buffer + 1396, 2);
	xonedb4_get_midi_output(out_urb->buffer + 1878, 2);

	ret = usb_submit_urb(&out_urb->instance, GFP_ATOMIC);
	
	if (ret < 0)
		goto out_fail;

	return;

out_fail:
	dev_err(&out_urb->chip->dev->dev, "%s: OUT FAIL\n", __func__);
	rt->panic = true;
}

static int xonedb4_pcm_open(struct snd_pcm_substream *alsa_sub)
{
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
		dev_err(&rt->chip->dev->dev, "%s: Invalid stream type\n", __func__);
		return -EINVAL;
	}

	sub->instance = alsa_sub;
	sub->active = false;
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int xonedb4_pcm_close(struct snd_pcm_substream *alsa_sub)
{
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
			dev_err(&rt->chip->dev->dev, "%s: Invalid samplerate %d\n", __func__, alsa_rt->rate);
			return -EINVAL;
		}

		dev_dbg(&rt->chip->dev->dev, "%s: Samplerate set to %d\n", __func__, alsa_rt->rate);

		ret = xonedb4_pcm_set_rate(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}

		ret = xonedb4_pcm_stream_start(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			dev_err(&rt->chip->dev->dev, "Could not start pcm stream!\n");
			return ret;
		}
	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int xonedb4_pcm_trigger(struct snd_pcm_substream *alsa_sub, int cmd)
{
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
	struct pcm_substream *sub = xonedb4_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;
	snd_pcm_uframes_t dma_offset;

	if (rt->panic || !sub) {
		dev_err(&rt->chip->dev->dev, "%s: Xone XRUN!\n", __func__);
		return SNDRV_PCM_POS_XRUN;
	}

	spin_lock_irqsave(&sub->lock, flags);
	dma_offset = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);

	return bytes_to_frames(alsa_sub->runtime, dma_offset);
}

void xonedb4_pcm_abort(struct xonedb4_chip *chip)
{
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

static int xonedb4_pcm_init_bulk_out_urbs(struct pcm_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(XDB4_PCM_BULK_OUT_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer) {
		return -ENOMEM;
	}

	memset(urb->buffer + 0, 0, 480);
	xonedb4_get_midi_output(urb->buffer + 480, 1);
	memset(urb->buffer + 481, 0xff, 1);
	memset(urb->buffer + 482, 0, 30);
	memset(urb->buffer + 512, 0, 480);
	xonedb4_get_midi_output(urb->buffer + 992, 1);
	memset(urb->buffer + 993, 0xff, 1);
	memset(urb->buffer + 994, 0, 30);
	memset(urb->buffer + 1024, 0, 480);
	xonedb4_get_midi_output(urb->buffer + 1504, 1);
	memset(urb->buffer + 1505, 0xff, 1);
	memset(urb->buffer + 1506, 0, 30);
	memset(urb->buffer + 1536, 0, 480);
	xonedb4_get_midi_output(urb->buffer + 2016, 1);
	memset(urb->buffer + 2017, 0xff, 1);
	memset(urb->buffer + 2018, 0, 30);

	usb_fill_bulk_urb(&urb->instance, chip->dev, usb_sndbulkpipe(chip->dev, ep), (void *)urb->buffer, XDB4_PCM_BULK_OUT_PACKET_SIZE, handler, urb);
	if (usb_urb_ep_type_check(&urb->instance)) {
		dev_err(&chip->dev->dev, "%s: Sanity check failed!\n", __func__);
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);

	return 0;
}

static int xonedb4_pcm_init_int_out_urbs(struct pcm_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(XDB4_PCM_INT_OUT_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer) {
		return -ENOMEM;
	}

	memset(urb->buffer + 0, 0, 432);
	xonedb4_get_midi_output(urb->buffer + 432, 2);
	memset(urb->buffer + 434, 0, 480);
	xonedb4_get_midi_output(urb->buffer + 914, 2);
	memset(urb->buffer + 916, 0, 480);
	xonedb4_get_midi_output(urb->buffer + 1396, 2);
	memset(urb->buffer + 1398, 0, 480);
	xonedb4_get_midi_output(urb->buffer + 1878, 2);
	memset(urb->buffer + 1880, 0, 48);

	usb_fill_int_urb(&urb->instance, chip->dev, usb_sndintpipe(chip->dev, ep), (void *)urb->buffer, XDB4_PCM_INT_OUT_PACKET_SIZE, handler, urb, chip->dev->ep_out[PCM_OUT_EP]->desc.bInterval);
	if (usb_urb_ep_type_check(&urb->instance)) {
		dev_err(&chip->dev->dev, "%s: Sanity check failed!\n", __func__);
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);

	return 0;
}

static int xonedb4_pcm_init_bulk_in_urbs(struct pcm_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(XDB4_PCM_IN_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer) {
		return -ENOMEM;
	}

	usb_fill_bulk_urb(&urb->instance, chip->dev, usb_rcvbulkpipe(chip->dev, ep), (void *)urb->buffer, XDB4_PCM_IN_PACKET_SIZE, handler, urb);
	if (usb_urb_ep_type_check(&urb->instance)) {
		dev_err(&chip->dev->dev, "%s: Sanity check failed!\n", __func__);
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);

	return 0;
}

static int xonedb4_pcm_init_int_in_urbs(struct pcm_urb *urb, struct xonedb4_chip *chip, unsigned int ep, void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(XDB4_PCM_IN_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer) {
		return -ENOMEM;
	}

	usb_fill_int_urb(&urb->instance, chip->dev, usb_rcvintpipe(chip->dev, ep), (void *)urb->buffer, XDB4_PCM_IN_PACKET_SIZE, handler, urb, chip->dev->ep_in[PCM_IN_EP]->desc.bInterval);
	if (usb_urb_ep_type_check(&urb->instance)) {
		dev_err(&chip->dev->dev, "%s: Sanity check failed!\n", __func__);
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);

	return 0;
}

int xonedb4_pcm_init_urbs(struct xonedb4_chip *chip)
{
	uint8_t i;
	int ret;

	struct pcm_runtime *rt = chip->pcm;
	rt->chip = chip;

	for (i = 0; i < PCM_N_URBS; i++) {
		if ((chip->dev->ep_in[PCM_IN_EP]->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
			ret = xonedb4_pcm_init_bulk_in_urbs(&rt->pcm_in_urbs[i], chip, PCM_IN_EP, xonedb4_pcm_in_urb_handler);
		} else if ((chip->dev->ep_in[PCM_IN_EP]->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT) {
			ret = xonedb4_pcm_init_int_in_urbs(&rt->pcm_in_urbs[i], chip, PCM_IN_EP, xonedb4_pcm_in_urb_handler);
		} else {
			goto error;
		}
		if (ret < 0) {
			goto error;
		}
	}

	for (i = 0; i < PCM_N_URBS; i++) {
		if ((chip->dev->ep_out[PCM_OUT_EP]->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK) {
			ret = xonedb4_pcm_init_bulk_out_urbs(&rt->pcm_out_urbs[i], chip, PCM_OUT_EP, xonedb4_pcm_bulk_out_urb_handler);
		} else if ((chip->dev->ep_out[PCM_OUT_EP]->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT) {
			ret = xonedb4_pcm_init_int_out_urbs(&rt->pcm_out_urbs[i], chip, PCM_OUT_EP, xonedb4_pcm_int_out_urb_handler);
		} else {
			goto error;
		}
		if (ret < 0) {
			goto error;
		}
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
	dev_err(&chip->dev->dev, "%s: ERROR\n", __func__);
	mutex_unlock(&rt->stream_mutex);
	for (i = 0; i < PCM_N_URBS; i++)
		kfree(rt->pcm_out_urbs[i].buffer);
	return ret;
}


int xonedb4_pcm_init(struct xonedb4_chip *chip)
{
	int ret;

	struct snd_pcm *pcm;
	struct pcm_runtime *rt = kzalloc(sizeof(struct pcm_runtime), GFP_KERNEL);
	if (!rt) {
		return -ENOMEM;
	}

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;

	mutex_init(&rt->stream_mutex);
	spin_lock_init(&rt->playback.lock);

	ret = snd_pcm_new(chip->card, chip->dev->product, 0, 1, 1, &pcm);
	if (ret < 0) {
		kfree(rt);
		dev_err(&chip->dev->dev, "%s: Cannot create PCM instance\n", __func__);
		return ret;
	}

	pcm->private_data = rt;

	strscpy(pcm->name, chip->dev->product, sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	rt->instance = pcm;
	chip->pcm = rt;

	ret = xonedb4_pcm_init_urbs(chip);

	if (ret < 0) {
		goto error;
	}

	return 0;

	error:
	dev_err(&chip->dev->dev, "%s: ERROR\n", __func__);
	kfree(rt);
	return ret;
}
