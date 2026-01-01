#include "PloytecAudioDevice.h"
#include "PloytecDriver.h"
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <CoreAudio/CoreAudio.h>
#include <IOKit/IOReturn.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <cstring>

static constexpr uint8_t COREAUDIO_BYTES_PER_SAMPLE = 4; // FLOAT32

PloytecAudioDevice& PloytecAudioDevice::Get() {
	static PloytecAudioDevice instance;
	return instance;
}

bool PloytecAudioDevice::init(PloytecDriver* in_driver, bool in_supports_prewarming, CFStringRef in_device_uid, CFStringRef in_model_uid, CFStringRef in_manufacturer_uid, uint32_t in_zero_timestamp_period, uint32_t inChannelCount, uint32_t outChannelCount, uint8_t* receiveBuffer, uint32_t receiveBufferSize, uint8_t* transmitBuffer, uint32_t transmitBufferSize, TransferMode transferMode) {
	os_log(GetLog(), "[PloytecHAL] >>> AUDIODRIVER INIT <<<");
	if (mIvars.in_device_uid) { CFRelease(mIvars.in_device_uid); mIvars.in_device_uid = nullptr; }
	if (mIvars.in_model_uid) { CFRelease(mIvars.in_model_uid); mIvars.in_model_uid = nullptr; }
	if (mIvars.in_manufacturer_uid) { CFRelease(mIvars.in_manufacturer_uid); mIvars.in_manufacturer_uid = nullptr; }

	mIvars.in_device_uid = in_device_uid ? (CFStringRef)CFRetain(in_device_uid) : CFSTR("unknown_uid");
	mIvars.in_model_uid = in_model_uid ? (CFStringRef)CFRetain(in_model_uid) : CFSTR("Ploytec Device");
	mIvars.in_manufacturer_uid = in_manufacturer_uid ? (CFStringRef)CFRetain(in_manufacturer_uid) : CFSTR("Ploytec");

	memset(&mIvars.inputConfig, 0, sizeof(AudioBufferList));
	memset(&mIvars.outputConfig, 0, sizeof(AudioBufferList));

	mActiveIOHandler = (transferMode == BULK) ? &PloytecAudioDevice::ioOperationBulk : &PloytecAudioDevice::ioOperationInterrupt;
	mOutputBufferSize = transmitBufferSize; mInputBufferSize = receiveBufferSize;
	mIvars.zeroTimestampPeriod = in_zero_timestamp_period;

	uint32_t inputPacketBytes = 80 * 64;
	uint32_t inputSegments = mInputBufferSize / inputPacketBytes;
	mInputBufferFrameCapacity = inputSegments * 80;

	uint32_t outputBlockStride = (transferMode == BULK) ? 512 : 482;
	uint32_t outputPacketBytes = 8 * outputBlockStride;
	uint32_t outputSegments = mOutputBufferSize / outputPacketBytes;
	mOutputBufferFrameCapacity = outputSegments * 80;

	mIvars.PloytecInputBufferAddr = receiveBuffer;
	mIvars.PloytecOutputBufferAddr = transmitBuffer;
	mIvars.CoreAudioPlaybackBytesPerFrame = outChannelCount * COREAUDIO_BYTES_PER_SAMPLE;
	mIvars.CoreAudioCaptureBytesPerFrame = inChannelCount * COREAUDIO_BYTES_PER_SAMPLE;
	mIvars.inChannelCount = inChannelCount;
	mIvars.outChannelCount = outChannelCount;
	mIvars.availableSampleRates.mMinimum = 96000.0; mIvars.availableSampleRates.mMaximum = 96000.0;
	
	mIvars.currentStreamFormat.mSampleRate = 96000.0;
	mIvars.currentStreamFormat.mFormatID = kAudioFormatLinearPCM;
	mIvars.currentStreamFormat.mFormatFlags = kAudioFormatFlagIsFloat;
	mIvars.currentStreamFormat.mBytesPerPacket = 32;
	mIvars.currentStreamFormat.mFramesPerPacket = 1;
	mIvars.currentStreamFormat.mBytesPerFrame = 32;
	mIvars.currentStreamFormat.mChannelsPerFrame = 8;
	mIvars.currentStreamFormat.mBitsPerChannel = 32;
	mIvars.bufferFrameSize = 2560;
	
	RebuildStreamConfigsForBufferSize();
	mach_timebase_info_data_t timebase; mach_timebase_info(&timebase);
	mTimebaseNumer = timebase.numer; mTimebaseDenom = timebase.denom;
	mIvars.HWSampleTimeOut = 0; mIvars.HWSampleTimeIn = 0;
	os_log(GetLog(), "[PloytecHAL] Audio Engine Initialized (Period=%u)", in_zero_timestamp_period);
	return true;
}

