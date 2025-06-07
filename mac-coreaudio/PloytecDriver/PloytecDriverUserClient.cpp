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
#include "PloytecAudioDevice.h"
#include "PloytecDriverKeys.h"

// The system includes.
#include <DriverKit/DriverKit.h>
#include <DriverKit/OSSharedPtr.h>
#include <AudioDriverKit/AudioDriverKit.h>

struct PloytecDriverUserClient_IVars
{
	OSSharedPtr<PloytecDriver> driver = nullptr;
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
		ivars->driver.reset();
	}
	IOSafeDeleteNULL(ivars, PloytecDriverUserClient_IVars, 1);
	super::free();
}

kern_return_t PloytecDriverUserClient::Start_Impl(IOService* provider)
{
	kern_return_t ret = kIOReturnSuccess;
	if (provider == nullptr) {
		os_log(OS_LOG_DEFAULT, "Provider is null");
		ret = kIOReturnBadArgument;
		goto Failure;
	}

	ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to start driver");
		goto Failure;
	}

	ivars->driver = OSSharedPtr(OSDynamicCast(PloytecDriver, provider), OSRetain);

	return kIOReturnSuccess;

Failure:
	ivars->driver.reset();
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
	if (ivars->driver.get() == nullptr) {
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
			arguments->structureOutput = ivars->driver->GetDeviceName();
			break;
		}

		case PloytecDriverExternalMethod_GetDeviceManufacturer: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = ivars->driver->GetDeviceManufacturer();
			break;
		}

		case PloytecDriverExternalMethod_GetFirmwareVersion: {
			ret = kIOReturnSuccess;
			arguments->structureOutput = ivars->driver->GetFirmwareVer();
			break;
		}

		default:
			ret = super::ExternalMethod(selector, arguments, dispatch, target, reference);
	};

	return ret;
}
