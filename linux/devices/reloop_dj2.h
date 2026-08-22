/* SPDX-License-Identifier: MIT */
/*
 * Reloop Digital Jockey 2 Master Edition (USB 200c:1009)
 *
 * Ploytec-based DJ controller with an integrated 6-in / 4-out 24-bit
 * audio interface, reverse engineered from USB traffic and the vendor's
 * Windows driver (rldj2meu.sys).
 */

#ifndef OZZY_RELOOP_DJ2_H
#define OZZY_RELOOP_DJ2_H

#include "../ozzy.h"

/* USB identity */
#define RELOOP_DJ2_VENDOR_ID    0x200C
#define RELOOP_DJ2_PRODUCT_ID   0x1009

/*
 * Hardware channel counts, from findInterfacesInConfig() in the vendor
 * driver: every 200c:1009 branch sets in=6 / out=4 at 24-bit.
 *
 * The Ploytec bulk wire frame always carries 8 slots per direction, so
 * the driver still streams 8 channels: surplus playback slots are
 * ignored by the device and surplus capture slots read as silence.
 *
 * Verified against hardware -- a loopback capture showed signal on
 * in 0..3 (Line In 1 and 2), the mic noise floor on in 4..5, and
 * digital silence on in 6..7.
 */
#define RELOOP_DJ2_HW_IN_CHANNELS   6
#define RELOOP_DJ2_HW_OUT_CHANNELS  4

/*
 * Capture channel map (matches "Input Routing" in the printed manual):
 *   in 0/1  Line In 1
 *   in 2/3  Line In 2
 *   in 4/5  Mic
 *   in 6/7  unused, always silent
 */

/*
 * This device runs at 44.1 kHz only. The vendor driver hardcodes 0xAC44
 * for every 200c:1009 branch -- the sibling 200c:1005 is the model that
 * gets 96 kHz. Running this DAC at 96 kHz audibly distorts playback.
 */
#define RELOOP_DJ2_SAMPLE_RATE      44100

/*
 * Precautionary pacing between handshake control requests, applied only
 * inside this device's own init path.
 *
 * NOT proven necessary: 25+ userspace iterations with no pacing at all,
 * in either SET_CUR order, completed the handshake without the device
 * dropping off the bus. It is kept because the one kernel probe that
 * hung the machine had no pacing, and the cost is ~1 s at probe time
 * only. The hang is now better explained by an invalid clear_halt on the
 * isochronous endpoint and by the 96 kHz default, both since fixed.
 */
#define RELOOP_DJ2_SETTLE_MS        200

/*
 * MIDI/LED notes
 *
 * Controls and LEDs use the shared Ploytec bulk transport: input arrives
 * on the dedicated MIDI IN endpoint, output is embedded one byte per
 * sub-packet at offset 480, padded with 0xFD.
 *
 * Output needs one device-specific fixup: this firmware ignores messages
 * that arrive without an explicit status byte. Hosts may legitimately use
 * MIDI running status -- Mixxx sends a single 0x90 followed by a long run
 * of bare note/velocity pairs -- and every one of those is dropped by the
 * device. reloop_dj2_fill_midi_out() reassembles messages with the status
 * byte restored. Without it, MIDI input works fine while no LED ever lights.
 *
 * The note/CC assignments themselves live in the host mapping (see the
 * "MIDI Control Values" table in the manual), not in the driver: LED
 * feedback is simply the same note echoed back to the device.
 */

/*
 * Self-contained: own handshake, own packet layout, own op table. Only
 * the read-only protocol helpers and bit-interleaved codec are shared
 * with the Xone devices, so their code path is untouched.
 */
extern const struct ozzy_device_info reloop_dj2_info;
extern const struct ozzy_device_ops reloop_dj2_ops;

#endif /* OZZY_RELOOP_DJ2_H */