void PloytecAudioDevice::free() {}

kern_return_t PloytecAudioDevice::StartIO() {
	os_log(GetLog(), "[PloytecHAL] >>> StartIO Called <<<");
	mIvars.lastZeroReported = 0; mIvars.IOStarted = true;
	mIvars.PCMinActive = false; mIvars.PCMoutActive = false;
	
	UpdateCurrentZeroTimestamp(mIvars.HWSampleTimeOut, mach_absolute_time());
	mIvars.mTimestampSeed++;
	return kIOReturnSuccess;
}

kern_return_t PloytecAudioDevice::StopIO() {
	os_log(GetLog(), "[PloytecHAL] >>> StopIO Called <<<");
	mIvars.IOStarted = false; mIvars.mTimestampSeed++;
	PloytecDriver::GetInstance().ClearOutputBuffer();
	return kIOReturnSuccess;
}

bool PloytecAudioDevice::Playback(uint16_t &currentpos, uint16_t frameCount, uint64_t completionTimestamp) {
	uint32_t period = GetZeroTimestampPeriod();
	uint64_t currentSampleTime = mIvars.HWSampleTimeOut;
	currentpos = (currentSampleTime % period) / frameCount;
	mIvars.HWSampleTimeOut += frameCount;
	uint64_t nextSampleTime = mIvars.HWSampleTimeOut;
	if ((currentSampleTime / period) != (nextSampleTime / period)) {
		uint64_t newAnchorSample = (nextSampleTime / period) * period;
		UpdateCurrentZeroTimestamp(newAnchorSample, completionTimestamp);
	}
	return true;
}

bool PloytecAudioDevice::Capture(uint16_t& currentpos, uint16_t frameCount, uint64_t completionTimestamp) {
	currentpos = (mIvars.HWSampleTimeIn % GetZeroTimestampPeriod()) / frameCount;
	mIvars.HWSampleTimeIn += frameCount;
	return true;
}

void PloytecAudioDevice::UpdateCurrentZeroTimestamp(uint64_t in_sample_time, uint64_t in_host_time) { StoreTimestamp(in_sample_time, in_host_time); }

HRESULT PloytecAudioDevice::GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
	uint64_t sample, host; LoadTimestamp(sample, host);
	if (outSampleTime) *outSampleTime = (Float64)sample;
	if (outHostTime) *outHostTime = host;
	if (outSeed) *outSeed = mIvars.mTimestampSeed;
	return kAudioHardwareNoError;
}

uint32_t PloytecAudioDevice::GetZeroTimestampPeriod() const { return mIvars.zeroTimestampPeriod; }

