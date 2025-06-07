//
//  PloytecDriver.cpp
//  PloytecDriver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecAudioDriver.h"
#include "PloytecAudioDevice.h"
#include "PloytecDriver.h"

constexpr uint32_t k_zero_time_stamp_period = 32768;

struct PloytecAudioDriver_IVars
{
	OSSharedPtr<IODispatchQueue>	workQueue;
	OSSharedPtr<PloytecDriver>	driver;
	OSSharedPtr<PloytecAudioDevice>	audioDevice;
	OSSharedPtr<OSString>		manufacturerName;
	OSSharedPtr<OSString>		deviceName;
};

bool PloytecAudioDriver::init()
{
	os_log(OS_LOG_DEFAULT, "PloytecAudioDriver::init()");
	
	bool result = false;
	
	os_log(OS_LOG_DEFAULT, "got in audio driver init 1");
	
	result = super::init();
	if (!result) {
		return false;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver init 2");

	ivars = IONewZero(PloytecAudioDriver_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver init 3");

Exit:
	return result;
}

kern_return_t IMPL(PloytecAudioDriver, Start)
{
	os_log(OS_LOG_DEFAULT, "PloytecAudioDriver::start()");
	
	kern_return_t ret;
	bool success = false;

	ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to start driver");
		goto Exit;
	}

	ivars->workQueue = GetWorkQueue();
	if (ivars->workQueue.get() == nullptr)
	{
		os_log(OS_LOG_DEFAULT, "Failed to get work queue");
		ret = kIOReturnInvalid;
		goto Exit;
	}

	ivars->audioDevice = OSSharedPtr(OSTypeAlloc(PloytecAudioDevice), OSNoRetain);
	if (ivars->audioDevice.get() == nullptr)
	{
		os_log(OS_LOG_DEFAULT, "Failed to allocate memory for audio device");
		ret = kIOReturnNoMemory;
		goto Exit;
	}
	
	ivars->manufacturerName = OSSharedPtr<OSString>(OSString::withCString("blah1"), OSNoRetain);
	ivars->deviceName = OSSharedPtr<OSString>(OSString::withCString("blah2"), OSNoRetain);
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 1");

	success = ivars->audioDevice->init(this, false, ivars->manufacturerName.get(), ivars->deviceName.get(), ivars->deviceName.get(), k_zero_time_stamp_period);
	if (!success)
	{
		os_log(OS_LOG_DEFAULT, "Failed to initialize audio device");
		ret = kIOReturnNoMemory;
		goto Exit;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 2");
	
	ivars->audioDevice->SetName(ivars->deviceName.get());
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set name to audio device");
		goto Exit;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 3");

	ret = AddObject(ivars->audioDevice.get());
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to add audio device object");
		goto Exit;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 4");

	ret = RegisterService();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to register service");
		goto Exit;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 5");

Exit:
	return ret;
}

kern_return_t IMPL(PloytecAudioDriver, Stop)
{
	kern_return_t ret = kIOReturnSuccess;

	os_log(OS_LOG_DEFAULT, "stop ding %s", __FUNCTION__);

	return ret;
}

void PloytecAudioDriver::free()
{
	if (ivars != nullptr)
	{
		ivars->workQueue.reset();
		ivars->audioDevice.reset();
	}
	IOSafeDeleteNULL(ivars, PloytecAudioDriver_IVars, 1);
	super::free();
}

kern_return_t PloytecAudioDriver::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->audioDevice->GetObjectID())
	{
		os_log(OS_LOG_DEFAULT, "AudioDriver::StartDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	__block kern_return_t ret;
	ivars->workQueue->DispatchSync(^(){
		ret = super::StartDevice(in_object_id, in_flags);
	});
	if (ret == kIOReturnSuccess)
	{
		// Enable any custom driver-related things here.
	}
	return ret;
}

kern_return_t PloytecAudioDriver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->audioDevice->GetObjectID())
	{
		os_log(OS_LOG_DEFAULT, "AudioDriver::StopDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	__block kern_return_t ret;
	ivars->workQueue->DispatchSync(^(){
		ret = super::StopDevice(in_object_id, in_flags);
	});

	if (ret == kIOReturnSuccess)
	{
		// Disable any custom driver-related things here.
	}
	return ret;
}

kern_return_t IMPL(PloytecAudioDriver, FillPCMPacketBulk)
{
	if (!ivars || !ivars->audioDevice)
		return kIOReturnNotReady;

	return ivars->audioDevice->FillPCMPacketBulk(buffer, packetSize, timestamp)
		? kIOReturnSuccess
		: kIOReturnError;
}

kern_return_t IMPL(PloytecAudioDriver, FillPCMPacketInt)
{
	if (!ivars || !ivars->audioDevice)
		return kIOReturnNotReady;

	return ivars->audioDevice->FillPCMPacketBulk(buffer, packetSize, timestamp)
		? kIOReturnSuccess
		: kIOReturnError;
}

kern_return_t IMPL(PloytecAudioDriver, ReadPCMPacketBulk)
{
	if (!ivars || !ivars->audioDevice)
		return kIOReturnNotReady;

	return ivars->audioDevice->FillPCMPacketBulk(buffer, packetSize, timestamp)
		? kIOReturnSuccess
		: kIOReturnError;
}

kern_return_t IMPL(PloytecAudioDriver, ReadPCMPacketInt)
{
	if (!ivars || !ivars->audioDevice)
		return kIOReturnNotReady;

	return ivars->audioDevice->FillPCMPacketBulk(buffer, packetSize, timestamp)
		? kIOReturnSuccess
		: kIOReturnError;
}
