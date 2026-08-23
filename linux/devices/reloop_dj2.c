// SPDX-License-Identifier: MIT
/*
 * Reloop Digital Jockey 2 Master Edition (USB 200c:1009)
 *
 * Self-contained device implementation. It shares the Ploytec chipset
 * with the Xone series, so it reuses the read-only protocol helpers and
 * the bit-interleaved codec from common/devices/ploytec/, but keeps its
 * own handshake, packet layout and op table so nothing in the existing
 * Xone code path is affected.
 *
 * Differences from the Xone baseline, all verified against hardware:
 *
 *   - Runs at 44.1 kHz only. The vendor driver hardcodes 0xAC44 for every
 *     200c:1009 branch; 96 kHz audibly distorts playback.
 *   - MIDI output must carry an explicit status byte. The firmware drops
 *     messages sent with MIDI running status, which the ALSA sequencer
 *     bridge emits by default, so no LED ever lights without reassembly.
 *   - Uses the full 8 sub-packets of the 4096-byte bulk output packet.
 *     Ozzy's shared tables describe only the first 4 (2048 bytes) while
 *     reporting 80 frames consumed, which drops half the audio; this
 *     device carries its own tables covering all 80 frames.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <sound/pcm.h>

#include "../ozzy.h"
#include "../ozzy_log.h"
#include "../ozzy_midi.h"
#include "reloop_dj2.h"
#include "../../common/devices/ploytec/ploytec_defs.h"
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../../common/devices/ploytec/ploytec_codec.h"

/* ALSA side: 8 slots x 3 bytes (S24_3LE) */
#define ALSA_FRAME_SIZE   (PLOYTEC_CHANNELS * 3)
#define ALSA_PKT_SIZE     (PLOYTEC_FRAMES_PER_PKT * ALSA_FRAME_SIZE)

/*
 * Bulk output packet: 8 sub-packets of 512 bytes.
 * Each holds 10 device frames (480 bytes), then one MIDI byte at +480,
 * a 0xFF sync byte at +481, and 30 bytes of padding.
 */
#define RELOOP_SUB_SIZE        512
#define RELOOP_SUBS_PER_PKT    8
#define RELOOP_FRAMES_PER_SUB  10
#define RELOOP_MIDI_OFFSET     480
#define RELOOP_SYNC_OFFSET     481
#define RELOOP_SYNC_BYTE       0xFF

/*
 * struct reloop_dj2_private - DMA-safe scratch for vendor requests.
 * usb_control_msg() buffers must not live on the stack.
 */
struct reloop_dj2_private {
	unsigned char firmware_ver[15];
	unsigned char status[1];
	unsigned char xfer_buf[16];

	/*
	 * MIDI output running-status expansion. This firmware ignores
	 * messages that arrive without an explicit status byte, but hosts
	 * are free to use running status -- Mixxx sends one 0x90 and then
	 * a long run of bare note/velocity pairs, none of which light an
	 * LED. Messages are reassembled here with the status byte restored.
	 */
	u8 running_status;   /* last status byte seen from the host */
	u8 outq[3];          /* fully-formed message awaiting transmission */
	u8 outq_len;
	u8 outq_pos;
};

/*
 * reloop_midi_data_len - Number of data bytes following a status byte.
 * Returns 0 for status values this driver does not reassemble.
 */
static unsigned int reloop_midi_data_len(u8 status)
{
	switch (status & 0xF0) {
	case 0x80:	/* note off */
	case 0x90:	/* note on */
	case 0xA0:	/* poly aftertouch */
	case 0xB0:	/* control change */
	case 0xE0:	/* pitch bend */
		return 2;
	case 0xC0:	/* program change */
	case 0xD0:	/* channel aftertouch */
		return 1;
	default:
		return 0;
	}
}

static const unsigned int reloop_dj2_rates[] = { RELOOP_DJ2_SAMPLE_RATE };

/*
 * Log every non-idle MIDI byte sent to the device. Diagnostic only --
 * useful when a host application's LED output does not light anything.
 */