OSStatus PloytecAudioDevice::ioOperationBulk(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
	if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
		mIvars.PCMoutActive = true; const float* srcPtr = (float*)ioMainBuffer; uint8_t* baseDst = mIvars.PloytecOutputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;
		uint32_t ringWriteIndex = (uint32_t)(sampleTime % mOutputBufferFrameCapacity);
		uint32_t framesRemaining = inIOBufferFrameSize;
		
		while (framesRemaining > 0) {
			uint32_t framesUntilWrap = mOutputBufferFrameCapacity - ringWriteIndex;
			uint32_t framesToProcess = (framesRemaining < framesUntilWrap) ? framesRemaining : framesUntilWrap;
			while (framesToProcess > 0) {
				uint32_t blockIndex = ringWriteIndex / 10;
				uint32_t frameInBlock = ringWriteIndex % 10;
				uint32_t byteOffset = (blockIndex * 512) + (frameInBlock * 48);
				uint8_t* dstPtr = baseDst + byteOffset;
				uint32_t framesLeftInBlock = 10 - frameInBlock;
				uint32_t batch = (framesToProcess < framesLeftInBlock) ? framesToProcess : framesLeftInBlock;
				for (uint32_t k = 0; k < batch; k++) { EncodePloytecPCM(dstPtr, srcPtr); dstPtr += 48; srcPtr += 8; }
				framesToProcess -= batch; framesRemaining -= batch; ringWriteIndex += batch;
			}
			if (ringWriteIndex >= mOutputBufferFrameCapacity) ringWriteIndex = 0;
		}
	} else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
		float* dstPtr = (float*)ioMainBuffer; uint8_t* baseSrc = mIvars.PloytecInputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;
		uint32_t ringReadIndex = (uint32_t)(sampleTime % mInputBufferFrameCapacity);
		uint32_t framesRemaining = inIOBufferFrameSize;
		while (framesRemaining > 0) {
			uint32_t framesUntilWrap = mInputBufferFrameCapacity - ringReadIndex;
			uint32_t framesToProcess = (framesRemaining < framesUntilWrap) ? framesRemaining : framesUntilWrap;
			uint8_t* srcPtr = baseSrc + (ringReadIndex * 64);
			for (uint32_t k = 0; k < framesToProcess; k++) { DecodePloytecPCM(dstPtr, srcPtr); srcPtr += 64; dstPtr += 8; }
			framesRemaining -= framesToProcess; ringReadIndex += framesToProcess;
			if (ringReadIndex >= mInputBufferFrameCapacity) ringReadIndex = 0;
		}
	}
	return kAudioHardwareNoError;
}

OSStatus PloytecAudioDevice::ioOperationInterrupt(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
	if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
		mIvars.PCMoutActive = true; const float* srcPtr = (float*)ioMainBuffer; uint8_t* baseDst = mIvars.PloytecOutputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mOutputTime.mSampleTime;
		uint32_t ringWriteIndex = (uint32_t)(sampleTime % mIvars.zeroTimestampPeriod);
		uint32_t framesRemaining = inIOBufferFrameSize; uint32_t period = mIvars.zeroTimestampPeriod;
		while (framesRemaining > 0) {
			uint32_t framesUntilWrap = period - ringWriteIndex;
			uint32_t framesToProcess = (framesRemaining < framesUntilWrap) ? framesRemaining : framesUntilWrap;
			while (framesToProcess > 0) {
				uint32_t blockIndex = ringWriteIndex / 10;
				uint32_t frameInBlock = ringWriteIndex % 10;
				uint32_t blockBase = blockIndex * 482;
				uint32_t framesLeftInBlock = 10 - frameInBlock;
				uint32_t batch = (framesToProcess < framesLeftInBlock) ? framesToProcess : framesLeftInBlock;
				for (uint32_t k = 0; k < batch; k++) {
					uint32_t padding = (frameInBlock >= 9) ? 2 : 0;
					uint32_t byteOffset = blockBase + (frameInBlock * 48) + padding;
					EncodePloytecPCM(baseDst + byteOffset, srcPtr);
					srcPtr += 8; frameInBlock++;
				}
				framesToProcess -= batch; framesRemaining -= batch; ringWriteIndex += batch;
			}
			if (ringWriteIndex >= period) ringWriteIndex = 0;
		}
	} else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
		float* dstPtr = (float*)ioMainBuffer; uint8_t* baseSrc = mIvars.PloytecInputBufferAddr;
		uint64_t sampleTime = (uint64_t)inIOCycleInfo->mInputTime.mSampleTime;
		uint32_t ringReadIndex = (uint32_t)(sampleTime % mIvars.zeroTimestampPeriod);
		uint32_t framesRemaining = inIOBufferFrameSize; uint32_t period = mIvars.zeroTimestampPeriod;
		while (framesRemaining > 0) {
			uint32_t framesUntilWrap = period - ringReadIndex;
			uint32_t framesToProcess = (framesRemaining < framesUntilWrap) ? framesRemaining : framesUntilWrap;
			uint8_t* srcPtr = baseSrc + (ringReadIndex * 64);
			for (uint32_t k = 0; k < framesToProcess; k++) { DecodePloytecPCM(dstPtr, srcPtr); srcPtr += 64; dstPtr += 8; }
			framesRemaining -= framesToProcess; ringReadIndex += framesToProcess;
			if (ringReadIndex >= period) ringReadIndex = 0;
		}
	}
	return kAudioHardwareNoError;
}

