#include "PloytecAudio.h"
#include <CoreAudio/CoreAudio.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define PLOYTEC_PCM_OUT_FRAME_SIZE 48
#define PLOYTEC_PCM_IN_FRAME_SIZE 64

static os_log_t GetLog() {
	static os_log_t log = os_log_create("hackerman.ploytecaudio", "plugin");
	return log;
}

static UInt32 GetAudioBufferListSize(UInt32 numBuffers) {
	return (UInt32)(offsetof(AudioBufferList, mBuffers) + (numBuffers * sizeof(AudioBuffer)));
}

static bool IsOutputScope(AudioObjectPropertyScope scope) {
	return (scope == kAudioObjectPropertyScopeOutput || scope == kAudioObjectPropertyScopeGlobal);
}

static OSStatus WriteCFString(UInt32 inMax, UInt32* outSize, void* outData, CFStringRef s) {
	if (outData && inMax < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
	if (outData) { *(CFStringRef*)outData = s; CFRetain(s); }
	if (outSize) *outSize = sizeof(CFStringRef);
	return kAudioHardwareNoError;
}

static OSStatus WriteClassID(UInt32 inMax, UInt32* outSize, void* outData, AudioClassID v) {
	if (outData && inMax < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
	if (outData) *(AudioClassID*)outData = v;
	if (outSize) *outSize = sizeof(AudioClassID);
	return kAudioHardwareNoError;
}

static OSStatus WriteUInt32(UInt32 inMax, UInt32* outSize, void* outData, UInt32 v) {
	if (outData && inMax < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
	if (outData) *(UInt32*)outData = v;
	if (outSize) *outSize = sizeof(UInt32);
	return kAudioHardwareNoError;
}

static OSStatus WriteFloat64(UInt32 inMax, UInt32* outSize, void* outData, Float64 v) {
	if (outData && inMax < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
	if (outData) *(Float64*)outData = v;
	if (outSize) *outSize = sizeof(Float64);
	return kAudioHardwareNoError;
}

static OSStatus WriteObjectID(UInt32 inMax, UInt32* outSize, void* outData, AudioObjectID v) {
	if (outData && inMax < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
	if (outData) *(AudioObjectID*)outData = v;
	if (outSize) *outSize = sizeof(AudioObjectID);
	return kAudioHardwareNoError;
}

PloytecAudio& PloytecAudio::Get() {
	static PloytecAudio instance;
	return instance;
}

void PloytecAudio::Init(AudioServerPlugInHostRef host) {
	os_log(GetLog(), "[PloytecAudio] Init");
	mHost = host;

	mDeviceUID = CFSTR("hackerman.ploytecaudio.device");
	mModelUID = CFSTR("Ploytec Audio Device");
	mManufacturerUID = CFSTR("Ploytec");
	mZeroTimestampPeriod = kZeroTimestampPeriod;

	mCurrentStreamFormat.mSampleRate = 96000.0;
	mCurrentStreamFormat.mFormatID = kAudioFormatLinearPCM;
	mCurrentStreamFormat.mFormatFlags = kAudioFormatFlagIsFloat;
	mCurrentStreamFormat.mBytesPerPacket = 32;
	mCurrentStreamFormat.mFramesPerPacket = 1;
	mCurrentStreamFormat.mBytesPerFrame = 32;
	mCurrentStreamFormat.mChannelsPerFrame = 8;
	mCurrentStreamFormat.mBitsPerChannel = 32;

	mAvailableSampleRates.mMinimum = 96000.0; 
	mAvailableSampleRates.mMaximum = 96000.0;

	RebuildStreamConfigs();

	mMonitorRunning = true;
	pthread_create(&mMonitorThread, NULL, MonitorEntry, this);
}

void PloytecAudio::Cleanup() {
	mMonitorRunning = false;
	pthread_join(mMonitorThread, NULL);
	UnmapSharedMemory();
}

void PloytecAudio::ClearOutputBuffer() {
	if (!mOutputBufferAddr || !mSHM) return;
	const bool bulk = mSHM->audio.isBulkMode.load(std::memory_order_relaxed);
	const uint32_t stride = bulk ? 512 : 482, pcm1 = bulk ? 480 : 432;
	const uint32_t limit = kNumPackets * (bulk ? kBulkPacketSizeOut : kInterruptPacketSizeOut);
	for (uint32_t i = 0; i < limit; i += stride) {
		memset(mOutputBufferAddr + i, 0, pcm1);
		if (!bulk) memset(mOutputBufferAddr + i + pcm1 + 2, 0, 48);
	}
}

void PloytecAudio::RebuildStreamConfigs() {
	const uint32_t kDefaultIOFrames = 512;
	const uint32_t kBytesPerFrame = mCurrentStreamFormat.mBytesPerFrame;

	mInputConfig.mNumberBuffers = 1;
	mInputConfig.mBuffers[0].mNumberChannels = 8;
	mInputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * kBytesPerFrame;
	mInputConfig.mBuffers[0].mData = nullptr;

	mOutputConfig.mNumberBuffers = 1;
	mOutputConfig.mBuffers[0].mNumberChannels = 8;
	mOutputConfig.mBuffers[0].mDataByteSize = kDefaultIOFrames * kBytesPerFrame;
	mOutputConfig.mBuffers[0].mData = nullptr;
}

void PloytecAudio::MapSharedMemory() {
	if (mSHM) return;
	
	mSHMFd = shm_open(kPloytecSharedMemName, O_RDWR, 0666);
	if (mSHMFd == -1) return; 

	struct stat sb;
	if (fstat(mSHMFd, &sb) == 0) {
		mSHMInode = sb.st_ino;
	} else {
		close(mSHMFd); mSHMFd = -1; return;
	}

	void* ptr = mmap(NULL, sizeof(PloytecSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, mSHMFd, 0);
	if (ptr == MAP_FAILED) { close(mSHMFd); mSHMFd = -1; return; }

	mSHM = (PloytecSharedMemory*)ptr;
	if (mSHM->magic != 0x504C4F59) {
		os_log_error(GetLog(), "[PloytecAudio] SHM Magic Mismatch!");
		UnmapSharedMemory();
		return;
	}

	mLastSessionID = mSHM->sessionID;
	mInputBufferAddr = mSHM->audio.inputBuffer;
	mOutputBufferAddr = mSHM->audio.outputBuffer;
	
	// Determine IO Handler based on Mode Flag
	if (mSHM->audio.isBulkMode.load(std::memory_order_relaxed)) {
		mActiveIOHandler = &PloytecAudio::ioOperationBulk;
		os_log(GetLog(), "[PloytecAudio] Mode: BULK");
	} else {
		mActiveIOHandler = &PloytecAudio::ioOperationInterrupt;
		os_log(GetLog(), "[PloytecAudio] Mode: INTERRUPT");
	}
	
	os_log(GetLog(), "[PloytecAudio] Attached (Session: 0x%08X)", mSHM->sessionID);
}

void PloytecAudio::UnmapSharedMemory() {
	if (mSHM) { munmap(mSHM, sizeof(PloytecSharedMemory)); mSHM = nullptr; }
	if (mSHMFd != -1) { close(mSHMFd); mSHMFd = -1; }
	mSHMInode = 0;
	mInputBufferAddr = nullptr;
	mOutputBufferAddr = nullptr;
}

bool PloytecAudio::CheckSharedMemoryValidity() {
	if (!mSHM) return false;
	int tempFd = shm_open(kPloytecSharedMemName, O_RDONLY, 0666);
	if (tempFd == -1) return false;

	struct stat sb;
	if (fstat(tempFd, &sb) != 0 || sb.st_ino != mSHMInode) {
		close(tempFd); return false;
	}

	void* tempPtr = mmap(NULL, sizeof(PloytecSharedMemory), PROT_READ, MAP_SHARED, tempFd, 0);
	close(tempFd); 

	if (tempPtr == MAP_FAILED) return false;
	PloytecSharedMemory* tempSHM = (PloytecSharedMemory*)tempPtr;
	bool sessionMatch = (tempSHM->sessionID == mLastSessionID);
	munmap(tempPtr, sizeof(PloytecSharedMemory));
	return sessionMatch;
}

void* PloytecAudio::MonitorEntry(void* arg) {
	((PloytecAudio*)arg)->MonitorLoop();
	return NULL;
}

void PloytecAudio::MonitorLoop() {
	while (mMonitorRunning) {
		if (!mSHM) {
			MapSharedMemory();
		} else {
			if (!CheckSharedMemoryValidity()) {
				UnmapSharedMemory();
				mLastConnectedState = false; 
				usleep(100000); 
				continue;
			}
		}

		bool currentlyConnected = (mSHM && mSHM->audio.hardwarePresent.load());

		if (currentlyConnected != mLastConnectedState) {
			mLastConnectedState = currentlyConnected;
			os_log(GetLog(), "[PloytecAudio] Connection: %s", currentlyConnected ? "UP" : "DOWN");
			
			if (mHost) {
				AudioObjectPropertyAddress addr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
				mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &addr);
				
				if (currentlyConnected && mSHM->productName[0] != 0) {
					if (mModelUID) CFRelease(mModelUID);
					mModelUID = CFStringCreateWithCString(NULL, mSHM->productName, kCFStringEncodingUTF8);
				}
			}
		}
		usleep(250000);
	}
}

bool PloytecAudio::IsConnected() { return mLastConnectedState; }

kern_return_t PloytecAudio::StartIO() {
	if (!IsConnected()) return kAudioHardwareNotRunningError;
	if (mSHM) {
		mSHM->audio.halWritePosition.store(0, std::memory_order_relaxed);
		// Update Handler Pointer just in case mode changed across reconnects
		if (mSHM->audio.isBulkMode.load(std::memory_order_relaxed)) {
			mActiveIOHandler = &PloytecAudio::ioOperationBulk;
		} else {
			mActiveIOHandler = &PloytecAudio::ioOperationInterrupt;
		}
	}
	os_log(GetLog(), "[PloytecAudio] StartIO");
	mIOStarted = true;
	mTimestampSeed++;
	return kIOReturnSuccess;
}

kern_return_t PloytecAudio::StopIO() {
	os_log(GetLog(), "[PloytecAudio] StopIO");
	mIOStarted = false;
	mTimestampSeed++;
	ClearOutputBuffer();
	return kIOReturnSuccess;
}

HRESULT PloytecAudio::GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
	if (!mSHM) return kAudioHardwareNotRunningError;

	auto& ts = mSHM->audio.timestamp;
	uint64_t sTime, hTime;
	uint32_t seq1, seq2;
	int retries = 0;

	do {
		seq1 = ts.sequence.load(std::memory_order_acquire);
		if (seq1 & 1) {
			std::atomic_thread_fence(std::memory_order_acquire);
			continue;
		}
		sTime = ts.sampleTime.load(std::memory_order_relaxed);
		hTime = ts.hostTime.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire); 
		seq2 = ts.sequence.load(std::memory_order_relaxed);
	} while (seq1 != seq2 && ++retries < 100);

	if (outSampleTime) *outSampleTime = (Float64)sTime;
	if (outHostTime) *outHostTime = hTime;
	if (outSeed) *outSeed = mTimestampSeed;
	
	return kAudioHardwareNoError;
}

// -----------------------------------------------------------------------------
// OPTIMIZED IO HANDLERS
// -----------------------------------------------------------------------------

OSStatus PloytecAudio::ioOperationBulk(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
	if (!mSHM || !mInputBufferAddr) return kAudioHardwareNoError;
	const uint32_t ringSize = kNumPackets * kFramesPerPacket;

	if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
		const float* srcBuffer = (float*)ioMainBuffer;
		uint8_t* dstBase = mOutputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;

		for (uint32_t i = 0; i < inIOBufferFrameSize; i++) {
			uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
			uint32_t byteOffset = (sampleOffset >= 10) ? ((sampleOffset - 10) / 10) * 32 + 32 : 0;
			EncodePloytecPCM(dstBase + byteOffset + (sampleOffset * PLOYTEC_PCM_OUT_FRAME_SIZE), srcBuffer + (i * 8));
		}
		mSHM->audio.halWritePosition.store(sampleTime + inIOBufferFrameSize, std::memory_order_release);
	}
	else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
		float* dstPtr = (float*)ioMainBuffer;
		uint8_t* baseSrc = mInputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;

		for (uint32_t i = 0; i < inIOBufferFrameSize; i++) {
			uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
			DecodePloytecPCM(dstPtr + (i * 8), baseSrc + (sampleOffset * PLOYTEC_PCM_IN_FRAME_SIZE));
		}
	}
	return kAudioHardwareNoError;
}

OSStatus PloytecAudio::ioOperationInterrupt(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
	if (!mSHM || !mInputBufferAddr) return kAudioHardwareNoError;
	const uint32_t ringSize = kNumPackets * kFramesPerPacket;

	if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
		const float* srcBuffer = (float*)ioMainBuffer;
		uint8_t* dstBase = mOutputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;

		for (uint32_t i = 0; i < inIOBufferFrameSize; i++) {
			uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
			uint32_t byteOffset = (sampleOffset >= 9) ? ((sampleOffset - 9) / 10) * 2 + 2 : 0;
			EncodePloytecPCM(dstBase + byteOffset + (sampleOffset * PLOYTEC_PCM_OUT_FRAME_SIZE), srcBuffer + (i * 8));
		}
		mSHM->audio.halWritePosition.store(sampleTime + inIOBufferFrameSize, std::memory_order_release);
	}
	else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
		float* dstPtr = (float*)ioMainBuffer;
		uint8_t* baseSrc = mInputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;

		for (uint32_t i = 0; i < inIOBufferFrameSize; i++) {
			uint32_t sampleOffset = (uint32_t)((sampleTime + i) % ringSize);
			DecodePloytecPCM(dstPtr + (i * 8), baseSrc + (sampleOffset * PLOYTEC_PCM_IN_FRAME_SIZE));
		}
	}
	return kAudioHardwareNoError;
}

