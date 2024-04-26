# snd-xonedb4

This is a driver for the Allen & Heath Xone:DB4 mixer.

It currently only supports Linux. Once I get the issues ironed out I will also get get into macOS and Windows.

Things that work:

- PCM out 8 channels
- PCM in 8 channels
- MIDI out

Things that don't work:

- MIDI in (ISO endpoint 2?)

The official drivers do find a way to get MIDI Sync to the mixer.