CFStringRef PloytecAudioDevice::GetDeviceName() const { return mIvars.in_device_uid; }
CFStringRef PloytecAudioDevice::GetManufacturer() const { return mIvars.in_manufacturer_uid; }
CFStringRef PloytecAudioDevice::GetDeviceUID() const { return mIvars.in_device_uid; }
CFStringRef PloytecAudioDevice::GetModelUID() const { return CFSTR("PloytecAudioDeviceModelUID"); }
UInt32 PloytecAudioDevice::GetTransportType() const { return kAudioDeviceTransportTypeUSB; }
UInt32 PloytecAudioDevice::GetClockDomain() const { return 0x504C4F59; }
Float64 PloytecAudioDevice::GetNominalSampleRate() const { return 96000.0; }
AudioValueRange PloytecAudioDevice::GetAvailableSampleRates() const { return mIvars.availableSampleRates; }
AudioBufferList* PloytecAudioDevice::GetInputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mIvars.inputConfig); }
AudioBufferList* PloytecAudioDevice::GetOutputStreamConfiguration() const { return const_cast<AudioBufferList*>(&mIvars.outputConfig); }
AudioObjectID PloytecAudioDevice::GetInputStreamID() const { return (mIvars.inChannelCount > 0) ? kPloytecInputStreamID : kAudioObjectUnknown; }
AudioObjectID PloytecAudioDevice::GetOutputStreamID() const { return (mIvars.outChannelCount > 0) ? kPloytecOutputStreamID : kAudioObjectUnknown; }
AudioStreamBasicDescription PloytecAudioDevice::GetStreamFormat() const { return mIvars.currentStreamFormat; }
AudioStreamRangedDescription PloytecAudioDevice::GetStreamRangedDescription() const {
	AudioStreamRangedDescription outDesc; outDesc.mFormat = GetStreamFormat();
	outDesc.mSampleRateRange.mMinimum = 96000.0; outDesc.mSampleRateRange.mMaximum = 96000.0;
	return outDesc;
}
AudioValueRange PloytecAudioDevice::GetBufferFrameSizeRange() const { AudioValueRange range; range.mMinimum = 2560; range.mMaximum = 2560; return range; }

AudioChannelLayout* PloytecAudioDevice::GetPreferredChannelLayout(AudioObjectPropertyScope inScope) {
	const UInt32 numChannels = (inScope == kAudioObjectPropertyScopeInput) ? mIvars.inChannelCount : mIvars.outChannelCount;
	AudioChannelLayout* layout = (AudioChannelLayout*)mChannelLayoutBuffer;
	layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
	layout->mChannelBitmap = 0x00;
	layout->mNumberChannelDescriptions = numChannels;
	for (UInt32 i = 0x00; i < numChannels; ++i) {
		layout->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Unknown;
		layout->mChannelDescriptions[i].mChannelFlags = kAudioChannelFlags_AllOff;
		layout->mChannelDescriptions[i].mCoordinates[0x00] = 0.0f;
		layout->mChannelDescriptions[i].mCoordinates[0x01] = 0.0f;
		layout->mChannelDescriptions[i].mCoordinates[0x02] = 0.0f;
	}
	return layout;
}

UInt32 PloytecAudioDevice::GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const {
	const UInt32 numChannels = (inScope == kAudioObjectPropertyScopeInput) ? mIvars.inChannelCount : mIvars.outChannelCount;
	return offsetof(AudioChannelLayout, mChannelDescriptions) + (numChannels * sizeof(AudioChannelDescription));
}

void PloytecAudioDevice::RebuildStreamConfigsForBufferSize() {
	const uint32_t frames = mIvars.bufferFrameSize;
	if (mIvars.inChannelCount > 0) {
		mIvars.inputConfig.mNumberBuffers = 1;
		mIvars.inputConfig.mBuffers[0].mNumberChannels = mIvars.inChannelCount;
		mIvars.inputConfig.mBuffers[0].mDataByteSize = frames * mIvars.inChannelCount * 4;
		mIvars.inputConfig.mBuffers[0].mData = nullptr;
	}
	if (mIvars.outChannelCount > 0) {
		mIvars.outputConfig.mNumberBuffers = 1;
		mIvars.outputConfig.mBuffers[0].mNumberChannels = mIvars.outChannelCount;
		mIvars.outputConfig.mBuffers[0].mDataByteSize = frames * mIvars.outChannelCount * 4;
		mIvars.outputConfig.mBuffers[0].mData = nullptr;
	}
}