static bool reloop_dj2_debug_midi;
module_param_named(reloop_debug_midi, reloop_dj2_debug_midi, bool, 0644);
MODULE_PARM_DESC(reloop_debug_midi, "Log outbound MIDI bytes (Reloop DJ2)");

/* Pace the firmware; see RELOOP_DJ2_SETTLE_MS. */
static inline void reloop_settle(void)
{
	msleep(RELOOP_DJ2_SETTLE_MS);
}

/* ========================================================================
 * Vendor handshake
 * ======================================================================== */

static int reloop_get_firmware(struct ozzy_chip *chip)
{
	struct reloop_dj2_private *priv = chip->private_data;
	struct ploytec_firmware_version fw;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_FIRMWARE, 0xC0, 0x0000, 0,
			      priv->firmware_ver, 15, 2000);
	if (ret < 0)
		return ret;

	fw = ploytec_parse_firmware(priv->firmware_ver);
	ozzy_log(&chip->dev->dev, "firmware: v1.%d.%d (chip ID: 0x%02X)\n",
		 fw.major, fw.minor, fw.chip_id);
	return 0;
}

static int reloop_get_status(struct ozzy_chip *chip)
{
	struct reloop_dj2_private *priv = chip->private_data;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0xC0, 0x0000,
			      PLOYTEC_REG_AJ_INPUT_SEL,
			      priv->status, 1, 2000);
	if (ret < 0)
		return ret;

	ozzy_log(&chip->dev->dev, "status: 0x%02X\n", priv->status[0]);
	return 0;
}

static int reloop_get_rate(struct ozzy_chip *chip)
{
	struct reloop_dj2_private *priv = chip->private_data;
	uint32_t rate;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_GET_RATE_REQ, PLOYTEC_CMD_GET_RATE_TYPE,
			      0x0100, 0, priv->xfer_buf, 3, 2000);
	if (ret < 0)
		return ret;

	rate = ploytec_decode_rate(priv->xfer_buf);
	ozzy_log(&chip->dev->dev, "hardware sample rate: %u Hz\n", rate);

	if (rate != RELOOP_DJ2_SAMPLE_RATE)
		ozzy_log(&chip->dev->dev, "note: expected %u Hz\n",
			 RELOOP_DJ2_SAMPLE_RATE);

	chip->current_rate = 0;
	return 0;
}

/*
 * reloop_set_rate - Program the sample rate on both stream endpoints.
 * Only index 0 (44.1 kHz) is valid for this device.
 */
static int reloop_set_rate(struct ozzy_chip *chip, unsigned int rate_index)
{
	struct reloop_dj2_private *priv = chip->private_data;
	int ret;

	if (rate_index >= ARRAY_SIZE(reloop_dj2_rates))
		return -EINVAL;

	ploytec_encode_rate(reloop_dj2_rates[rate_index], priv->xfer_buf);

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_SET_RATE_REQ, PLOYTEC_CMD_SET_RATE_TYPE,
			      0x0100, PLOYTEC_EP_RATE_IN, priv->xfer_buf, 3, 2000);
	if (ret < 0)
		return ret;

	/* Precautionary pacing; see RELOOP_DJ2_SETTLE_MS. */
	reloop_settle();

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_SET_RATE_REQ, PLOYTEC_CMD_SET_RATE_TYPE,
			      0x0100, PLOYTEC_EP_RATE_OUT, priv->xfer_buf, 3, 2000);
	if (ret < 0)
		return ret;

	chip->current_rate = rate_index;
	ozzy_log(&chip->dev->dev, "sample rate set: %u Hz\n",
		 reloop_dj2_rates[rate_index]);
	return 0;
}

/*
 * reloop_confirm_status - Read-modify-write the AJ Input Selector to arm
 * the device for streaming.
 */
