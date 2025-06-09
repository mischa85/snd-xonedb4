//
//  PloytecDriverUserClient.cpp
//  PloytecDriver
//
//  Created by Marcel Bierling on 04/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

// The local includes.
//#include "PloytecDriverUserClient.h"
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

	return kIOReturnSuccess;

Failure:
	ivars->mProvider.reset();
	return ret;
}

kern_return_t PloytecDriverUserClient::Stop_Impl(IOService* provider)
{
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
			arguments->structureOutput = ivars->mProvider->GetDeviceName();
			break;
		}

		case PloytecDriverExternalMethod_GetDeviceManufacturer: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = ivars->mProvider->GetDeviceManufacturer();
			break;
		}

		case PloytecDriverExternalMethod_GetFirmwareVer: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = ivars->mProvider->GetFirmwareVer();
			break;
		}

		case PloytecDriverExternalMethod_GetPlaybackStats: {
			playbackstats stats;
			ret = ivars->mProvider->GetPlaybackStats(&stats);
			arguments->structureOutput = OSData::withBytes(&stats, sizeof(stats));
			break;
		}

		default:
			ret = super::ExternalMethod(selector, arguments, dispatch, target, reference);
	};

	return ret;
}

kern_return_t
PloytecDriverUserClient::registerForMIDINotification_Impl(IOUserClientMethodArguments* arguments)
{
	if (!arguments || !arguments->completion)
		return kIOReturnBadArgument;

	// Retain the OSAction, we will complete it later
	arguments->completion->retain();
	ivars->midiNotificationAction = arguments->completion;

	return kIOReturnSuccess;
}

kern_return_t
PloytecDriverUserClient::GetNextMIDIMessage_Impl(OSData **msg_out)
{
	if (ivars->mProvider->ivars->midiCount == 0)
		return kIOReturnNoResources;

	uint64_t msg = ivars->mProvider->ivars->midiRingBuffer[ivars->mProvider->ivars->midiReadIndex];
	ivars->mProvider->ivars->midiReadIndex = (ivars->mProvider->ivars->midiReadIndex + 1) % 255;
	ivars->mProvider->ivars->midiCount--;

	*msg_out = OSData::withBytes(&msg, sizeof(msg));
	return (*msg_out) ? kIOReturnSuccess : kIOReturnNoMemory;
}

kern_return_t
PloytecDriverUserClient::postMIDIMessage(uint64_t msg)
{
	if (!ivars || !ivars->midiNotificationAction)
		return kIOReturnNoResources;

	uint64_t asyncArgs[1] = { msg };

	// Send async result
	AsyncCompletion(ivars->midiNotificationAction, kIOReturnSuccess, asyncArgs, 1);

	// Release the OSAction, we are done using it
	ivars->midiNotificationAction->release();
	ivars->midiNotificationAction = nullptr;

	return kIOReturnSuccess;
}

