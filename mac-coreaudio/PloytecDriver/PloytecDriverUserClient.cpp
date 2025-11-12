#include "PloytecDriver.h"
#include "PloytecDevice.h"
#include "PloytecDriverKeys.h"

// The system includes.
#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/IODispatchSource.h>

#define	DebugMsg(inFormat, args...)	os_log(OS_LOG_DEFAULT, "%s: " inFormat "\n", __FUNCTION__, ##args)

struct PloytecDriverUserClient_IVars
{
	OSSharedPtr<PloytecDriver> mProvider = nullptr;
	OSAction* midiNotificationAction = nullptr;
};

bool PloytecDriverUserClient::init()
{
	if (!super::init()) {
		return false;
	}

	ivars = IONewZero(PloytecDriverUserClient_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}

	return true;
}

void PloytecDriverUserClient::free()
{
	if (ivars != nullptr) {
		if (ivars->midiNotificationAction) {
			ivars->midiNotificationAction->release();
			ivars->midiNotificationAction = nullptr;
		}
		ivars->mProvider.reset();
	}
	IOSafeDeleteNULL(ivars, PloytecDriverUserClient_IVars, 1);
	super::free();
}

kern_return_t PloytecDriverUserClient::Start_Impl(IOService* provider)
{
	kern_return_t ret = kIOReturnSuccess;
	if (provider == nullptr) {
		DebugMsg("provider is null!");
		ret = kIOReturnBadArgument;
		goto Failure;
	}

	ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess) {
		DebugMsg("Failed to start super!");
		goto Failure;
	}

	ivars->mProvider = OSSharedPtr(OSDynamicCast(PloytecDriver, provider), OSRetain);
	
	ivars->mProvider->ivars->midiClient = this;

	return kIOReturnSuccess;

Failure:
	ivars->mProvider.reset();
	return ret;
}

kern_return_t PloytecDriverUserClient::Stop_Impl(IOService* provider)
{
	if (ivars && ivars->mProvider) {
	    if (ivars->mProvider->ivars) {
		ivars->mProvider->ivars->midiClient = nullptr;
	    }
	    ivars->mProvider.reset();
	}

	return Stop(provider, SUPERDISPATCH);
}