void PloytecAudio::EncodePloytecPCM(uint8_t *dst, const float *src) {
	int32_t s[8];
	for(int i=0; i<8; i++) {
		s[i] = (int32_t)(src[i] * 8388608.0f);
		if(src[i] > 1.0f) s[i] = 0x7FFFFF; else if(src[i] < -1.0f) s[i] = -0x800000;
	}
	uint8_t c[8][3];
	for(int i=0; i<8; i++) {
		c[i][0] = s[i] & 0xFF; c[i][1] = (s[i] >> 8) & 0xFF; c[i][2] = (s[i] >> 16) & 0xFF;
	}
	const uint8_t c1_L = c[0][0]; const uint8_t c1_M = c[0][1]; const uint8_t c1_H = c[0][2];
	const uint8_t c2_L = c[1][0]; const uint8_t c2_M = c[1][1]; const uint8_t c2_H = c[1][2];
	const uint8_t c3_L = c[2][0]; const uint8_t c3_M = c[2][1]; const uint8_t c3_H = c[2][2];
	const uint8_t c4_L = c[3][0]; const uint8_t c4_M = c[3][1]; const uint8_t c4_H = c[3][2];
	const uint8_t c5_L = c[4][0]; const uint8_t c5_M = c[4][1]; const uint8_t c5_H = c[4][2];
	const uint8_t c6_L = c[5][0]; const uint8_t c6_M = c[5][1]; const uint8_t c6_H = c[5][2];
	const uint8_t c7_L = c[6][0]; const uint8_t c7_M = c[6][1]; const uint8_t c7_H = c[6][2];
	const uint8_t c8_L = c[7][0]; const uint8_t c8_M = c[7][1]; const uint8_t c8_H = c[7][2];

	dst[0x00] = ((c1_H & 0x80) >> 0x07) | ((c3_H & 0x80) >> 0x06) | ((c5_H & 0x80) >> 0x05) | ((c7_H & 0x80) >> 0x04);
	dst[0x01] = ((c1_H & 0x40) >> 0x06) | ((c3_H & 0x40) >> 0x05) | ((c5_H & 0x40) >> 0x04) | ((c7_H & 0x40) >> 0x03);
	dst[0x02] = ((c1_H & 0x20) >> 0x05) | ((c3_H & 0x20) >> 0x04) | ((c5_H & 0x20) >> 0x03) | ((c7_H & 0x20) >> 0x02);
	dst[0x03] = ((c1_H & 0x10) >> 0x04) | ((c3_H & 0x10) >> 0x03) | ((c5_H & 0x10) >> 0x02) | ((c7_H & 0x10) >> 0x01);
	dst[0x04] = ((c1_H & 0x08) >> 0x03) | ((c3_H & 0x08) >> 0x02) | ((c5_H & 0x08) >> 0x01) | ((c7_H & 0x08) >> 0x00);
	dst[0x05] = ((c1_H & 0x04) >> 0x02) | ((c3_H & 0x04) >> 0x01) | ((c5_H & 0x04) >> 0x00) | ((c7_H & 0x04) << 0x01);
	dst[0x06] = ((c1_H & 0x02) >> 0x01) | ((c3_H & 0x02) >> 0x00) | ((c5_H & 0x02) << 0x01) | ((c7_H & 0x02) << 0x02);
	dst[0x07] = ((c1_H & 0x01) >> 0x00) | ((c3_H & 0x01) << 0x01) | ((c5_H & 0x01) << 0x02) | ((c7_H & 0x01) << 0x03);
	dst[0x08] = ((c1_M & 0x80) >> 0x07) | ((c3_M & 0x80) >> 0x06) | ((c5_M & 0x80) >> 0x05) | ((c7_M & 0x80) >> 0x04);
	dst[0x09] = ((c1_M & 0x40) >> 0x06) | ((c3_M & 0x40) >> 0x05) | ((c5_M & 0x40) >> 0x04) | ((c7_M & 0x40) >> 0x03);
	dst[0x0A] = ((c1_M & 0x20) >> 0x05) | ((c3_M & 0x20) >> 0x04) | ((c5_M & 0x20) >> 0x03) | ((c7_M & 0x20) >> 0x02);
	dst[0x0B] = ((c1_M & 0x10) >> 0x04) | ((c3_M & 0x10) >> 0x03) | ((c5_M & 0x10) >> 0x02) | ((c7_M & 0x10) >> 0x01);
	dst[0x0C] = ((c1_M & 0x08) >> 0x03) | ((c3_M & 0x08) >> 0x02) | ((c5_M & 0x08) >> 0x01) | ((c7_M & 0x08) >> 0x00);
	dst[0x0D] = ((c1_M & 0x04) >> 0x02) | ((c3_M & 0x04) >> 0x01) | ((c5_M & 0x04) >> 0x00) | ((c7_M & 0x04) << 0x01);
	dst[0x0E] = ((c1_M & 0x02) >> 0x01) | ((c3_M & 0x02) >> 0x00) | ((c5_M & 0x02) << 0x01) | ((c7_M & 0x02) << 0x02);
	dst[0x0F] = ((c1_M & 0x01) >> 0x00) | ((c3_M & 0x01) << 0x01) | ((c5_M & 0x01) << 0x02) | ((c7_M & 0x01) << 0x03);
	dst[0x10] = ((c1_L & 0x80) >> 0x07) | ((c3_L & 0x80) >> 0x06) | ((c5_L & 0x80) >> 0x05) | ((c7_L & 0x80) >> 0x04);
	dst[0x11] = ((c1_L & 0x40) >> 0x06) | ((c3_L & 0x40) >> 0x05) | ((c5_L & 0x40) >> 0x04) | ((c7_L & 0x40) >> 0x03);
	dst[0x12] = ((c1_L & 0x20) >> 0x05) | ((c3_L & 0x20) >> 0x04) | ((c5_L & 0x20) >> 0x03) | ((c7_L & 0x20) >> 0x02);
	dst[0x13] = ((c1_L & 0x10) >> 0x04) | ((c3_L & 0x10) >> 0x03) | ((c5_L & 0x10) >> 0x02) | ((c7_L & 0x10) >> 0x01);
	dst[0x14] = ((c1_L & 0x08) >> 0x03) | ((c3_L & 0x08) >> 0x02) | ((c5_L & 0x08) >> 0x01) | ((c7_L & 0x08) >> 0x00);
	dst[0x15] = ((c1_L & 0x04) >> 0x02) | ((c3_L & 0x04) >> 0x01) | ((c5_L & 0x04) >> 0x00) | ((c7_L & 0x04) << 0x01);
	dst[0x16] = ((c1_L & 0x02) >> 0x01) | ((c3_L & 0x02) >> 0x00) | ((c5_L & 0x02) << 0x01) | ((c7_L & 0x02) << 0x02);
	dst[0x17] = ((c1_L & 0x01) >> 0x00) | ((c3_L & 0x01) << 0x01) | ((c5_L & 0x01) << 0x02) | ((c7_L & 0x01) << 0x03);

	dst[0x18] = ((c2_H & 0x80) >> 0x07) | ((c4_H & 0x80) >> 0x06) | ((c6_H & 0x80) >> 0x05) | ((c8_H & 0x80) >> 0x04);
	dst[0x19] = ((c2_H & 0x40) >> 0x06) | ((c4_H & 0x40) >> 0x05) | ((c6_H & 0x40) >> 0x04) | ((c8_H & 0x40) >> 0x03);
	dst[0x1A] = ((c2_H & 0x20) >> 0x05) | ((c4_H & 0x20) >> 0x04) | ((c6_H & 0x20) >> 0x03) | ((c8_H & 0x20) >> 0x02);
	dst[0x1B] = ((c2_H & 0x10) >> 0x04) | ((c4_H & 0x10) >> 0x03) | ((c6_H & 0x10) >> 0x02) | ((c8_H & 0x10) >> 0x01);
	dst[0x1C] = ((c2_H & 0x08) >> 0x03) | ((c4_H & 0x08) >> 0x02) | ((c6_H & 0x08) >> 0x01) | ((c8_H & 0x08) >> 0x00);
	dst[0x1D] = ((c2_H & 0x04) >> 0x02) | ((c4_H & 0x04) >> 0x01) | ((c6_H & 0x04) >> 0x00) | ((c8_H & 0x04) << 0x01);
	dst[0x1E] = ((c2_H & 0x02) >> 0x01) | ((c4_H & 0x02) >> 0x00) | ((c6_H & 0x02) << 0x01) | ((c8_H & 0x02) << 0x02);
	dst[0x1F] = ((c2_H & 0x01) >> 0x00) | ((c4_H & 0x01) << 0x01) | ((c6_H & 0x01) << 0x02) | ((c8_H & 0x01) << 0x03);
	dst[0x20] = ((c2_M & 0x80) >> 0x07) | ((c4_M & 0x80) >> 0x06) | ((c6_M & 0x80) >> 0x05) | ((c8_M & 0x80) >> 0x04);
	dst[0x21] = ((c2_M & 0x40) >> 0x06) | ((c4_M & 0x40) >> 0x05) | ((c6_M & 0x40) >> 0x04) | ((c8_M & 0x40) >> 0x03);
	dst[0x22] = ((c2_M & 0x20) >> 0x05) | ((c4_M & 0x20) >> 0x04) | ((c6_M & 0x20) >> 0x03) | ((c8_M & 0x20) >> 0x02);
	dst[0x23] = ((c2_M & 0x10) >> 0x04) | ((c4_M & 0x10) >> 0x03) | ((c6_M & 0x10) >> 0x02) | ((c8_M & 0x10) >> 0x01);
	dst[0x24] = ((c2_M & 0x08) >> 0x03) | ((c4_M & 0x08) >> 0x02) | ((c6_M & 0x08) >> 0x01) | ((c8_M & 0x08) >> 0x00);
	dst[0x25] = ((c2_M & 0x04) >> 0x02) | ((c4_M & 0x04) >> 0x01) | ((c6_M & 0x04) >> 0x00) | ((c8_M & 0x04) << 0x01);
	dst[0x26] = ((c2_M & 0x02) >> 0x01) | ((c4_M & 0x02) >> 0x00) | ((c6_M & 0x02) << 0x01) | ((c8_M & 0x02) << 0x02);
	dst[0x27] = ((c2_M & 0x01) >> 0x00) | ((c4_M & 0x01) << 0x01) | ((c6_M & 0x01) << 0x02) | ((c8_M & 0x01) << 0x03);
	dst[0x28] = ((c2_L & 0x80) >> 0x07) | ((c4_L & 0x80) >> 0x06) | ((c6_L & 0x80) >> 0x05) | ((c8_L & 0x80) >> 0x04);
	dst[0x29] = ((c2_L & 0x40) >> 0x06) | ((c4_L & 0x40) >> 0x05) | ((c6_L & 0x40) >> 0x04) | ((c8_L & 0x40) >> 0x03);
	dst[0x2A] = ((c2_L & 0x20) >> 0x05) | ((c4_L & 0x20) >> 0x04) | ((c6_L & 0x20) >> 0x03) | ((c8_L & 0x20) >> 0x02);
	dst[0x2B] = ((c2_L & 0x10) >> 0x04) | ((c4_L & 0x10) >> 0x03) | ((c6_L & 0x10) >> 0x02) | ((c8_L & 0x10) >> 0x01);
	dst[0x2C] = ((c2_L & 0x08) >> 0x03) | ((c4_L & 0x08) >> 0x02) | ((c6_L & 0x08) >> 0x01) | ((c8_L & 0x08) >> 0x00);
	dst[0x2D] = ((c2_L & 0x04) >> 0x02) | ((c4_L & 0x04) >> 0x01) | ((c6_L & 0x04) >> 0x00) | ((c8_L & 0x04) << 0x01);
	dst[0x2E] = ((c2_L & 0x02) >> 0x01) | ((c4_L & 0x02) >> 0x00) | ((c6_L & 0x02) << 0x01) | ((c8_L & 0x02) << 0x02);
	dst[0x2F] = ((c2_L & 0x01) >> 0x00) | ((c4_L & 0x01) << 0x01) | ((c6_L & 0x01) << 0x02) | ((c8_L & 0x01) << 0x03);
}

