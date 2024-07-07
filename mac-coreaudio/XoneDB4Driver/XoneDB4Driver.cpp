//
//  XoneDB4Driver.cpp
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "XoneDB4Driver.h"
#include "XoneDB4Device.h"

constexpr uint32_t k_zero_time_stamp_period = 640;

#define __Require(assertion, exceptionLabel)                               \
do                                                                      \
{                                                                       \
if ( __builtin_expect(!(assertion), 0) )                            \
{                                                                   \
goto exceptionLabel;                                            \
}                                                                   \
} while ( 0 )

#define __Require_Action(assertion, exceptionLabel, action)                \
do                                                                      \
{                                                                       \
if ( __builtin_expect(!(assertion), 0) )                            \
{                                                                   \
{                                                               \
action;                                                     \
}                                                               \
goto exceptionLabel;                                            \
}                                                                   \
} while ( 0 )

static const uint8_t kMIDIinEndpointAddress = 0x83;
static const uint8_t kPCMoutEndpointAddress = 0x05;
static const uint8_t kPCMinEndpointAddress = 0x86;

//static const uint16_t kPCMPacketSize = 512;
static const uint16_t kPCMPacketSize = 482; // 480 PCM + 2 UART

static const char8_t sampleratebytes48[3] = { 0x80, 0xBB, 0x00 };
static const char8_t sampleratebytes96[3] = { 0x00, 0x77, 0x01 };

struct XoneDB4Driver_IVars
{
	OSSharedPtr<IODispatchQueue>	m_work_queue;
	OSSharedPtr<XoneDB4Device>		m_audio_device;

	IOUSBHostDevice					*device;
	IOUSBHostInterface				*interface0;
	IOUSBHostInterface				*interface1;
	IOBufferMemoryDescriptor		*receivebuffer;
	char							*firmwarever;
	IOBufferMemoryDescriptor		*sampleratebytes;
	IOUSBHostPipe					*MIDIinPipe;
	IOUSBHostPipe					*PCMoutPipe;
	IOUSBHostPipe					*PCMinPipe;
	OSAction						*PCMoutCallback;
	OSAction						*PCMinCallback;
	IOBufferMemoryDescriptor		*PCMoutData;
	IOBufferMemoryDescriptor		*PCMinData;
	uint16_t						PCMPacketSize = 512;
	
	OSSharedPtr<OSString>			FirmwareVersionBytes;
};

bool XoneDB4Driver::init()
{
	bool result = false;
	
	result = super::init();
	__Require(true == result, Exit);

	ivars = IONewZero(XoneDB4Driver_IVars, 1);
	__Require_Action(NULL != ivars, Exit, result = false);

Exit:
	return result;
}

