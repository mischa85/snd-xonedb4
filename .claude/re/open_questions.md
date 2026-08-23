# Ploytec RE — Open Questions and Leads

## Open Questions

### Interrupt output path has the same half-packet bug as bulk (UNFIXED)
`ploytec_int_subpackets[]` covers 40 frames / 1928 bytes, but
`PLOYTEC_INT_OUT_PKT_SIZE` is 3856 (= 2 x 1928) and
`ploytec_process_out_int()` reports `PLOYTEC_FRAMES_PER_PKT` (80) frames
consumed per packet. So half of every interrupt packet's audio is pulled
from the ALSA ring and dropped, exactly as the bulk path did before it was
fixed. Affects Xone:DB4 on firmware 1.4.1+ (which exposes interrupt
endpoints instead of bulk).

The layout repeats every 1928 bytes / 40 frames, so the fix is mechanical:
duplicate the five groups and four MIDI slots at +1928 bytes / +40 frames
and bump `PLOYTEC_INT_NUM_SUBPACKETS` to 10 and
`PLOYTEC_INT_NUM_MIDI_SLOTS` to 8. Left undone because no interrupt-mode
hardware was available to verify it.


### Sample rate change: 5-call dance vs official driver
Our driver sends SET_CUR 5 times alternating 0x86/0x05 (from Windows USB capture). The official
macOS driver only calls `setFrequency` once per pipe (2 total: input + output). The 5-call
pattern may be a Windows driver quirk, or redundant. Need to test if 2 calls works for all devices.

### Junction encoder vs our bit-interleaved codec
`pcmTo24Junction6CH` does **channel reordering** (0→0, 3→1, 1→2, 4→3, 2→4, 5→5) and
extracts 3 bytes from 4-byte int samples. It does NOT do bit-level interleaving.
Our codec (`ploytec_codec.h`) does bit-interleaved encoding. Need to verify these produce
the same wire format, or if our codec is wrong.

### Index 0 bits 3-7 (0x08–0x80)
- For product 0x644: bits 3-4 (mask `0x18`) come from `this[0xd2]`, rest preserved with `& 0xE7`
- ESU uses fixed composite states (0x32, 0xB0, 0xB2) rather than individual flags
- Bits 0-1 now understood (USB1 mode and clock source), but upper bits still device-dependent

### PGNoiseGenerator anti-tamper
`onServiceStart` uses a PRNG seeded with VID/PID to verify expected values. This is a
license/anti-clone check. Not relevant to our driver but interesting.

### AJ subsystem
`AJ::configurationDone`, `AJ::sendCPort`, `AJ::getLevelInfo` — purpose unclear. May be
related to level metering or some control protocol. Has its own pipe assignment via `AJ::assignPipes`.

## Resolved (moved from questions)

- ~~Junction encoder vs our bit-interleaved codec~~ — The bit-interleaved
  codec in `ploytec_codec.c` is correct for bulk devices. Confirmed by
  playing tones through a Reloop DJ2 ME (200c:1009) from userspace: plain
  3-byte packing produces noise, the bit-interleaved 48-byte frame produces
  a clean tone. The `pcmTo24Junction*` encoders belong to the **isoc** path
  (`chooseISOOutEncoder`), which bulk devices never execute — an easy trap
  when reading the Windows driver.
- ~~Bulk output packet coverage~~ — `ploytec_bulk_subpackets[]` and
  `ploytec_bulk_midi_slots[]` only described 4 sub-packets (40 frames /
  2048 bytes) while `PLOYTEC_FRAMES_PER_PKT` is 80 and
  `PLOYTEC_BULK_OUT_PKT_SIZE` is 4096. `ploytec_process_out_bulk()` reported
  80 frames consumed per packet, so half of every packet's audio was pulled
  from the ALSA ring and silently dropped, and bytes 2048-4095 were never
  filled. Heard as broadband distortion at all frequencies. Both tables now
  cover all 8 sub-packets. **This affected the Xone devices too, not just
  the Reloop.**

- ~~Vendor request 'A'~~ — Now understood: ESU clock config (wIndex=0x101) and Wolfson codec (wIndex=0x102/0x106)
- ~~Index 2 digital output selector~~ — Write-only, `AjExtData[4] & 0xFFFF`, only for devices with `this[0x7AC]`
- ~~setEsuCpldByte~~ — Writes to 'I' wIndex=1 with `| 0xE7` mask
- ~~this[0x1888] meaning~~ — USB 2.0 High Speed flag, NOT bulk device flag
- ~~wValue high byte~~ — Sign extension artifact from `(short)(char)byte` cast

## Leads

- **pcmTo24Junction8CH** — decompile to verify our 8-channel codec against the official encoder
- **USB::buildDummyDescriptor** — how the driver synthesizes USB audio descriptors for Ploytec devices
- **sendElektronChannelMap** — USB transactions for Elektron channel config (for Elektron support)
- **rtsProcessBulkOut** — the actual real-time audio processing loop (partially decompiled, shows MIDI embedding)
- **Real-time service functions**: `rtsBulkOutBulkIn`, `rtsInterruptOutInterruptIn` — the streaming state machines
- Windows driver RE for sample rate change pattern comparison