void PloytecAudio::DecodePloytecPCM(float *dst, const uint8_t *src) {
	uint8_t c[8][3];
	c[0][2] = ((src[0x00] & 0x01) << 7) | ((src[0x01] & 0x01) << 6) | ((src[0x02] & 0x01) << 5) | ((src[0x03] & 0x01) << 4) | ((src[0x04] & 0x01) << 3) | ((src[0x05] & 0x01) << 2) | ((src[0x06] & 0x01) << 1) | ((src[0x07] & 0x01));
	c[0][1] = ((src[0x08] & 0x01) << 7) | ((src[0x09] & 0x01) << 6) | ((src[0x0A] & 0x01) << 5) | ((src[0x0B] & 0x01) << 4) | ((src[0x0C] & 0x01) << 3) | ((src[0x0D] & 0x01) << 2) | ((src[0x0E] & 0x01) << 1) | ((src[0x0F] & 0x01));
	c[0][0] = ((src[0x10] & 0x01) << 7) | ((src[0x11] & 0x01) << 6) | ((src[0x12] & 0x01) << 5) | ((src[0x13] & 0x01) << 4) | ((src[0x14] & 0x01) << 3) | ((src[0x15] & 0x01) << 2) | ((src[0x16] & 0x01) << 1) | ((src[0x17] & 0x01));
	c[2][2] = ((src[0x00] & 0x02) << 6) | ((src[0x01] & 0x02) << 5) | ((src[0x02] & 0x02) << 4) | ((src[0x03] & 0x02) << 3) | ((src[0x04] & 0x02) << 2) | ((src[0x05] & 0x02) << 1) | ((src[0x06] & 0x02)) | ((src[0x07] & 0x02) >> 1);
	c[2][1] = ((src[0x08] & 0x02) << 6) | ((src[0x09] & 0x02) << 5) | ((src[0x0A] & 0x02) << 4) | ((src[0x0B] & 0x02) << 3) | ((src[0x0C] & 0x02) << 2) | ((src[0x0D] & 0x02) << 1) | ((src[0x0E] & 0x02)) | ((src[0x0F] & 0x02) >> 1);
	c[2][0] = ((src[0x10] & 0x02) << 6) | ((src[0x11] & 0x02) << 5) | ((src[0x12] & 0x02) << 4) | ((src[0x13] & 0x02) << 3) | ((src[0x14] & 0x02) << 2) | ((src[0x15] & 0x02) << 1) | ((src[0x16] & 0x02)) | ((src[0x17] & 0x02) >> 1);
	c[4][2] = ((src[0x00] & 0x04) << 5) | ((src[0x01] & 0x04) << 4) | ((src[0x02] & 0x04) << 3) | ((src[0x03] & 0x04) << 2) | ((src[0x04] & 0x04) << 1) | ((src[0x05] & 0x04)) | ((src[0x06] & 0x04) >> 1) | ((src[0x07] & 0x04) >> 2);
	c[4][1] = ((src[0x08] & 0x04) << 5) | ((src[0x09] & 0x04) << 4) | ((src[0x0A] & 0x04) << 3) | ((src[0x0B] & 0x04) << 2) | ((src[0x0C] & 0x04) << 1) | ((src[0x0D] & 0x04)) | ((src[0x0E] & 0x04) >> 1) | ((src[0x0F] & 0x04) >> 2);
	c[4][0] = ((src[0x10] & 0x04) << 5) | ((src[0x11] & 0x04) << 4) | ((src[0x12] & 0x04) << 3) | ((src[0x13] & 0x04) << 2) | ((src[0x14] & 0x04) << 1) | ((src[0x15] & 0x04)) | ((src[0x16] & 0x04) >> 1) | ((src[0x17] & 0x04) >> 2);
	c[6][2] = ((src[0x00] & 0x08) << 4) | ((src[0x01] & 0x08) << 3) | ((src[0x02] & 0x08) << 2) | ((src[0x03] & 0x08) << 1) | ((src[0x04] & 0x08)) | ((src[0x05] & 0x08) >> 1) | ((src[0x06] & 0x08) >> 2) | ((src[0x07] & 0x08) >> 3);
	c[6][1] = ((src[0x08] & 0x08) << 4) | ((src[0x09] & 0x08) << 3) | ((src[0x0A] & 0x08) << 2) | ((src[0x0B] & 0x08) << 1) | ((src[0x0C] & 0x08)) | ((src[0x0D] & 0x08) >> 1) | ((src[0x0E] & 0x08) >> 2) | ((src[0x0F] & 0x08) >> 3);
	c[6][0] = ((src[0x10] & 0x08) << 4) | ((src[0x11] & 0x08) << 3) | ((src[0x12] & 0x08) << 2) | ((src[0x13] & 0x08) << 1) | ((src[0x14] & 0x08)) | ((src[0x15] & 0x08) >> 1) | ((src[0x16] & 0x08) >> 2) | ((src[0x17] & 0x08) >> 3);
	c[1][2] = ((src[0x20] & 0x01) << 7) | ((src[0x21] & 0x01) << 6) | ((src[0x22] & 0x01) << 5) | ((src[0x23] & 0x01) << 4) | ((src[0x24] & 0x01) << 3) | ((src[0x25] & 0x01) << 2) | ((src[0x26] & 0x01) << 1) | ((src[0x27] & 0x01));
	c[1][1] = ((src[0x28] & 0x01) << 7) | ((src[0x29] & 0x01) << 6) | ((src[0x2A] & 0x01) << 5) | ((src[0x2B] & 0x01) << 4) | ((src[0x2C] & 0x01) << 3) | ((src[0x2D] & 0x01) << 2) | ((src[0x2E] & 0x01) << 1) | ((src[0x2F] & 0x01));
	c[1][0] = ((src[0x30] & 0x01) << 7) | ((src[0x31] & 0x01) << 6) | ((src[0x32] & 0x01) << 5) | ((src[0x33] & 0x01) << 4) | ((src[0x34] & 0x01) << 3) | ((src[0x35] & 0x01) << 2) | ((src[0x36] & 0x01) << 1) | ((src[0x37] & 0x01));
	c[3][2] = ((src[0x20] & 0x02) << 6) | ((src[0x21] & 0x02) << 5) | ((src[0x22] & 0x02) << 4) | ((src[0x23] & 0x02) << 3) | ((src[0x24] & 0x02) << 2) | ((src[0x25] & 0x02) << 1) | ((src[0x26] & 0x02)) | ((src[0x27] & 0x02) >> 1);
	c[3][1] = ((src[0x28] & 0x02) << 6) | ((src[0x29] & 0x02) << 5) | ((src[0x2A] & 0x02) << 4) | ((src[0x2B] & 0x02) << 3) | ((src[0x2C] & 0x02) << 2) | ((src[0x2D] & 0x02) << 1) | ((src[0x2E] & 0x02)) | ((src[0x2F] & 0x02) >> 1);
	c[3][0] = ((src[0x30] & 0x02) << 6) | ((src[0x31] & 0x02) << 5) | ((src[0x32] & 0x02) << 4) | ((src[0x33] & 0x02) << 3) | ((src[0x34] & 0x02) << 2) | ((src[0x35] & 0x02) << 1) | ((src[0x36] & 0x02)) | ((src[0x37] & 0x02) >> 1);
	c[5][2] = ((src[0x20] & 0x04) << 5) | ((src[0x21] & 0x04) << 4) | ((src[0x22] & 0x04) << 3) | ((src[0x23] & 0x04) << 2) | ((src[0x24] & 0x04) << 1) | ((src[0x25] & 0x04)) | ((src[0x26] & 0x04) >> 1) | ((src[0x27] & 0x04) >> 2);
	c[5][1] = ((src[0x28] & 0x04) << 5) | ((src[0x29] & 0x04) << 4) | ((src[0x2A] & 0x04) << 3) | ((src[0x2B] & 0x04) << 2) | ((src[0x2C] & 0x04) << 1) | ((src[0x2D] & 0x04)) | ((src[0x2E] & 0x04) >> 1) | ((src[0x2F] & 0x04) >> 2);
	c[5][0] = ((src[0x30] & 0x04) << 5) | ((src[0x31] & 0x04) << 4) | ((src[0x32] & 0x04) << 3) | ((src[0x33] & 0x04) << 2) | ((src[0x34] & 0x04) << 1) | ((src[0x35] & 0x04)) | ((src[0x36] & 0x04) >> 1) | ((src[0x37] & 0x04) >> 2);
	c[7][2] = ((src[0x20] & 0x08) << 4) | ((src[0x21] & 0x08) << 3) | ((src[0x22] & 0x08) << 2) | ((src[0x23] & 0x08) << 1) | ((src[0x24] & 0x08)) | ((src[0x25] & 0x08) >> 1) | ((src[0x26] & 0x08) >> 2) | ((src[0x27] & 0x08) >> 3);
	c[7][1] = ((src[0x28] & 0x08) << 4) | ((src[0x29] & 0x08) << 3) | ((src[0x2A] & 0x08) << 2) | ((src[0x2B] & 0x08) << 1) | ((src[0x2C] & 0x08)) | ((src[0x2D] & 0x08) >> 1) | ((src[0x2E] & 0x08) >> 2) | ((src[0x2F] & 0x08) >> 3);
	c[7][0] = ((src[0x30] & 0x08) << 4) | ((src[0x31] & 0x08) << 3) | ((src[0x32] & 0x08) << 2) | ((src[0x33] & 0x08) << 1) | ((src[0x34] & 0x08)) | ((src[0x35] & 0x08) >> 1) | ((src[0x36] & 0x08) >> 2) | ((src[0x37] & 0x08) >> 3);

	for(int i=0; i<8; i++) {
		int32_t s = (c[i][0]) | (c[i][1] << 8) | (c[i][2] << 16);
		if(s & 0x800000) s |= 0xFF000000;
		dst[i] = (float)s / 8388608.0f;
	}
}

