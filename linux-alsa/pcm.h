#ifndef XONEDB4_PCM_H
#define XONEDB4_PCM_H

struct xonedb4_chip;

int xonedb4_pcm_init(struct xonedb4_chip *chip);
int xonedb4_pcm_init_bulk_urbs(struct xonedb4_chip *chip);
void xonedb4_pcm_abort(struct xonedb4_chip *chip);
#endif /* XONEDB4_PCM_H */
