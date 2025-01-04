#ifndef XONEDB4_MIDI_H
#define XONEDB4_MIDI_H

struct xonedb4_chip;

int xonedb4_midi_init(struct xonedb4_chip *chip);
int xonedb4_midi_init_bulk_urbs(struct xonedb4_chip *chip);
void xonedb4_get_midi_output(u8 *buffer, int count);
void xonedb4_midi_abort(struct xonedb4_chip *chip);
#endif /* XONEDB4_MIDI_H */