CFStringRef PloytecAudio::GetDeviceName() const { return mModelUID; }
CFStringRef PloytecAudio::GetManufacturer() const { return mManufacturerUID; }
CFStringRef PloytecAudio::GetDeviceUID() const { return mDeviceUID; }
CFStringRef PloytecAudio::GetModelUID() const { return mModelUID; }
UInt32 PloytecAudio::GetTransportType() const { return kAudioDeviceTransportTypeUSB; }
UInt32 PloytecAudio::GetClockDomain() const { return 0x504C4F59; }
Float64 PloytecAudio::GetNominalSampleRate() const { return 96000.0; }
AudioValueRange PloytecAudio::GetAvailableSampleRates() const { return mAvailableSampleRates; }
AudioBufferList* PloytecAudio::GetInputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mInputConfig); }
AudioBufferList* PloytecAudio::GetOutputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mOutputConfig); }
AudioObjectID PloytecAudio::GetInputStreamID() const { return kPloytecInputStreamID; }
AudioObjectID PloytecAudio::GetOutputStreamID() const { return kPloytecOutputStreamID; }
AudioStreamBasicDescription PloytecAudio::GetStreamFormat() const { return mCurrentStreamFormat; }
AudioStreamRangedDescription PloytecAudio::GetStreamRangedDescription() const {
	AudioStreamRangedDescription outDesc; outDesc.mFormat = GetStreamFormat();
	outDesc.mSampleRateRange.mMinimum = 96000.0; outDesc.mSampleRateRange.mMaximum = 96000.0;
	return outDesc;
}
UInt32 PloytecAudio::GetZeroTimestampPeriod() const { return mZeroTimestampPeriod; }