static int reloop_confirm_status(struct ozzy_chip *chip)
{
	struct reloop_dj2_private *priv = chip->private_data;
	uint16_t wvalue;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0xC0, 0x0000,
			      PLOYTEC_REG_AJ_INPUT_SEL,
			      priv->status, 1, 2000);
	if (ret < 0)
		return ret;

	wvalue = ploytec_confirm_wvalue(priv->status[0]);

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0x40,
			      wvalue, PLOYTEC_REG_AJ_INPUT_SEL,
			      NULL, 0, 2000);
	if (ret >= 0)
		ozzy_log(&chip->dev->dev,
			 "status confirmed (read=0x%02X, wrote wValue=0x%04X)\n",
			 priv->status[0], wvalue);
	return ret;
}

/*
 * reloop_dj2_init - Vendor handshake, paced throughout.
 */
static int reloop_dj2_init(struct ozzy_chip *chip)
{
	struct reloop_dj2_private *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	chip->private_data = priv;

	ozzy_log(&chip->dev->dev, "--- begin handshake sequence ---\n");

	/*
	 * Let the device settle after the alt-setting changes done during
	 * probe before issuing the first control request. Handled here so
	 * the core does not need a delay of its own after interface 1.
	 */
	reloop_settle();

	ret = reloop_get_firmware(chip);
	if (ret < 0)
		goto err;
	reloop_settle();

	ret = reloop_get_status(chip);
	if (ret < 0)
		goto err;
	reloop_settle();

	ret = reloop_get_rate(chip);
	if (ret < 0)
		goto err;
	reloop_settle();

	ret = reloop_set_rate(chip, 0);
	if (ret < 0)
		goto err;
	reloop_settle();

	ret = reloop_get_status(chip);
	if (ret < 0)
		goto err;
	reloop_settle();

	ret = reloop_confirm_status(chip);
	if (ret < 0)
		goto err;
	reloop_settle();

	ozzy_log(&chip->dev->dev, "--- handshake complete, device ready ---\n");
	return 0;

err:
	ozzy_err(&chip->dev->dev, "handshake failed (ret=%d)\n", ret);
	kfree(priv);
	chip->private_data = NULL;
	return ret;
}

static void reloop_dj2_free(struct ozzy_chip *chip)
{
	kfree(chip->private_data);
	chip->private_data = NULL;
}

static int reloop_dj2_reset(struct ozzy_chip *chip)
{
	return usb_reset_device(chip->dev);
}

/* ========================================================================
 * PCM packet processing
 * ======================================================================== */

/*
 * reloop_dj2_process_out_packet - Encode ALSA audio into one bulk packet.
 *
 * Fills all 8 sub-packets, i.e. all PLOYTEC_FRAMES_PER_PKT (80) frames.
 * Anything less would silently discard the frames it does not cover,
 * since the return value reports the full packet as consumed.
 */
static unsigned int reloop_dj2_process_out_packet(struct ozzy_chip *chip,
						  uint8_t *urb_buf,
						  uint8_t *dma_area,
						  unsigned int dma_off,
						  unsigned int pcm_buffer_size)
{
	unsigned int sub, f, src_off, frame = 0;

	for (sub = 0; sub < RELOOP_SUBS_PER_PKT; sub++) {
		unsigned int base = sub * RELOOP_SUB_SIZE;

		for (f = 0; f < RELOOP_FRAMES_PER_SUB; f++, frame++) {
			src_off = dma_off + frame * ALSA_FRAME_SIZE;
			if (src_off >= pcm_buffer_size)
				src_off -= pcm_buffer_size;

			ploytec_encode_frame(urb_buf + base + f * PLOYTEC_OUT_FRAME_SIZE,
					     dma_area + src_off);
		}
	}

	return ALSA_PKT_SIZE;
}

/*
 * reloop_dj2_process_in_packet - Decode one capture packet.
 * Input frames are contiguous 64-byte frames, no gaps.
 */
static unsigned int reloop_dj2_process_in_packet(struct ozzy_chip *chip,
						 uint8_t *urb_buf,
						 uint8_t *dma_area,
						 unsigned int dma_off,
						 unsigned int pcm_buffer_size)
{
	unsigned int f, dst_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		dst_off = dma_off + f * ALSA_FRAME_SIZE;
		if (dst_off >= pcm_buffer_size)
			dst_off -= pcm_buffer_size;

		ploytec_decode_frame(dma_area + dst_off,
				     urb_buf + f * PLOYTEC_IN_FRAME_SIZE);
	}

	return ALSA_PKT_SIZE;
}

