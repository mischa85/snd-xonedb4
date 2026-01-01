#include <CoreAudio/CoreAudio.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <mach/mach_time.h>
#include "PloytecDriver.h"
#include "PloytecAudioDevice.h"

static bool gIsRunning = false;

static void LogCall(const char* fn, AudioObjectID obj, UInt32 a=0, UInt64 b=0, UInt64 c=0) {
	os_log_error(GetLog(), "[PloytecHAL] CALL %{public}s obj=%u a=%u b=%llu c=%llu", fn, (unsigned)obj, (unsigned)a, (unsigned long long)b, (unsigned long long)c);
}

static UInt32 GetAudioBufferListSize(UInt32 numBuffers) {
	return (UInt32)(offsetof(AudioBufferList, mBuffers) + (numBuffers * sizeof(AudioBuffer)));
}

static bool IsOutputScope(AudioObjectPropertyScope scope) {
	return (scope == kAudioObjectPropertyScopeOutput || scope == kAudioObjectPropertyScopeGlobal);
}

static void LogUnknown(const char* where, AudioObjectID obj, const AudioObjectPropertyAddress* addr, OSStatus err) {
	UInt32 prop = CFSwapInt32HostToBig(addr->mSelector);
	UInt32 scope = CFSwapInt32HostToBig(addr->mScope);
	os_log_error(GetLog(), "[PloytecHAL] %{public}s Obj:%u Sel:'%{public}4.4s' Scope:'%{public}4.4s' Elem:%u -> 0x%{public}08X", where, (unsigned)obj, (char*)&prop, (char*)&scope, (unsigned)addr->mElement, (unsigned)err);
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
			case kAudioDevicePropertyAvailableNominalSampleRates: case kAudioDevicePropertyBufferFrameSize:
			case kAudioDevicePropertyBufferFrameSizeRange: case kAudioDevicePropertyStreams:
			case kAudioDevicePropertyStreamConfiguration: case kAudioDevicePropertyLatency:
			case kAudioDevicePropertyZeroTimeStampPeriod: case kAudioDevicePropertyPreferredChannelLayout:
				return true;
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

extern "C" HRESULT PloytecGetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32* outDataSize) {
	if (!outDataSize) return kAudioHardwareIllegalOperationError;
	*outDataSize = 0;

	if (inObjectID == kAudioObjectPlugInObject) {
		switch (inAddress->mSelector) {
			case kAudioPlugInPropertyDeviceList: case kAudioObjectPropertyOwnedObjects:
				*outDataSize = PloytecDriver::GetInstance().IsConnected() ? sizeof(AudioObjectID) : 0; return kAudioHardwareNoError;
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
			case kAudioDevicePropertyBufferFrameSize: case kAudioDevicePropertyBufferFrameSizeRange: *outDataSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
			case kAudioDevicePropertyStreams:
				if (inAddress->mScope == kAudioObjectPropertyScopeInput) *outDataSize = sizeof(AudioObjectID);
				else if (IsOutputScope(inAddress->mScope)) *outDataSize = sizeof(AudioObjectID);
				else if (inAddress->mScope == kAudioObjectPropertyScopeGlobal) *outDataSize = 2 * sizeof(AudioObjectID);
				else *outDataSize = 0;
				return kAudioHardwareNoError;
			case kAudioDevicePropertyStreamConfiguration:
				*outDataSize = GetAudioBufferListSize((IsOutputScope(inAddress->mScope) ? PloytecAudioDevice::Get().GetOutputStreamConfiguration() : PloytecAudioDevice::Get().GetInputStreamConfiguration())->mNumberBuffers);
				return kAudioHardwareNoError;
			case kAudioDevicePropertyLatency: case kAudioDevicePropertyZeroTimeStampPeriod: *outDataSize = sizeof(UInt32); return kAudioHardwareNoError;
			case kAudioDevicePropertyPreferredChannelLayout: *outDataSize = PloytecAudioDevice::Get().GetPreferredChannelLayoutSize(inAddress->mScope); return kAudioHardwareNoError;
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
				if (!PloytecDriver::GetInstance().IsConnected()) { if (outSize) *outSize = 0; return kAudioHardwareNoError; }
				return WriteObjectID(inMax, outSize, outData, kPloytecDeviceID);
			case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
			case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, CFSTR("Ploytec HAL"));
			case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, CFSTR("Ploytec"));
			case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
			case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioPlugInClassID);
			default: LogUnknown("Plugin.GetData", inObjectID, inAddress, kAudioHardwareUnknownPropertyError); return kAudioHardwareUnknownPropertyError;
		}
	}
	if (inObjectID == kPloytecDeviceID) {
		switch (inAddress->mSelector) {
			case kAudioObjectPropertyBaseClass: return WriteClassID(inMax, outSize, outData, kAudioObjectClassID);
			case kAudioObjectPropertyClass: return WriteClassID(inMax, outSize, outData, kAudioDeviceClassID);
			case kAudioObjectPropertyControlList: if (outSize) *outSize = 0; return kAudioHardwareNoError;
			case kAudioObjectPropertyOwner: return WriteObjectID(inMax, outSize, outData, kAudioObjectPlugInObject);
			case kAudioObjectPropertyName: return WriteCFString(inMax, outSize, outData, PloytecAudioDevice::Get().GetDeviceName());
			case kAudioObjectPropertyManufacturer: return WriteCFString(inMax, outSize, outData, PloytecAudioDevice::Get().GetManufacturer());
			case kAudioDevicePropertyDeviceUID: return WriteCFString(inMax, outSize, outData, PloytecAudioDevice::Get().GetDeviceUID());
			case kAudioDevicePropertyModelUID: return WriteCFString(inMax, outSize, outData, PloytecAudioDevice::Get().GetModelUID());
			case kAudioDevicePropertyTransportType: return WriteUInt32(inMax, outSize, outData, PloytecAudioDevice::Get().GetTransportType());
			case kAudioDevicePropertyIsHidden: return WriteUInt32(inMax, outSize, outData, 0);
			case kAudioDevicePropertyDeviceIsAlive: return WriteUInt32(inMax, outSize, outData, PloytecDriver::GetInstance().IsConnected() ? 1 : 0);
			case kAudioDevicePropertyDeviceIsRunning: case kAudioDevicePropertyDeviceIsRunningSomewhere: return WriteUInt32(inMax, outSize, outData, gIsRunning ? 1 : 0);
			case kAudioDevicePropertyNominalSampleRate: return WriteFloat64(inMax, outSize, outData, PloytecAudioDevice::Get().GetNominalSampleRate());
			case kAudioDevicePropertyAvailableNominalSampleRates: if (outSize) *outSize = sizeof(AudioValueRange); if (outData) { if (inMax < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError; *(AudioValueRange*)outData = PloytecAudioDevice::Get().GetAvailableSampleRates(); } return kAudioHardwareNoError;
			case kAudioDevicePropertyBufferFrameSize: return WriteUInt32(inMax, outSize, outData, PloytecAudioDevice::Get().GetBufferFrameSize());
			case kAudioDevicePropertyBufferFrameSizeRange: if (inMax < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError; *(AudioValueRange*)outData = PloytecAudioDevice::Get().GetBufferFrameSizeRange(); if (outSize) *outSize = sizeof(AudioValueRange); return kAudioHardwareNoError;
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
				AudioBufferList* config = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? PloytecAudioDevice::Get().GetInputStreamConfiguration() : PloytecAudioDevice::Get().GetOutputStreamConfiguration();
				UInt32 actualSize = GetAudioBufferListSize(config->mNumberBuffers); if (outSize) *outSize = actualSize;
				if (outData) { if (inMax < actualSize) return kAudioHardwareBadPropertySizeError; memcpy(outData, config, actualSize); } return kAudioHardwareNoError;
			}
			case kAudioDevicePropertyClockDomain: return WriteUInt32(inMax, outSize, outData, PloytecAudioDevice::Get().GetClockDomain());
			case kAudioDevicePropertyLatency: return WriteUInt32(inMax, outSize, outData, PloytecAudioDevice::Get().GetLatency());
			case kAudioDevicePropertySafetyOffset: return WriteUInt32(inMax, outSize, outData, PloytecAudioDevice::Get().GetSafetyOffset());
			case kAudioDevicePropertyZeroTimeStampPeriod: return WriteUInt32(inMax, outSize, outData, PloytecAudioDevice::Get().GetZeroTimestampPeriod());
			case kAudioDevicePropertyPreferredChannelLayout: {
				UInt32 size = PloytecAudioDevice::Get().GetPreferredChannelLayoutSize(inAddress->mScope);
				if (outSize) *outSize = size;
				if (outData) { if (inMax < size) return kAudioHardwareBadPropertySizeError; memcpy(outData, PloytecAudioDevice::Get().GetPreferredChannelLayout(inAddress->mScope), size); }
				return kAudioHardwareNoError;
			}
			default: LogUnknown("Device.GetData", inObjectID, inAddress, kAudioHardwareUnknownPropertyError); return kAudioHardwareUnknownPropertyError;
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
			case kAudioStreamPropertyLatency: return WriteUInt32(inMax, outSize, outData, PloytecAudioDevice::Get().GetLatency());
			case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat:
				if (outData && inMax < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
				if (outData) *(AudioStreamBasicDescription*)outData = PloytecAudioDevice::Get().GetStreamFormat();
				if (outSize) *outSize = sizeof(AudioStreamBasicDescription); return kAudioHardwareNoError;
			case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats:
				if (outData && inMax < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
				if (outData) *(AudioStreamRangedDescription*)outData = PloytecAudioDevice::Get().GetStreamRangedDescription();
				if (outSize) *outSize = sizeof(AudioStreamRangedDescription); return kAudioHardwareNoError;
			default: LogUnknown("Stream.GetData", inObjectID, inAddress, kAudioHardwareUnknownPropertyError); return kAudioHardwareUnknownPropertyError;
		}
	}
	LogUnknown("Unknown.GetData", inObjectID, inAddress, kAudioHardwareUnknownPropertyError);
	return kAudioHardwareBadObjectError;
}

extern "C" HRESULT PloytecIsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable) { *outIsSettable = false; return kAudioHardwareNoError; }
extern "C" HRESULT PloytecSetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t, const AudioObjectPropertyAddress* inAddress, UInt32, const void*, UInt32 inSize, const void* inData) { return kAudioHardwareUnsupportedOperationError; }
extern "C" HRESULT PloytecQueryInterface(void*, REFIID inREFIID, LPVOID* outInterface) { extern AudioServerPlugInDriverRef gPloytecDriverRef; CFUUIDBytes bytes = CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID); if (outInterface && memcmp(&inREFIID, &bytes, sizeof(REFIID)) == 0) { *outInterface = gPloytecDriverRef; return kAudioHardwareNoError; } return kAudioHardwareUnknownPropertyError; }
extern "C" ULONG PloytecAddRef(void*) { return 1; }
extern "C" ULONG PloytecRelease(void*) { return 1; }
extern "C" HRESULT PloytecInitialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef inHost) { PloytecDriver& drv = PloytecDriver::GetInstance(); os_log(GetLog(), "[PloytecHAL] >>> INITIALIZE <<<"); drv.SetHost(inHost); drv.Initialize(); return kAudioHardwareNoError; }
extern "C" HRESULT PloytecCreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID) { os_log(GetLog(), "[PloytecHAL] >>> CREATEDEVICE <<<"); return kAudioHardwareNoError; }
extern "C" HRESULT PloytecDestroyDevice(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID) { LogCall("DestroyDevice", inDeviceObjectID); return kAudioHardwareNoError; }
extern "C" HRESULT PloytecAddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) { return kAudioHardwareNoError; }
extern "C" HRESULT PloytecRemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) { return kAudioHardwareNoError; }
extern "C" HRESULT PloytecPerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) { LogCall("PerformDeviceConfigurationChange", inDeviceObjectID, 0, inChangeAction); return kAudioHardwareNoError; }
extern "C" HRESULT PloytecAbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) { LogCall("AbortDeviceConfigurationChange", inDeviceObjectID, 0, inChangeAction); return kAudioHardwareNoError; }
extern "C" HRESULT PloytecStartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) { gIsRunning = true; return PloytecAudioDevice::Get().StartIO(); }
extern "C" HRESULT PloytecStopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID) { gIsRunning = false; return PloytecAudioDevice::Get().StopIO(); }
extern "C" HRESULT PloytecGetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) { return PloytecAudioDevice::Get().GetZeroTimeStamp(inDriver, inDeviceObjectID, inClientID, outSampleTime, outHostTime, outSeed); }
extern "C" OSStatus PloytecWillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace) { bool willDo=true; if(outWillDo) *outWillDo=willDo; if(outWillDoInPlace) *outWillDoInPlace=true; return 0; }
extern "C" OSStatus PloytecBeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) { return 0; }
extern "C" OSStatus PloytecDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) { return PloytecAudioDevice::Get().ioOperationHandler(inDriver, inDeviceObjectID, inStreamObjectID, inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo, ioMainBuffer, ioSecondaryBuffer); }
extern "C" OSStatus PloytecEndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo) { return 0; }

static AudioServerPlugInDriverInterface gPloytecInterface = { NULL, PloytecQueryInterface, PloytecAddRef, PloytecRelease, PloytecInitialize, PloytecCreateDevice, PloytecDestroyDevice, PloytecAddDeviceClient, PloytecRemoveDeviceClient, PloytecPerformDeviceConfigurationChange, PloytecAbortDeviceConfigurationChange, PloytecHasProperty, PloytecIsPropertySettable, PloytecGetPropertyDataSize, PloytecGetPropertyData, PloytecSetPropertyData, PloytecStartIO, PloytecStopIO, PloytecGetZeroTimeStamp, PloytecWillDoIOOperation, PloytecBeginIOOperation, PloytecDoIOOperation, PloytecEndIOOperation };
static AudioServerPlugInDriverInterface* gPloytecInterfacePtr = &gPloytecInterface;
AudioServerPlugInDriverRef gPloytecDriverRef = &gPloytecInterfacePtr;

extern "C" __attribute__((visibility("default"))) void* PloytecPluginFactory(CFAllocatorRef, CFUUIDRef inRequestedTypeUUID) {
	if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return (void*)gPloytecDriverRef;
	return NULL;
}