AudioChannelLayout* PloytecAudio::GetPreferredChannelLayout(AudioObjectPropertyScope inScope) {
	AudioChannelLayout* layout = (AudioChannelLayout*)mChannelLayoutBuffer;
	layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
	layout->mChannelBitmap = 0;
	layout->mNumberChannelDescriptions = 8;
	for (UInt32 i = 0; i < 8; ++i) {
		layout->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Unknown;
		layout->mChannelDescriptions[i].mChannelFlags = kAudioChannelFlags_AllOff;
	}
	return layout;
}

UInt32 PloytecAudio::GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const {
	return offsetof(AudioChannelLayout, mChannelDescriptions) + (8 * sizeof(AudioChannelDescription));
}

extern "C" HRESULT PloytecInitialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost) {
	PloytecAudio::Get().Init(inHost);
	return kAudioHardwareNoError;
}

extern "C" HRESULT PloytecCreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo*, AudioObjectID*) { return kAudioHardwareNoError; }
extern "C" HRESULT PloytecDestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareNoError; }
extern "C" HRESULT PloytecAddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT PloytecRemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo*) { return kAudioHardwareNoError; }
extern "C" HRESULT PloytecPerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }
extern "C" HRESULT PloytecAbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void*) { return kAudioHardwareNoError; }

