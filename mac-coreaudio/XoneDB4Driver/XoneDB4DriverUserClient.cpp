//
//  XoneDB4DriverUserClient.cpp
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 04/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

// The local includes.
#include "XoneDB4DriverUserClient.h"
#include "XoneDB4Driver.h"
#include "XoneDB4Device.h"
#include "XoneDB4DriverKeys.h"

// The system includes.
#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/AudioDriverKit.h>

#define	DebugMsg(inFormat, args...)	os_log(OS_LOG_DEFAULT, "%s: " inFormat "\n", __FUNCTION__, ##args)

struct XoneDB4DriverUserClient_IVars
{
	OSSharedPtr<XoneDB4Driver> mProvider = nullptr;
};

bool XoneDB4DriverUserClient::init()
{
	if (!super::init()) {
		return false;
	}

	ivars = IONewZero(XoneDB4DriverUserClient_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}

	return true;
}

void XoneDB4DriverUserClient::free()
{
	if (ivars != nullptr) {
		ivars->mProvider.reset();
	}
	IOSafeDeleteNULL(ivars, XoneDB4DriverUserClient_IVars, 1);
	super::free();
}

kern_return_t XoneDB4DriverUserClient::Start_Impl(IOService* provider)
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

	ivars->mProvider = OSSharedPtr(OSDynamicCast(XoneDB4Driver, provider), OSRetain);

	return kIOReturnSuccess;

Failure:
	ivars->mProvider.reset();
	return ret;
}

kern_return_t XoneDB4DriverUserClient::Stop_Impl(IOService* provider)
{
	return Stop(provider, SUPERDISPATCH);
}

kern_return_t XoneDB4DriverUserClient::ExternalMethod(uint64_t selector, IOUserClientMethodArguments* arguments, const IOUserClientMethodDispatch* dispatch, OSObject* target, void* reference)
{
	kern_return_t ret = kIOReturnSuccess;

	if (ivars == nullptr) {
		return kIOReturnNoResources;
	}
	if (ivars->mProvider.get() == nullptr) {
		return kIOReturnNotAttached;
	}

	switch(static_cast<XoneDB4DriverExternalMethod>(selector)) {
		case XoneDB4DriverExternalMethod_Open: {
			ret = kIOReturnSuccess;
			break;
		}

		case XoneDB4DriverExternalMethod_Close: {
			ret = kIOReturnSuccess;
			break;
		}

		case XoneDB4DriverExternalMethod_GetDeviceName: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = ivars->mProvider->GetDeviceName();
			break;
		}

		case XoneDB4DriverExternalMethod_GetDeviceManufacturer: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = ivars->mProvider->GetDeviceManufacturer();
			break;
		}

		case XoneDB4DriverExternalMethod_GetFirmwareVer: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = ivars->mProvider->GetFirmwareVer();
			break;
		}

		case XoneDB4DriverExternalMethod_ChangeBufferSize: {
			ret = kIOReturnSuccess;
			OSNumber* buffersize = OSNumber::withNumber(*static_cast<const uint64_t*>(arguments->scalarInput), sizeof(arguments->scalarInput));
			ivars->mProvider->ChangeBufferSize(buffersize);
			break;
		}

		case XoneDB4DriverExternalMethod_GetPlaybackStats: {
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
