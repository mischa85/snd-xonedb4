//
//  XoneDB4Driver.cpp
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <os/log.h>

#include <string>

#include <AudioDriverKit/AudioDriverKit.h>

#include "XoneDB4Driver.h"
#include "AudioDevice.h"

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

static const uint16_t kPCMPacketSize = 512;

static const char8_t sampleratebytes48[3] = { 0x80, 0xBB, 0x00 };
static const char8_t sampleratebytes96[3] = { 0x00, 0x77, 0x01 };

struct XoneDB4Driver_IVars
{
	OSSharedPtr<IODispatchQueue>	m_work_queue;
	OSSharedPtr<AudioDevice>		m_audio_device;

	IOUSBHostDevice					*device;
	IOUSBHostInterface				*interface0;
	IOUSBHostInterface				*interface1;
	IOBufferMemoryDescriptor		*firmwarever;
	IOBufferMemoryDescriptor		*sampleratebytes;
	IOUSBHostPipe					*MIDIinPipe;
	IOUSBHostPipe					*PCMoutPipe;
	IOUSBHostPipe					*PCMinPipe;
	OSAction						*PCMoutCallback;
	OSAction						*PCMinCallback;
	IOBufferMemoryDescriptor		*PCMoutData;
	IOBufferMemoryDescriptor		*PCMinData;
	uint16_t						PCMPacketSize = 512;
};

std::string utf16_to_utf8(const std::u16string& utf16_str) {
	std::string utf8_str;
	
	for (char16_t c : utf16_str) {
		if (c <= 0x7F) {
			utf8_str.push_back(static_cast<char>(c));
		}
		else if (c <= 0x7FF) {
			utf8_str.push_back(static_cast<char>(0xC0 | (c >> 6)));
			utf8_str.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
		else {
			utf8_str.push_back(static_cast<char>(0xE0 | (c >> 12)));
			utf8_str.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
			utf8_str.push_back(static_cast<char>(0x80 | (c & 0x3F)));
		}
	}

	return utf8_str;
}

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

	bool success = false;
	auto device_uid = OSSharedPtr(OSString::withCString("xonedb4"), OSNoRetain);
	auto model_uid = OSSharedPtr(OSString::withCString("Model UID"), OSNoRetain);
	OSSharedPtr<OSString> manufacturer_uid;
	OSSharedPtr<OSString> device_name;
	
	uint16_t bytesTransferred;
	
	ret = Start(provider, SUPERDISPATCH);
	__Require(kIOReturnSuccess == ret, Exit);

	ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
	__Require_Action(NULL != ivars->device, Exit, ret = kIOReturnNoDevice);

	ret = ivars->device->Open(this, 0, NULL);
	__Require(kIOReturnSuccess == ret, Exit);

	manufacturer_uid = OSSharedPtr(OSString::withCString(utf16_to_utf8(reinterpret_cast<const char16_t*>(ivars->device->CopyStringDescriptor(1)->bString)).c_str()), OSNoRetain);
	device_name = OSSharedPtr(OSString::withCString(utf16_to_utf8(reinterpret_cast<const char16_t*>(ivars->device->CopyStringDescriptor(2)->bString)).c_str()), OSNoRetain);

	ret = ivars->firmwarever->Create(kIOMemoryDirectionInOut, 0x0f, 0, &ivars->firmwarever);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->device->DeviceRequest(this, 0xc0, 0x56, 0x00, 0x00, 0x0f, ivars->firmwarever, &bytesTransferred, 0);
	__Require(kIOReturnSuccess == ret, Exit);

	ret = ivars->firmwarever->GetAddressRange(&range);
	__Require(kIOReturnSuccess == ret, Exit);

	os_log(OS_LOG_DEFAULT, "%s FIRMWARE:", __FUNCTION__);
	for (uint16_t i = 0; i < bytesTransferred; i++) {
		os_log(OS_LOG_DEFAULT, "%02x ", reinterpret_cast<const uint8_t *>(range.address)[i]);
	}

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

	ivars->m_audio_device = OSSharedPtr(OSTypeAlloc(AudioDevice), OSNoRetain);
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