extern "C" Boolean PloytecHasProperty(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress) {
	if (inObjectID == kAudioObjectPlugInObject) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
			case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
			case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
			case kAudioObjectPropertyControlList: return true;
		}
		return false;
	}
	if (inObjectID == kPloytecDeviceID) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
			case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
			case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
			case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
			case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
			case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
			case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyStreams:
			case kAudioDevicePropertyStreamConfiguration: case kAudioDevicePropertyLatency:
			case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyPreferredChannelLayout:
			case kAudioDevicePropertyZeroTimeStampPeriod: return true;
		}
		return false;
	}
	if (inObjectID == kPloytecOutputStreamID || inObjectID == kPloytecInputStreamID) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
			case kAudioObjectPropertyName: case kAudioObjectPropertyOwner:
			case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType:
			case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyVirtualFormat:
			case kAudioStreamPropertyPhysicalFormat: case kAudioStreamPropertyAvailableVirtualFormats:
			case kAudioStreamPropertyAvailablePhysicalFormats: return true;
		}
		return false;
	}
	return false;
}

extern "C" HRESULT PloytecIsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, Boolean* outIsSettable) {
	*outIsSettable = false; return kAudioHardwareNoError;
}

extern "C" HRESULT PloytecGetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32* outDataSize) {
	if (!outDataSize) return kAudioHardwareIllegalOperationError;
	*outDataSize = 0;

	if (inObjectID == kAudioObjectPlugInObject) {
		switch (inAddress->mSelector) {
			case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
				*outDataSize = PloytecAudio::Get().IsConnected() ? sizeof(AudioObjectID) : 0; return kAudioHardwareNoError;
			case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
			case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
			case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
			default: return kAudioHardwareUnknownPropertyError;
		}
	}
	if (inObjectID == kPloytecDeviceID) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
			case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID:
				*outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
			case kAudioObjectPropertyControlList: *outDataSize = 0; return kAudioHardwareNoError;
			case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
			case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
			case kAudioDevicePropertyTransportType: case kAudioDevicePropertyIsHidden:
			case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceIsRunning:
			case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyNominalSampleRate:
				*outDataSize = sizeof(Float64); return kAudioHardwareNoError;
			case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
			case kAudioDevicePropertyStreams:
				if (inAddress->mScope == kAudioObjectPropertyScopeInput) *outDataSize = sizeof(AudioObjectID);
				else if (IsOutputScope(inAddress->mScope)) *outDataSize = sizeof(AudioObjectID);
				else if (inAddress->mScope == kAudioObjectPropertyScopeGlobal) *outDataSize = 2 * sizeof(AudioObjectID);
				else *outDataSize = 0;
				return kAudioHardwareNoError;
			case kAudioDevicePropertyStreamConfiguration:
				*outDataSize = GetAudioBufferListSize((IsOutputScope(inAddress->mScope) ? PloytecAudio::Get().GetOutputStreamConfiguration() : PloytecAudio::Get().GetInputStreamConfiguration())->mNumberBuffers);
				return kAudioHardwareNoError;
			case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyLatency: case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
			case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = PloytecAudio::Get().GetPreferredChannelLayoutSize(inAddress->mScope); return kAudioHardwareNoError;
			default: return kAudioHardwareUnknownPropertyError;
		}
	}
	if (inObjectID == kPloytecOutputStreamID || inObjectID == kPloytecInputStreamID) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *outDataSize = sizeof(AudioClassID); return kAudioHardwareNoError;
			case kAudioObjectPropertyName: *outDataSize = sizeof(CFStringRef); return kAudioHardwareNoError;
			case kAudioObjectPropertyOwner: *outDataSize = sizeof(AudioObjectID); return kAudioHardwareNoError;
			case kAudioStreamPropertyDirection: case kAudioStreamPropertyTerminalType: case kAudioStreamPropertyStartingChannel: case kAudioStreamPropertyLatency:
				*outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
			case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: *outDataSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
			case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: *outDataSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
			default: return kAudioHardwareUnknownPropertyError;
		}
	}
	return kAudioHardwareBadObjectError;
}