kern_return_t PloytecDriverUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments* arguments, const IOUserClientMethodDispatch* dispatch, OSObject* target, void* reference)
{
	kern_return_t ret = kIOReturnSuccess;

	if (ivars == nullptr) {
		return kIOReturnNoResources;
	}
	if (ivars->mProvider.get() == nullptr) {
		return kIOReturnNotAttached;
	}

	switch(static_cast<PloytecDriverExternalMethod>(selector)) {
		case PloytecDriverExternalMethod_Open: {
			ret = kIOReturnSuccess;
			break;
		}

		case PloytecDriverExternalMethod_Close: {
			ret = kIOReturnSuccess;
			break;
		}

		case PloytecDriverExternalMethod_GetDeviceName: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = OSData::withBytes(ivars->mProvider->GetDeviceName()->getCStringNoCopy(), ivars->mProvider->GetDeviceName()->getLength());
			break;
		}

		case PloytecDriverExternalMethod_GetDeviceManufacturer: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = OSData::withBytes(ivars->mProvider->GetDeviceManufacturer()->getCStringNoCopy(), ivars->mProvider->GetDeviceManufacturer()->getLength());
			break;
		}

		case PloytecDriverExternalMethod_GetFirmwareVer: {
			ret = kIOReturnSuccess;
			FirmwareVersion firmwareVersion = ivars->mProvider->GetFirmwareVer();
			arguments->structureOutput = OSData::withBytes(&firmwareVersion, 3);
			break;
		}

		case PloytecDriverExternalMethod_GetPlaybackStats: {
			playbackstats stats;
			ret = ivars->mProvider->GetPlaybackStats(&stats);
			arguments->structureOutput = OSData::withBytes(&stats, sizeof(stats));
			break;
		}

		case PloytecDriverExternalMethod_SetCurrentUrbCount: {
			if (arguments->scalarInputCount < 1) {
				ret = kIOReturnBadArgument;
				break;
			}
			ivars->mProvider->SetCurrentUrbCount(*reinterpret_cast<const uint8_t*>(arguments->scalarInput));
			break;
		}

		case PloytecDriverExternalMethod_GetCurrentUrbCount: {
			if (arguments->scalarOutputCount < 1) {
				ret = kIOReturnBadArgument;
				break;
			}
			arguments->scalarOutput[0] = static_cast<uint64_t>(ivars->mProvider->GetCurrentUrbCount());
			ret = kIOReturnSuccess;
			break;
		}

		case PloytecDriverExternalMethod_SetFrameCount: {
			if (arguments->scalarInputCount < 1) {
				ret = kIOReturnBadArgument;
				break;
			}

			uint64_t packed = arguments->scalarInput[0];
			uint16_t inputFrames = static_cast<uint16_t>(packed & 0xFFFF);
			uint16_t outputFrames = static_cast<uint16_t>((packed >> 32) & 0xFFFF);

			ivars->mProvider->SetFrameCount(inputFrames, outputFrames);

			ret = kIOReturnSuccess;
			break;
		}

		case PloytecDriverExternalMethod_GetCurrentInputFramesCount: {
			if (arguments->scalarOutputCount < 1) {
				ret = kIOReturnBadArgument;
				break;
			}
			arguments->scalarOutput[0] = static_cast<uint64_t>(ivars->mProvider->GetCurrentInputFramesCount());
			ret = kIOReturnSuccess;
			break;
		}

		case PloytecDriverExternalMethod_GetCurrentOutputFramesCount: {
			if (arguments->scalarOutputCount < 1) {
				ret = kIOReturnBadArgument;
				break;
			}
			arguments->scalarOutput[0] = static_cast<uint64_t>(ivars->mProvider->GetCurrentOutputFramesCount());
			ret = kIOReturnSuccess;
			break;
		}

		case PloytecDriverExternalMethod_RegisterForMIDINotification:
			ret = RegisterForMIDINotification_Impl(arguments);
			break;

		case PloytecDriverExternalMethod_SendMIDI:
			return SendMIDI(arguments);
			break;

		default:
			ret = super::ExternalMethod(selector, arguments, dispatch, target, reference);
	};

	return ret;
}

kern_return_t
PloytecDriverUserClient::RegisterForMIDINotification_Impl(IOUserClientMethodArguments *arguments)
{
	if (!arguments || !arguments->completion) {
		os_log(OS_LOG_DEFAULT, "Missing completion in async registration");
		return kIOReturnBadArgument;
	}

	if (ivars->midiNotificationAction)
		ivars->midiNotificationAction->release();

	ivars->midiNotificationAction = arguments->completion;
	ivars->midiNotificationAction->retain();

	return kIOReturnSuccess;
}

kern_return_t
PloytecDriverUserClient::postMIDIMessage(uint64_t msg)
{
	if (!ivars || !ivars->midiNotificationAction)
	{
		return kIOReturnNoResources;
	}

	uint64_t asyncArgs[1] = { msg };
	AsyncCompletion(ivars->midiNotificationAction, kIOReturnSuccess, asyncArgs, 1);

	return kIOReturnSuccess;
}

kern_return_t
PloytecDriverUserClient::SendMIDI(IOUserClientMethodArguments *arguments)
{
	if (!ivars || !ivars->mProvider) {
		return kIOReturnNotAttached;
	}

	if (!arguments || arguments->scalarInputCount < 1) {
		os_log(OS_LOG_DEFAULT, "SendMIDI: Invalid input");
		return kIOReturnBadArgument;
	}

	ivars->mProvider->WriteMIDIBytes(arguments->scalarInput[0]);

	return kIOReturnSuccess;
}
