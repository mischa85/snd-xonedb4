#ifndef PloytecSharedData_h
#define PloytecSharedData_h

#include <stdint.h>

// --- KERNEL COMPATIBILITY SHIM ---
// The Kernel does not have <atomic>. We map std::atomic to volatile 
// to preserve the memory layout and binary compatibility with User Space.
#ifdef KERNEL
    typedef volatile uint32_t AtomicU32;
    typedef volatile uint64_t AtomicU64;
    typedef volatile bool     AtomicBool;
    #define CACHE_ALIGN __attribute__((aligned(64)))
#else
    #include <atomic>
    typedef std::atomic<uint32_t> AtomicU32;
    typedef std::atomic<uint64_t> AtomicU64;
    typedef std::atomic<bool>     AtomicBool;
    #define CACHE_ALIGN alignas(64)
#endif

// --- YOUR CONTRACT ---
#define kPloytecVendorID            0x0A4A
#define kPloytecPID_DB4             0xFFDB
#define kPloytecPID_DB2             0xFFD2
#define kPloytecPID_DX              0xFFDD
#define kPloytecPID_4D              0xFF4D

#define kPloytecSharedMemName       "/ploytecsharedmem"
#define kMidiRingSize               1024 
#define kMidiRingMask               (kMidiRingSize - 1)

#define kBulkPacketSizeOut          4096 
#define kInterruptPacketSizeOut     3856 
#define kPacketSizeIn               5120 
#define kFramesPerPacket            80

#define kNumPackets                 128
#define kPacketMask                 (kNumPackets - 1)
#define kZeroTimestampPeriod        640

static const uint8_t kPcmOutEp = 0x05;
static const uint8_t kPcmInEp = 0x86;
static const uint8_t kMidiInEp = 0x83;
static const uint32_t kDefaultUrbs = 2;

struct AudioRingBuffer {
	AtomicBool hardwarePresent;
	AtomicBool driverReady;
	AtomicU32  sampleRate;
	AtomicBool isBulkMode;

	struct {
		AtomicU32 sequence; 
		AtomicU64 sampleTime; 
		AtomicU64 hostTime;   
	} timestamp;

	uint8_t _pad1[64]; 

	CACHE_ALIGN AtomicU64 halWritePosition;
	
	uint8_t _pad2[64];

	uint8_t inputBuffer[kPacketSizeIn * kNumPackets];
	uint8_t outputBuffer[kBulkPacketSizeOut * kNumPackets];
};

struct MidiRingBuffer {
	CACHE_ALIGN AtomicU32 writeIndex;
	uint8_t _pad1[64];
	CACHE_ALIGN AtomicU32 readIndex;
	uint8_t _pad2[64];
	uint8_t buffer[kMidiRingSize];
};

struct PloytecSharedMemory {
	uint32_t magic;
	uint32_t version;
	uint32_t sessionID; 

	// Kernel cannot use C++ initializers like { 0 }
#ifdef KERNEL
	AtomicU32 heartbeat;
#else
	AtomicU32 heartbeat { 0 };
#endif

	char manufacturerName[64];
	char productName[64];
	char serialNumber[64];
	
	MidiRingBuffer midiOut;
	MidiRingBuffer midiIn;
	
	CACHE_ALIGN AudioRingBuffer audio;
};

#endif