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

#define kBulkPacketSizeOut          4096 
#define kInterruptPacketSizeOut     3856 
#define kPacketSizeIn               5120 
#define kFramesPerPacket            80

#define kNumPackets                 128
#define kPacketMask                 (kNumPackets - 1)
#define kZeroTimestampPeriod        640

#define CACHE_ALIGN alignas(64)

struct AudioRingBuffer {
	std::atomic<bool> hardwarePresent;
	std::atomic<bool> driverReady;
	std::atomic<uint32_t> sampleRate;
	std::atomic<bool> isBulkMode;

	struct {
		std::atomic<uint32_t> sequence; 
		std::atomic<uint64_t> sampleTime; 
		std::atomic<uint64_t> hostTime;   
	} timestamp;

	uint8_t _pad1[64]; 

	CACHE_ALIGN std::atomic<uint64_t> halWritePosition;
	
	uint8_t _pad2[64];

	uint8_t inputBuffer[kPacketSizeIn * kNumPackets];
	uint8_t outputBuffer[kBulkPacketSizeOut * kNumPackets];
};

struct MidiRingBuffer {
	CACHE_ALIGN std::atomic<uint32_t> writeIndex;
	uint8_t _pad1[64];
	CACHE_ALIGN std::atomic<uint32_t> readIndex;
	uint8_t _pad2[64];
	uint8_t buffer[kMidiRingSize];
};

struct PloytecSharedMemory {
	uint32_t magic;
	uint32_t version;
	uint32_t sessionID; 
	
	char manufacturerName[64];
	char productName[64];
	char serialNumber[64];
	
	MidiRingBuffer midiOut;
	MidiRingBuffer midiIn;
	
	CACHE_ALIGN AudioRingBuffer audio;
};

#endif