kern_return_t IMPL(XoneDB4Driver, Start)
{
	kern_return_t                       ret;
	uintptr_t                           interfaceIterator;
	IOAddressSegment                    range;

	int i, j;
	char* manufacturer_utf8;
	char* device_name_utf8;
	bool success = false;
	auto device_uid = OSSharedPtr(OSString::withCString("xonedb4"), OSNoRetain);
	auto model_uid = OSSharedPtr(OSString::withCString("Model UID"), OSNoRetain);
	OSSharedPtr<OSString> manufacturer_uid;
	OSSharedPtr<OSString> device_name;
	//char* tempstring;
	
	uint16_t bytesTransferred;
	
	ret = Start(provider, SUPERDISPATCH);
	__Require(kIOReturnSuccess == ret, Exit);

	ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
	__Require_Action(NULL != ivars->device, Exit, ret = kIOReturnNoDevice);

	ret = ivars->device->Open(this, 0, NULL);
	__Require(kIOReturnSuccess == ret, Exit);

	manufacturer_utf8 = new char[(ivars->device->CopyStringDescriptor(1)->bLength / 2)];
	j = 0;
	for(i = 0; i < (ivars->device->CopyStringDescriptor(1)->bLength / 2); i++) {
		manufacturer_utf8[i] = ivars->device->CopyStringDescriptor(1)->bString[j];
		j+=2;
	}

	device_name_utf8 = new char[(ivars->device->CopyStringDescriptor(2)->bLength / 2)];
	j = 0;
	for(i = 0; i < (ivars->device->CopyStringDescriptor(2)->bLength / 2); i++) {
		device_name_utf8[i] = ivars->device->CopyStringDescriptor(2)->bString[j];
		j+=2;
	}

	manufacturer_uid = OSSharedPtr(OSString::withCString(manufacturer_utf8), OSNoRetain);
	device_name = OSSharedPtr(OSString::withCString(device_name_utf8), OSNoRetain);

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 0x0f, 0, &ivars->receivebuffer);
	__Require(kIOReturnSuccess == ret, Exit);
	
	// get firmware version
	// 0x31 0x01 0x27 = 1.3.9
	// 0x31 0x01 0x32 = 1.5.0
	ret = ivars->device->DeviceRequest(this, 0xc0, 0x56, 0x00, 0x00, 0x0f, ivars->receivebuffer, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);
	
	ret = ivars->receivebuffer->GetAddressRange(&range);
	__Require(kIOReturnSuccess == ret, Exit);
	
	os_log(OS_LOG_DEFAULT, "GOT HERE 1");
	ivars->firmwarever = new char[range.length];
	os_log(OS_LOG_DEFAULT, "GOT HERE 2");
	memcpy(ivars->firmwarever, reinterpret_cast<const uint8_t *>(range.address), range.length);
	os_log(OS_LOG_DEFAULT, "GOT HERE 3");
	//ivars->firmwarever[range.length] = 0x00;
	os_log(OS_LOG_DEFAULT, "GOT HERE 4");
	
	os_log(OS_LOG_DEFAULT, "%s firmwarever: %02X %02X %02X", __FUNCTION__, ivars->firmwarever[0], ivars->firmwarever[1], ivars->firmwarever[2]);
	
	/*
	ivars->FirmwareVersionBytes = OSSharedPtr(OSString::withCString(tempstring), OSNoRetain);
	
	os_log(OS_LOG_DEFAULT, "%s FIRMWARE:", __FUNCTION__);
	for (uint16_t i = 0; i < bytesTransferred; i++) {
		os_log(OS_LOG_DEFAULT, "%02x ", reinterpret_cast<const uint8_t *>(range.address)[i]);
	}
	*/

	ret = ivars->sampleratebytes->Create(kIOMemoryDirectionInOut, 0x03, 0, &ivars->sampleratebytes);
	__Require(kIOReturnSuccess == ret, Exit);

	// get status
	ret = ivars->device->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);
	
	ret = ivars->sampleratebytes->GetAddressRange(&range);
	__Require(kIOReturnSuccess == ret, Exit);
	
	os_log(OS_LOG_DEFAULT, "%s STATUS:", __FUNCTION__);
	for (uint16_t i = 0; i < bytesTransferred; i++) {
		os_log(OS_LOG_DEFAULT, "%02x ", reinterpret_cast<const uint8_t *>(range.address)[i]);
	}
	
	// get samplerate

	ret = ivars->device->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->sampleratebytes->GetAddressRange(&range);
	__Require(kIOReturnSuccess == ret, Exit);

	os_log(OS_LOG_DEFAULT, "%s CURRENT SAMPLERATE:", __FUNCTION__);
	for (uint16_t i = 0; i < bytesTransferred; i++) {
		os_log(OS_LOG_DEFAULT, "%02x ", reinterpret_cast<const uint8_t *>(range.address)[i]);
	}

	ret = ivars->sampleratebytes->GetAddressRange(&range);
	__Require(kIOReturnSuccess == ret, Exit);

	memcpy(reinterpret_cast<void *>(range.address), sampleratebytes96, 3);
	
	// set 96 khz
	
	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	// get samplerate

	ret = ivars->device->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->sampleratebytes->GetAddressRange(&range);
	__Require(kIOReturnSuccess == ret, Exit);

	os_log(OS_LOG_DEFAULT, "%s SET SAMPLERATE:", __FUNCTION__);
	for (uint16_t i = 0; i < bytesTransferred; i++) {
		os_log(OS_LOG_DEFAULT, "%02x ", reinterpret_cast<const uint8_t *>(range.address)[i]);
	}

	// get status
	
	ret = ivars->device->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->sampleratebytes->GetAddressRange(&range);
	__Require(kIOReturnSuccess == ret, Exit);

	os_log(OS_LOG_DEFAULT, "%s STATUS:", __FUNCTION__);
	// Print the contents in hexadecimal format
	for (uint16_t i = 0; i < bytesTransferred; i++) {
		os_log(OS_LOG_DEFAULT, "%02x ", reinterpret_cast<const uint8_t *>(range.address)[i]);
	}

	// allgood

	ret = ivars->device->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, ivars->sampleratebytes, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->SetConfiguration(1, false);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->CreateInterfaceIterator(&interfaceIterator);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->CopyInterface(interfaceIterator, &ivars->interface0);
	__Require(kIOReturnSuccess == ret, Exit);
	
	ret = ivars->interface0->Open(this, NULL, NULL);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->interface0->SelectAlternateSetting(1);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->CopyInterface(interfaceIterator, &ivars->interface1);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->interface1->Open(this, NULL, NULL);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->interface1->SelectAlternateSetting(1);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->interface0->CopyPipe(kMIDIinEndpointAddress, &ivars->MIDIinPipe);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->interface1->CopyPipe(kPCMinEndpointAddress, &ivars->PCMinPipe);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->interface0->CopyPipe(kPCMoutEndpointAddress, &ivars->PCMoutPipe);
	__Require(kIOReturnSuccess == ret, Exit);

	ivars->PCMPacketSize = kPCMPacketSize;

	ret = OSAction::Create(this, XoneDB4Driver_PCMinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->PCMinCallback);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = OSAction::Create(this, XoneDB4Driver_PCMoutHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->PCMoutCallback);
	__Require(kIOReturnSuccess == ret, Exit);

	ivars->m_work_queue = GetWorkQueue();
	__Require_Action(NULL != ivars->m_work_queue.get(), Exit, ret = kIOReturnInvalid);

	ivars->m_audio_device = OSSharedPtr(OSTypeAlloc(XoneDB4Device), OSNoRetain);
	__Require_Action(NULL != ivars->m_audio_device.get(), Exit, ret = kIOReturnNoMemory);

    success = ivars->m_audio_device->init(this, false, device_uid.get(), model_uid.get(), manufacturer_uid.get(), k_zero_time_stamp_period, ivars->PCMinPipe, ivars->PCMinCallback, ivars->PCMoutPipe, ivars->PCMoutCallback, ivars->PCMPacketSize, ivars->device);
	__Require_Action(false != success, Exit, ret = kIOReturnNoMemory);

	ivars->m_audio_device->SetName(device_name.get());

	AddObject(ivars->m_audio_device.get());

	ret = RegisterService();
	__Require(kIOReturnSuccess == ret, Exit);
