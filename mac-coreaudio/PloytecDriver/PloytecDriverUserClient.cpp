//
//  PloytecDriverUserClient.cpp
//  PloytecDriver
//
//  Created by Marcel Bierling on 04/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

// The local includes.
#include "PloytecDriverUserClient.h"
#include "PloytecDriver.h"
#include "PloytecDevice.h"
#include "PloytecDriverKeys.h"

// The system includes.
#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/AudioDriverKit.h>

#define	DebugMsg(inFormat, args...)	os_log(OS_LOG_DEFAULT, "%s: " inFormat "\n", __FUNCTION__, ##args)

struct PloytecDriverUserClient_IVars
{
	OSSharedPtr<PloytecDriver> mProvider = nullptr;
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
