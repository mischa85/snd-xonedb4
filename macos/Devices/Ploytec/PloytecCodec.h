#ifndef PloytecCodec_h
#define PloytecCodec_h

#include <stdint.h>

// Ploytec-specific PCM encoding/decoding
// 8 channels of 24-bit audio packed in proprietary bit-interleaved format

void PloytecEncodePCM(uint8_t* dst, const float* src);
void PloytecDecodePCM(float* dst, const uint8_t* src);

// Packet-level I/O operations (handle MIDI byte interleaving)
typedef void (*WriteOutputFunc)(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ReadInputFunc)(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
typedef void (*ClearOutputFunc)(uint8_t* outputBuffer, uint32_t bufferSize);

void PloytecWriteOutputBulk(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
void PloytecWriteOutputInterrupt(uint8_t* ringBuffer, const float* srcFrames, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);
void PloytecReadInput(float* dstFrames, const uint8_t* ringBuffer, uint64_t sampleTime, uint32_t frameCount, uint32_t ringSize, uint32_t bytesPerFrame);

void PloytecClearOutputBulk(uint8_t* outputBuffer, uint32_t bufferSize);
void PloytecClearOutputInterrupt(uint8_t* outputBuffer, uint32_t bufferSize);

#endif