void PloytecAudioDevice::EncodePloytecPCM(uint8_t *dst, const float *src)
{
	int32_t s[8];
	for(int i=0; i<8; i++) {
		s[i] = (int32_t)(src[i] * 8388608.0f);
		if(src[i] > 1.0f) s[i] = 0x7FFFFF; else if(src[i] < -1.0f) s[i] = -0x800000;
	}

	uint8_t c[8][3];
	for(int i=0; i<8; i++) {
		c[i][0] = s[i] & 0xFF; c[i][1] = (s[i] >> 8) & 0xFF; c[i][2] = (s[i] >> 16) & 0xFF;
	}

	for(int i=0; i<8; i++) {
		dst[i] = ((c[0][2] & (0x80 >> i)) >> (7-i)) | ((c[2][2] & (0x80 >> i)) >> (6-i)) | 
				 ((c[4][2] & (0x80 >> i)) >> (5-i)) | ((c[6][2] & (0x80 >> i)) >> (4-i));
	}

	const uint8_t c1_L = s[0] & 0xFF; const uint8_t c1_M = (s[0] >> 0x08) & 0xFF; const uint8_t c1_H = (s[0] >> 0x10) & 0xFF;
	const uint8_t c3_L = s[2] & 0xFF; const uint8_t c3_M = (s[2] >> 0x08) & 0xFF; const uint8_t c3_H = (s[2] >> 0x10) & 0xFF;
	const uint8_t c5_L = s[4] & 0xFF; const uint8_t c5_M = (s[4] >> 0x08) & 0xFF; const uint8_t c5_H = (s[4] >> 0x10) & 0xFF;
	const uint8_t c7_L = s[6] & 0xFF; const uint8_t c7_M = (s[6] >> 0x08) & 0xFF; const uint8_t c7_H = (s[6] >> 0x10) & 0xFF;

	const uint8_t c2_L = s[1] & 0xFF; const uint8_t c2_M = (s[1] >> 0x08) & 0xFF; const uint8_t c2_H = (s[1] >> 0x10) & 0xFF;
	const uint8_t c4_L = s[3] & 0xFF; const uint8_t c4_M = (s[3] >> 0x08) & 0xFF; const uint8_t c4_H = (s[3] >> 0x10) & 0xFF;
	const uint8_t c6_L = s[5] & 0xFF; const uint8_t c6_M = (s[5] >> 0x08) & 0xFF; const uint8_t c6_H = (s[5] >> 0x10) & 0xFF;
	const uint8_t c8_L = s[7] & 0xFF; const uint8_t c8_M = (s[7] >> 0x08) & 0xFF; const uint8_t c8_H = (s[7] >> 0x10) & 0xFF;

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

void PloytecAudioDevice::DecodePloytecPCM(float *dst, const uint8_t *src)
{
	uint8_t c[8][3];

	// Channel 1, 3, 5, 7
	c[0][2] = ((src[0x00] & 0x01) << 7) | ((src[0x01] & 0x01) << 6) | ((src[0x02] & 0x01) << 5) | ((src[0x03] & 0x01) << 4) | ((src[0x04] & 0x01) << 3) | ((src[0x05] & 0x01) << 2) | ((src[0x06] & 0x01) << 1) | ((src[0x07] & 0x01));
	c[0][1] = ((src[0x08] & 0x01) << 7) | ((src[0x09] & 0x01) << 6) | ((src[0x0A] & 0x01) << 5) | ((src[0x0B] & 0x01) << 4) | ((src[0x0C] & 0x01) << 3) | ((src[0x0D] & 0x01) << 2) | ((src[0x0E] & 0x01) << 1) | ((src[0x0F] & 0x01));
	c[0][0] = ((src[0x10] & 0x01) << 7) | ((src[0x11] & 0x01) << 6) | ((src[0x12] & 0x01) << 5) | ((src[0x13] & 0x01) << 4) | ((src[0x14] & 0x01) << 3) | ((src[0x15] & 0x01) << 2) | ((src[0x16] & 0x01) << 1) | ((src[0x17] & 0x01));

	c[2][2] = ((src[0x00] & 0x02) << 6) | ((src[0x01] & 0x02) << 5) | ((src[0x02] & 0x02) << 4) | ((src[0x03] & 0x02) << 3) | ((src[0x04] & 0x02) << 2) | ((src[0x05] & 0x02) << 1) | ((src[0x06] & 0x02)) | ((src[0x07] & 0x02) >> 1);
	c[2][1] = ((src[0x08] & 0x02) << 6) | ((src[0x09] & 0x02) << 5) | ((src[0x0A] & 0x02) << 4) | ((src[0x0B] & 0x02) << 3) | ((src[0x0C] & 0x02) << 2) | ((src[0x0D] & 0x02) << 1) | ((src[0x0E] & 0x02)) | ((src[0x0F] & 0x02) >> 1);
	c[2][0] = ((src[0x10] & 0x02) << 6) | ((src[0x11] & 0x02) << 5) | ((src[0x12] & 0x02) << 4) | ((src[0x13] & 0x02) << 3) | ((src[0x14] & 0x02) << 2) | ((src[0x15] & 0x02) << 1) | ((src[0x16] & 0x02)) | ((src[0x17] & 0x02) >> 1);

	// Channel 5
	c[4][2] = ((src[0x00] & 0x04) << 5) | ((src[0x01] & 0x04) << 4) | ((src[0x02] & 0x04) << 3) | ((src[0x03] & 0x04) << 2) | ((src[0x04] & 0x04) << 1) | ((src[0x05] & 0x04)) | ((src[0x06] & 0x04) >> 1) | ((src[0x07] & 0x04) >> 2);
	c[4][1] = ((src[0x08] & 0x04) << 5) | ((src[0x09] & 0x04) << 4) | ((src[0x0A] & 0x04) << 3) | ((src[0x0B] & 0x04) << 2) | ((src[0x0C] & 0x04) << 1) | ((src[0x0D] & 0x04)) | ((src[0x0E] & 0x04) >> 1) | ((src[0x0F] & 0x04) >> 2);
	c[4][0] = ((src[0x10] & 0x04) << 5) | ((src[0x11] & 0x04) << 4) | ((src[0x12] & 0x04) << 3) | ((src[0x13] & 0x04) << 2) | ((src[0x14] & 0x04) << 1) | ((src[0x15] & 0x04)) | ((src[0x16] & 0x04) >> 1) | ((src[0x17] & 0x04) >> 2);

	// Channel 7
	c[6][2] = ((src[0x00] & 0x08) << 4) | ((src[0x01] & 0x08) << 3) | ((src[0x02] & 0x08) << 2) | ((src[0x03] & 0x08) << 1) | ((src[0x04] & 0x08)) | ((src[0x05] & 0x08) >> 1) | ((src[0x06] & 0x08) >> 2) | ((src[0x07] & 0x08) >> 3);
	c[6][1] = ((src[0x08] & 0x08) << 4) | ((src[0x09] & 0x08) << 3) | ((src[0x0A] & 0x08) << 2) | ((src[0x0B] & 0x08) << 1) | ((src[0x0C] & 0x08)) | ((src[0x0D] & 0x08) >> 1) | ((src[0x0E] & 0x08) >> 2) | ((src[0x0F] & 0x08) >> 3);
	c[6][0] = ((src[0x10] & 0x08) << 4) | ((src[0x11] & 0x08) << 3) | ((src[0x12] & 0x08) << 2) | ((src[0x13] & 0x08) << 1) | ((src[0x14] & 0x08)) | ((src[0x15] & 0x08) >> 1) | ((src[0x16] & 0x08) >> 2) | ((src[0x17] & 0x08) >> 3);

	// Channel 2
	c[1][2] = ((src[0x20] & 0x01) << 7) | ((src[0x21] & 0x01) << 6) | ((src[0x22] & 0x01) << 5) | ((src[0x23] & 0x01) << 4) | ((src[0x24] & 0x01) << 3) | ((src[0x25] & 0x01) << 2) | ((src[0x26] & 0x01) << 1) | ((src[0x27] & 0x01));
	c[1][1] = ((src[0x28] & 0x01) << 7) | ((src[0x29] & 0x01) << 6) | ((src[0x2A] & 0x01) << 5) | ((src[0x2B] & 0x01) << 4) | ((src[0x2C] & 0x01) << 3) | ((src[0x2D] & 0x01) << 2) | ((src[0x2E] & 0x01) << 1) | ((src[0x2F] & 0x01));
	c[1][0] = ((src[0x30] & 0x01) << 7) | ((src[0x31] & 0x01) << 6) | ((src[0x32] & 0x01) << 5) | ((src[0x33] & 0x01) << 4) | ((src[0x34] & 0x01) << 3) | ((src[0x35] & 0x01) << 2) | ((src[0x36] & 0x01) << 1) | ((src[0x37] & 0x01));

	// Channel 4
	c[3][2] = ((src[0x20] & 0x02) << 6) | ((src[0x21] & 0x02) << 5) | ((src[0x22] & 0x02) << 4) | ((src[0x23] & 0x02) << 3) | ((src[0x24] & 0x02) << 2) | ((src[0x25] & 0x02) << 1) | ((src[0x26] & 0x02)) | ((src[0x27] & 0x02) >> 1);
	c[3][1] = ((src[0x28] & 0x02) << 6) | ((src[0x29] & 0x02) << 5) | ((src[0x2A] & 0x02) << 4) | ((src[0x2B] & 0x02) << 3) | ((src[0x2C] & 0x02) << 2) | ((src[0x2D] & 0x02) << 1) | ((src[0x2E] & 0x02)) | ((src[0x2F] & 0x02) >> 1);
	c[3][0] = ((src[0x30] & 0x02) << 6) | ((src[0x31] & 0x02) << 5) | ((src[0x32] & 0x02) << 4) | ((src[0x33] & 0x02) << 3) | ((src[0x34] & 0x02) << 2) | ((src[0x35] & 0x02) << 1) | ((src[0x36] & 0x02)) | ((src[0x37] & 0x02) >> 1);

	// Channel 6
	c[5][2] = ((src[0x20] & 0x04) << 5) | ((src[0x21] & 0x04) << 4) | ((src[0x22] & 0x04) << 3) | ((src[0x23] & 0x04) << 2) | ((src[0x24] & 0x04) << 1) | ((src[0x25] & 0x04)) | ((src[0x26] & 0x04) >> 1) | ((src[0x27] & 0x04) >> 2);
	c[5][1] = ((src[0x28] & 0x04) << 5) | ((src[0x29] & 0x04) << 4) | ((src[0x2A] & 0x04) << 3) | ((src[0x2B] & 0x04) << 2) | ((src[0x2C] & 0x04) << 1) | ((src[0x2D] & 0x04)) | ((src[0x2E] & 0x04) >> 1) | ((src[0x2F] & 0x04) >> 2);
	c[5][0] = ((src[0x30] & 0x04) << 5) | ((src[0x31] & 0x04) << 4) | ((src[0x32] & 0x04) << 3) | ((src[0x33] & 0x04) << 2) | ((src[0x34] & 0x04) << 1) | ((src[0x35] & 0x04)) | ((src[0x36] & 0x04) >> 1) | ((src[0x37] & 0x04) >> 2);

	// Channel 8
	c[7][2] = ((src[0x20] & 0x08) << 4) | ((src[0x21] & 0x08) << 3) | ((src[0x22] & 0x08) << 2) | ((src[0x23] & 0x08) << 1) | ((src[0x24] & 0x08)) | ((src[0x25] & 0x08) >> 1) | ((src[0x26] & 0x08) >> 2) | ((src[0x27] & 0x08) >> 3);
	c[7][1] = ((src[0x28] & 0x08) << 4) | ((src[0x29] & 0x08) << 3) | ((src[0x2A] & 0x08) << 2) | ((src[0x2B] & 0x08) << 1) | ((src[0x2C] & 0x08)) | ((src[0x2D] & 0x08) >> 1) | ((src[0x2E] & 0x08) >> 2) | ((src[0x2F] & 0x08) >> 3);
	c[7][0] = ((src[0x30] & 0x08) << 4) | ((src[0x31] & 0x08) << 3) | ((src[0x32] & 0x08) << 2) | ((src[0x33] & 0x08) << 1) | ((src[0x34] & 0x08)) | ((src[0x35] & 0x08) >> 1) | ((src[0x36] & 0x08) >> 2) | ((src[0x37] & 0x08) >> 3);

	for(int i=0; i<8; i++) {
		int32_t s = (c[i][0]) | (c[i][1] << 8) | (c[i][2] << 16);
		if(s & 0x800000) s |= 0xFF000000;
		dst[i] = (float)s / 8388608.0f;
	}
}