Exit:
	return ret;
}

kern_return_t IMPL(XoneDB4Driver, Stop)
{
	kern_return_t ret = kIOReturnSuccess;

	os_log(OS_LOG_DEFAULT, "stop ding %s", __FUNCTION__);

	return ret;
}

void XoneDB4Driver::free()
{
	if (ivars != nullptr)
	{
		ivars->m_work_queue.reset();
		ivars->m_audio_device.reset();
	}
	IOSafeDeleteNULL(ivars, XoneDB4Driver_IVars, 1);
	super::free();
}

kern_return_t XoneDB4Driver::NewUserClient_Impl(uint32_t in_type, IOUserClient** out_user_client)
{
	kern_return_t error = kIOReturnSuccess;
	
	// Have the superclass create the IOUserAudioDriverUserClient object
	// if the type is kIOUserAudioDriverUserClientType.
	if (in_type == kIOUserAudioDriverUserClientType)
	{
		error = super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
		FailIfError(error, , Failure, "Failed to create user client");
		FailIfNULL(*out_user_client, error = kIOReturnNoMemory, Failure, "Failed to create user client");
	}
	else
	{
		IOService* user_client_service = nullptr;
		error = Create(this, "XoneDB4DriverUserClientProperties", &user_client_service);
		FailIfError(error, , Failure, "failed to create the XoneDB4Driver user client");
		*out_user_client = OSDynamicCast(IOUserClient, user_client_service);
	}
	
Failure:
	return error;
}

kern_return_t XoneDB4Driver::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->m_audio_device->GetObjectID())
	{
		os_log(OS_LOG_DEFAULT, "AudioDriver::StartDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	__block kern_return_t ret;
	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StartDevice(in_object_id, in_flags);
	});
	if (ret == kIOReturnSuccess)
	{
		// Enable any custom driver-related things here.
	}
	return ret;
}

kern_return_t XoneDB4Driver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->m_audio_device->GetObjectID())
	{
		os_log(OS_LOG_DEFAULT, "AudioDriver::StopDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	__block kern_return_t ret;
	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StopDevice(in_object_id, in_flags);
	});

	if (ret == kIOReturnSuccess)
	{
		// Disable any custom driver-related things here.
	}
	return ret;
}

OSData* XoneDB4Driver::GetFirmwareVer()
{
	return OSData::withBytes(ivars->firmwarever, sizeof(ivars->firmwarever));
}

kern_return_t IMPL(XoneDB4Driver, PCMinHandler)
{
	kern_return_t ret;

	ret = ivars->m_audio_device->ReceivePCMfromDevice(completionTimestamp);
	__Require(kIOReturnSuccess == ret, Exit);
 
Exit:
	return ret;
}

kern_return_t IMPL(XoneDB4Driver, PCMoutHandler)
{
	kern_return_t ret;
 
	ret = ivars->m_audio_device->SendPCMToDevice(completionTimestamp);
	__Require(kIOReturnSuccess == ret, Exit);
 
Exit:
	return ret;
}