extern "C" HRESULT PloytecGetPropertyData(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inMax, UInt32* outSize, void* outData) {
	if (inObjectID == kAudioObjectPlugInObject) {
		switch (inAddress->mSelector) {
			case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
				if (!PloytecAudio::Get().IsConnected()) { if (outSize) *outSize = 0; return kAudioHardwareNoError; }
				return WriteObjectID(inMax, outSize, outData, kPloytecDeviceID);
			case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
			case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, CFSTR("Ploytec HAL"));
			case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, CFSTR("Ploytec"));
			case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
			case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioPlugInClassID);
			default: return kAudioHardwareUnknownPropertyError;
		}
	}
	if (inObjectID == kPloytecDeviceID) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
			case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioDeviceClassID);
			case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
			case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kAudioObjectPlugInObject);
			case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, PloytecAudio::Get().GetDeviceName());
			case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, PloytecAudio::Get().GetManufacturer());
			case kAudioDevicePropertyDeviceUID: return WriteCFString(inMax, outSize, outData, PloytecAudio::Get().GetDeviceUID());
			case kAudioDevicePropertyModelUID: return WriteCFString(inMax, outSize, outData, PloytecAudio::Get().GetModelUID());
			case kAudioDevicePropertyTransportType: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().GetTransportType());
			case kAudioDevicePropertyIsHidden: return WriteUInt32(inMax, outSize, outData, 0);
			case kAudioDevicePropertyDeviceIsAlive: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().IsConnected() ? 1 : 0);
			case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceIsRunningSomewhere: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().IsRunning() ? 1 : 0);
			case kAudioDevicePropertyNominalSampleRate: return WriteFloat64(inMax, outSize, outData, PloytecAudio::Get().GetNominalSampleRate());
			case kAudioDevicePropertyAvailableNominalSampleRates: if (outSize) *outSize = sizeof(AudioValueRange); if (outData) { if (inMax < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError; *(AudioValueRange*)outData = PloytecAudio::Get().GetAvailableSampleRates(); } return kAudioHardwareNoError;
			case kAudioDevicePropertyStreams: {
				UInt32 streamList[] = { kPloytecInputStreamID, kPloytecOutputStreamID };
				if (inAddress->mScope == kAudioObjectPropertyScopeInput) return WriteObjectID(inMax, outSize, outData, kPloytecInputStreamID);
				else if (inAddress->mScope == kAudioObjectPropertyScopeOutput) return WriteObjectID(inMax, outSize, outData, kPloytecOutputStreamID);
				else { if (outData && inMax < sizeof(streamList)) return kAudioHardwareBadPropertySizeError; if (outData) memcpy(outData, streamList, sizeof(streamList)); if (outSize) *outSize = sizeof(streamList); return kAudioHardwareNoError; }
			}
			case kAudioObjectPropertyOwnedObjects: {
				if (inMax < 2 * sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
				AudioObjectID* list = (AudioObjectID*)outData; if (list) { list[0] = kPloytecOutputStreamID; list[1] = kPloytecInputStreamID; }
				if (outSize) *outSize = 2 * sizeof(AudioObjectID); return kAudioHardwareNoError;
			}
			case kAudioDevicePropertyStreamConfiguration: {
				AudioBufferList* config = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? PloytecAudio::Get().GetInputStreamConfiguration() : PloytecAudio::Get().GetOutputStreamConfiguration();
				UInt32 actualSize = GetAudioBufferListSize(config->mNumberBuffers); if (outSize) *outSize = actualSize;
				if (outData) { if (inMax < actualSize) return kAudioHardwareBadPropertySizeError; memcpy(outData, config, actualSize); } return kAudioHardwareNoError;
			}
			case kAudioDevicePropertyClockDomain: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().GetClockDomain());
			case kAudioDevicePropertyLatency: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().GetLatency());
			case kAudioDevicePropertySafetyOffset: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().GetSafetyOffset());
			case kAudioDevicePropertyZeroTimeStampPeriod: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().GetZeroTimestampPeriod());
			case kAudioDevicePropertyPreferredChannelLayout: {
				UInt32 size = PloytecAudio::Get().GetPreferredChannelLayoutSize(inAddress->mScope);
				if (outSize) *outSize = size;
				if (outData) { if (inMax < size) return kAudioHardwareBadPropertySizeError; memcpy(outData, PloytecAudio::Get().GetPreferredChannelLayout(inAddress->mScope), size); }
				return kAudioHardwareNoError;
			}
			default: return kAudioHardwareUnknownPropertyError;
		}
	}
	if (inObjectID == kPloytecOutputStreamID || inObjectID == kPloytecInputStreamID) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
			case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioStreamClassID);
			case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, (inObjectID == kPloytecOutputStreamID ? CFSTR("Ploytec PCM Output Stream") : CFSTR("Ploytec PCM Input Stream")));
			case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kPloytecDeviceID);
			case kAudioStreamPropertyDirection: return WriteUInt32(inMax, outSize, outData, (inObjectID == kPloytecOutputStreamID ? 0 : 1));
			case kAudioStreamPropertyTerminalType: return WriteUInt32(inMax, outSize, outData, kAudioStreamTerminalTypeSpeaker);
			case kAudioStreamPropertyStartingChannel: return WriteUInt32(inMax, outSize, outData, 1);
			case kAudioStreamPropertyLatency: return WriteUInt32(inMax, outSize, outData, PloytecAudio::Get().GetLatency());
			case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
				if (outData && inMax < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
				if (outData) *(AudioStreamBasicDescription*)outData = PloytecAudio::Get().GetStreamFormat();
				if (outSize) *outSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
			case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
				if (outData && inMax < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
				if (outData) *(AudioStreamRangedDescription*)outData = PloytecAudio::Get().GetStreamRangedDescription();
				if (outSize) *outSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
			default: return kAudioHardwareUnknownPropertyError;
		}
	}
	return kAudioHardwareBadObjectError;
}