/*
 * reloop_dj2_init_out_urb - Silence pattern: zeroed audio, MIDI idle and
 * sync bytes in every sub-packet.
 */
static void reloop_dj2_init_out_urb(struct ozzy_chip *chip, uint8_t *buffer)
{
	unsigned int sub;

	memset(buffer, 0, RELOOP_SUB_SIZE * RELOOP_SUBS_PER_PKT);

	for (sub = 0; sub < RELOOP_SUBS_PER_PKT; sub++) {
		unsigned int base = sub * RELOOP_SUB_SIZE;

		buffer[base + RELOOP_MIDI_OFFSET] = PLOYTEC_MIDI_IDLE_BYTE;
		buffer[base + RELOOP_SYNC_OFFSET] = RELOOP_SYNC_BYTE;
	}
}

/*
 * reloop_midi_drop - Remove n bytes from the front of the send buffer.
 * Unconsumed data stays anchored at index 0, which keeps send_pending a
 * valid append offset for ozzy_midi_out_trigger().
 * Caller must hold rt->out_lock.
 */
static void reloop_midi_drop(struct midi_runtime *rt, unsigned int n)
{
	if (n > rt->send_pending)
		n = rt->send_pending;
	rt->send_pending -= n;
	if (rt->send_pending)
		memmove(rt->send_buffer, rt->send_buffer + n, rt->send_pending);
}

/*
 * reloop_dj2_fill_midi_out - Embed outbound MIDI (button LEDs) in the
 * PCM packet, one byte per sub-packet at offset 480.
 *
 * Only the first slot carries real data; the rest stay idle. At 44.1 kHz
 * that is 44100/80 = ~551 packets/sec, i.e. ~551 MIDI bytes/sec, safely
 * under the 3125 bytes/sec the MIDI wire rate allows.
 *
 * This deliberately does not use ozzy_midi_consume(). That helper walks
 * the send buffer with a read cursor (send_count) while
 * ozzy_midi_out_trigger() appends new data at send_pending, so any write
 * arriving while bytes are still pending lands at the wrong offset and
 * corrupts the queue. Mixxx sends LED state in bursts far larger than one
 * packet's worth, so that collision happens on essentially every update.
 *
 * Instead the unconsumed bytes are kept anchored at index 0, which keeps
 * send_pending a valid append offset for the shared trigger and needs no
 * change to ozzy_midi.c.
 */
