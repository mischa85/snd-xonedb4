#ifndef PloytecSharedData_h
#define PloytecSharedData_h

#include <atomic>
#include <cstdint>

#define kPloytecVendorID            0x0A4A
#define kPloytecPID_DB4             0xFFDB
#define kPloytecPID_DB2             0xFFD2
#define kPloytecPID_DX              0xFFDD
#define kPloytecPID_4D              0xFF4D

#define kPloytecSharedMemName       "/hackerman.ploytecsharedmem"
#define kMidiRingSize               1024 
#define kMidiRingMask               (kMidiRingSize - 1)

struct MidiRingBuffer {
	std::atomic<uint32_t> writeIndex;
	uint8_t _pad1[64];
	std::atomic<uint32_t> readIndex;
	uint8_t _pad2[64];
	uint8_t buffer[kMidiRingSize];
};

struct PloytecSharedMemory {
	uint32_t magic;      // 0x504C4F59
	uint32_t version;
	uint32_t sessionID; 
	char productName[64]; 
	
	std::atomic<bool> audioDriverReady;
	MidiRingBuffer midiOut;
	MidiRingBuffer midiIn;
};

#endif