extern "C" HRESULT PloytecSetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress*, UInt32, const void*, UInt32, const void*) { return kAudioHardwareUnsupportedOperationError; }
extern "C" HRESULT PloytecStartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) { return PloytecAudio::Get().StartIO(); }
extern "C" HRESULT PloytecStopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32) { return PloytecAudio::Get().StopIO(); }
extern "C" HRESULT PloytecGetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) { return PloytecAudio::Get().GetZeroTimeStamp(inDriver, inDeviceObjectID, inClientID, outSampleTime, outHostTime, outSeed); }
extern "C" OSStatus PloytecWillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) { bool willDo = false; bool willDoInPlace = true; switch (inOperationID) { case kAudioServerPlugInIOOperationWriteMix: case kAudioServerPlugInIOOperationReadInput: willDo = true; break; default: willDo = false; break; } if (outWillDo) *outWillDo = willDo; if (outWillDoInPlace) *outWillDoInPlace = willDoInPlace; return kAudioHardwareNoError; }
extern "C" OSStatus PloytecBeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return 0; }
extern "C" OSStatus PloytecDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) { return PloytecAudio::Get().ioOperationHandler(inDriver, inDeviceObjectID, inStreamObjectID, inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo, ioMainBuffer, ioSecondaryBuffer); }
extern "C" OSStatus PloytecEndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*) { return 0; }
extern "C" HRESULT PloytecQueryInterface(void*, REFIID inREFIID, LPVOID* outInterface) { extern AudioServerPlugInDriverRef gPloytecDriverRef; CFUUIDBytes bytes = CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID); if (outInterface && memcmp(&inREFIID, &bytes, sizeof(REFIID)) == 0) { *outInterface = gPloytecDriverRef; return kAudioHardwareNoError; } return kAudioHardwareUnknownPropertyError; }
extern "C" ULONG PloytecAddRef(void*) { return 1; }
extern "C" ULONG PloytecRelease(void*) { return 1; }

static AudioServerPlugInDriverInterface gPloytecInterface = { NULL, PloytecQueryInterface, PloytecAddRef, PloytecRelease, PloytecInitialize, PloytecCreateDevice, PloytecDestroyDevice, PloytecAddDeviceClient, PloytecRemoveDeviceClient, PloytecPerformDeviceConfigurationChange, PloytecAbortDeviceConfigurationChange, PloytecHasProperty, PloytecIsPropertySettable, PloytecGetPropertyDataSize, PloytecGetPropertyData, PloytecSetPropertyData, PloytecStartIO, PloytecStopIO, PloytecGetZeroTimeStamp, PloytecWillDoIOOperation, PloytecBeginIOOperation, PloytecDoIOOperation, PloytecEndIOOperation };
static AudioServerPlugInDriverInterface* gPloytecInterfacePtr = &gPloytecInterface;
AudioServerPlugInDriverRef gPloytecDriverRef = &gPloytecInterfacePtr;

extern "C" __attribute__((visibility("default"))) void* PloytecPluginFactory(CFAllocatorRef, CFUUIDRef inRequestedTypeUUID) {
	if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return (void*)gPloytecDriverRef;
	return NULL;
}