static void reloop_dj2_fill_midi_out(struct ozzy_chip *chip, uint8_t *urb_buf)
{
	struct midi_runtime *rt = chip->midi;
	struct reloop_dj2_private *priv = chip->private_data;
	unsigned long flags;
	unsigned int sub;
	u8 out = PLOYTEC_MIDI_IDLE_BYTE;

	if (!rt)
		return;

	spin_lock_irqsave(&rt->out_lock, flags);

	/* Emit the next byte of an already-assembled message. */
	if (priv && priv->outq_pos < priv->outq_len) {
		out = priv->outq[priv->outq_pos++];
		goto unlock;
	}

	/*
	 * Otherwise assemble one complete message, restoring the status
	 * byte when the host used running status. Only consume from the
	 * send buffer once the whole message has arrived, so a partially
	 * received message is never emitted.
	 */
	while (priv && rt->send_pending > 0) {
		u8 first = rt->send_buffer[0];
		unsigned int need, consume;

		if (first >= 0xF8) {
			/* Real-time byte: pass straight through. */
			out = first;
			reloop_midi_drop(rt, 1);
			goto unlock;
		}

		if (first & 0x80) {
			need = reloop_midi_data_len(first);
			if (!need) {
				/* Not reassembled (e.g. sysex): drop it. */
				reloop_midi_drop(rt, 1);
				continue;
			}
			if (rt->send_pending < 1 + need)
				break;          /* wait for the rest */
			priv->running_status = first;
			priv->outq[0] = first;
			priv->outq[1] = rt->send_buffer[1];
			if (need == 2)
				priv->outq[2] = rt->send_buffer[2];
			consume = 1 + need;
		} else {
			if (!priv->running_status) {
				reloop_midi_drop(rt, 1);
				continue;       /* data with no status: drop */
			}
			need = reloop_midi_data_len(priv->running_status);
			if (rt->send_pending < need)
				break;          /* wait for the rest */
			priv->outq[0] = priv->running_status;
			priv->outq[1] = rt->send_buffer[0];
			if (need == 2)
				priv->outq[2] = rt->send_buffer[1];
			consume = need;
		}

		priv->outq_len = need + 1;
		priv->outq_pos = 1;
		out = priv->outq[0];
		reloop_midi_drop(rt, consume);
		goto unlock;
	}

unlock:
	/* Read cursor is unused here; keep it consistent for the shared code. */
	rt->send_count = 0;
	spin_unlock_irqrestore(&rt->out_lock, flags);

	if (reloop_dj2_debug_midi && out != PLOYTEC_MIDI_IDLE_BYTE)
		ozzy_log(&chip->dev->dev, "midi out: %02X\n", out);

	urb_buf[RELOOP_MIDI_OFFSET] = out;

	for (sub = 1; sub < RELOOP_SUBS_PER_PKT; sub++)
		urb_buf[sub * RELOOP_SUB_SIZE + RELOOP_MIDI_OFFSET] =
			PLOYTEC_MIDI_IDLE_BYTE;
}

static unsigned int reloop_dj2_get_out_packet_size(struct ozzy_chip *chip,
						   bool is_bulk)
{
	return RELOOP_SUB_SIZE * RELOOP_SUBS_PER_PKT;
}

/* ========================================================================
 * Exported descriptor
 * ======================================================================== */

const struct ozzy_device_info reloop_dj2_info = {
	.name                  = "Reloop Digital Jockey 2 ME",

	/*
	 * Streams the full 8-slot Ploytec wire frame even though the
	 * hardware is 6 in / 4 out; see reloop_dj2.h.
	 */
	.playback_channels     = PLOYTEC_CHANNELS,
	.capture_channels      = PLOYTEC_CHANNELS,
	.out_packet_size       = RELOOP_SUB_SIZE * RELOOP_SUBS_PER_PKT,
	.in_packet_size        = PLOYTEC_IN_PKT_SIZE,
	.frames_per_out_packet = PLOYTEC_FRAMES_PER_PKT,
	.frames_per_in_packet  = PLOYTEC_FRAMES_PER_PKT,
	.out_ep                = PLOYTEC_EP_PCM_OUT,
	.in_ep                 = PLOYTEC_EP_PCM_IN,
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,

	.midi_in_ep            = PLOYTEC_EP_MIDI_IN,
	.midi_out_embedded     = true,

	.num_interfaces        = PLOYTEC_NUM_INTERFACES,
	.alt_setting           = PLOYTEC_ALT_SETTING,

	.rates                 = reloop_dj2_rates,
	.num_rates             = ARRAY_SIZE(reloop_dj2_rates),
	.rates_mask            = SNDRV_PCM_RATE_44100,
	.rate_min              = RELOOP_DJ2_SAMPLE_RATE,
	.rate_max              = RELOOP_DJ2_SAMPLE_RATE,
};

const struct ozzy_device_ops reloop_dj2_ops = {
	.init                = reloop_dj2_init,
	.free                = reloop_dj2_free,
	.set_rate            = reloop_set_rate,
	.reset               = reloop_dj2_reset,
	.process_out_packet  = reloop_dj2_process_out_packet,
	.process_in_packet   = reloop_dj2_process_in_packet,
	.init_out_urb        = reloop_dj2_init_out_urb,
	.fill_midi_out       = reloop_dj2_fill_midi_out,
	.get_out_packet_size = reloop_dj2_get_out_packet_size,
};
