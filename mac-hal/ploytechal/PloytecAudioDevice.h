#ifndef PloytecAudioDevice_h
#define PloytecAudioDevice_h

#include "PloytecDriver.h"
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <cstdint>
#include <mach/arm/kern_return.h>

struct PloytecAudioDevice_IVars {
	CFStringRef in_device_uid = nullptr;
	CFStringRef in_model_uid = nullptr;
	CFStringRef in_manufacturer_uid = nullptr;
	uint32_t bufferFrameSize = 32768;
	uint32_t zeroTimestampPeriod = 2560;
	uint8_t* PloytecInputBufferAddr = nullptr;
	uint8_t* PloytecOutputBufferAddr = nullptr;
	AudioStreamBasicDescription currentStreamFormat;
	uint32_t CoreAudioPlaybackBytesPerFrame = 0;
	uint32_t CoreAudioCaptureBytesPerFrame = 0;
	uint32_t inChannelCount = 0;
	uint32_t outChannelCount = 0;
	AudioBufferList inputConfig;
	AudioBufferList outputConfig;
	AudioValueRange availableSampleRates;
	bool IOStarted;
	bool PCMinActive;
	bool PCMoutActive;
	uint64_t HWSampleTimeIn;
	uint64_t HWSampleTimeOut;
	uint64_t lastZeroReported;
	uint32_t mTimestampSeed = 1;
};

typedef OSStatus (PloytecAudioDevice::*IOHandler)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);

class PloytecAudioDevice {
public:
	static PloytecAudioDevice& Get();
	bool init(PloytecDriver* in_driver, bool in_supports_prewarming, CFStringRef in_device_uid, CFStringRef in_model_uid, CFStringRef in_manufacturer_uid, uint32_t in_zero_timestamp_period, uint32_t inChannelCount, uint32_t outChannelCount, uint8_t* receiveBuffer, uint32_t receiveBufferSize, uint8_t* transmitBuffer, uint32_t transmitBufferSize, TransferMode transferMode);
	void free();

	kern_return_t StartIO();
	kern_return_t StopIO();

	bool Playback(uint16_t& currentpos, uint16_t frameCount, uint64_t completionTimestamp);
	bool Capture(uint16_t& currentpos, uint16_t frameCount, uint64_t completionTimestamp);
	void UpdateCurrentZeroTimestamp(uint64_t in_sample_time, uint64_t in_host_time);

	OSStatus ioOperationHandler(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
		return (this->*mActiveIOHandler)(inDriver, inDeviceObjectID, inStreamObjectID, inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo, ioMainBuffer, ioSecondaryBuffer);
	}

	HRESULT GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
	Float64 GetNominalSampleRate() const;
	AudioValueRange GetAvailableSampleRates() const;
	AudioBufferList* GetInputStreamConfiguration() const;
	AudioBufferList* GetOutputStreamConfiguration() const;
	CFStringRef GetDeviceName() const;
	CFStringRef GetManufacturer() const;
	CFStringRef GetDeviceUID() const;
	CFStringRef GetModelUID() const;
	UInt32 GetTransportType() const;
	UInt32 GetClockDomain() const;
	AudioObjectID GetInputStreamID() const;
	AudioObjectID GetOutputStreamID() const;
	AudioStreamBasicDescription GetStreamFormat() const;
	AudioStreamRangedDescription GetStreamRangedDescription() const;
	UInt32 GetLatency() const { return 128; }
	UInt32 GetSafetyOffset() const { return 512; }
	UInt32 GetZeroTimestampPeriod() const;
	UInt32 GetBufferFrameSize() const { return mIvars.bufferFrameSize; }
	AudioValueRange GetBufferFrameSizeRange() const;
	uint64_t GetTotalHardwareFrames() const { return mIvars.HWSampleTimeOut; }
	AudioChannelLayout* GetPreferredChannelLayout(AudioObjectPropertyScope inScope);
	UInt32 GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const;

private:
	PloytecAudioDevice() = default;
	PloytecAudioDevice_IVars mIvars;
	
	alignas(16) uint8_t mChannelLayoutBuffer[0x50C];
	
	std::atomic<uint32_t> mTimestampSeq{0};
	std::atomic<uint64_t> mTimestampSample{0};
	std::atomic<uint64_t> mTimestampHost{0};
	uint64_t mTimebaseNumer = 1;
	uint64_t mTimebaseDenom = 1;

	void RebuildStreamConfigsForBufferSize();
	inline void StoreTimestamp(uint64_t sample, uint64_t host) {
		mTimestampSeq.fetch_add(1, std::memory_order_relaxed);
		mTimestampHost.store(host, std::memory_order_relaxed);
		mTimestampSample.store(sample, std::memory_order_release);
		mTimestampSeq.fetch_add(1, std::memory_order_release);
	}
	inline void LoadTimestamp(uint64_t& sample, uint64_t& host) {
		sample = mTimestampSample.load(std::memory_order_relaxed);
		host = mTimestampHost.load(std::memory_order_relaxed);
	}

	OSStatus ioOperationBulk(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
	OSStatus ioOperationInterrupt(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);

	IOHandler mActiveIOHandler = nullptr;
	uint32_t mOutputBufferFrameCapacity;
	uint32_t mInputBufferFrameCapacity;
	uint32_t mOutputBufferSize; 
	uint32_t mInputBufferSize;

	void EncodePloytecPCM(uint8_t *dst, const float *src);
	void DecodePloytecPCM(float *dst, const uint8_t *src);
};

#endif