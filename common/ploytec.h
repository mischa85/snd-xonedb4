#ifndef PLOYTEC_H
#define PLOYTEC_H

void ploytec_convert_from_s24_3le(uint8_t *dest, uint8_t *src);
void ploytec_convert_to_s24_3le(uint8_t *dest, uint8_t *src);
void ploytec_sync_bytes(uint8_t *dest);
#endif /* PLOYTEC_H */
