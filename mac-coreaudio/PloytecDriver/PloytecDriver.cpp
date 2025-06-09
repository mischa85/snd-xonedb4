//
//  PloytecDriver.cpp
//  PloytecDriver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecDriver.h"
#include "PloytecDevice.h"

constexpr uint32_t k_zero_time_stamp_period = 2560;

#define PCM_OUT_EP	0x05
#define PCM_IN_EP	0x86
#define MIDI_IN_EP	0x83

static const char8_t sampleratebytes48[3] = { 0x80, 0xBB, 0x00 };
static const char8_t sampleratebytes96[3] = { 0x00, 0x77, 0x01 };

struct PloytecDriver_IVars
{
	OSSharedPtr<IODispatchQueue>	m_work_queue;
	OSSharedPtr<PloytecDevice>		m_audio_device;

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
	
	OSSharedPtr<OSString>			FirmwareVersionBytes;
	OSSharedPtr<OSString>			manufacturer_uid;
	OSSharedPtr<OSString>			device_name;
	char 							*manufacturer_utf8;
	char 							*device_name_utf8;

	OSSharedPtr<IOBufferMemoryDescriptor> 	usbTXBuffer;
	OSSharedPtr<IOMemoryDescriptor>		usbTXBufferSegment[32768];
	OSSharedPtr<IOBufferMemoryDescriptor> 	usbRXBuffer;
	OSSharedPtr<IOMemoryDescriptor>		usbRXBufferSegment[32768];
	
	int32_t					currentsegment;
};

bool PloytecDriver::init()
{
	bool result = false;
	
	result = super::init();
	FailIf(result != true, , Exit, "Failed to init driver");

	ivars = IONewZero(PloytecDriver_IVars, 1);
	FailIfNULL(ivars, result = false, Exit, "Failed to init vars");

Exit:
	return result;
}

kern_return_t IMPL(PloytecDriver, Start)
{
	kern_return_t ret;
	uintptr_t interfaceIterator;
	IOAddressSegment range;
	uint16_t bytesTransferred;
	bool success = false;
	auto device_uid = OSSharedPtr(OSString::withCString("ploytec"), OSNoRetain);
	auto model_uid = OSSharedPtr(OSString::withCString("Model UID"), OSNoRetain);

	ret = Start(provider, SUPERDISPATCH);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to start driver");

	ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
	FailIfNULL(ivars->device, ret = kIOReturnNoDevice, Exit, "No device");

	ret = ivars->device->Open(this, 0, NULL);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to open device");

	int i, j;
	ivars->manufacturer_utf8 = new char[(ivars->device->CopyStringDescriptor(1)->bLength / 2)];
	j = 0;
	for(i = 0; i < (ivars->device->CopyStringDescriptor(1)->bLength / 2); i++) {
		ivars->manufacturer_utf8[i] = ivars->device->CopyStringDescriptor(1)->bString[j];
		j+=2;
	}
	ivars->device_name_utf8 = new char[(ivars->device->CopyStringDescriptor(2)->bLength / 2)];
	j = 0;
	for(i = 0; i < (ivars->device->CopyStringDescriptor(2)->bLength / 2); i++) {
		ivars->device_name_utf8[i] = ivars->device->CopyStringDescriptor(2)->bString[j];
		j+=2;
	}

	ivars->manufacturer_uid = OSSharedPtr(OSString::withCString(ivars->manufacturer_utf8), OSNoRetain);
	ivars->device_name = OSSharedPtr(OSString::withCString(ivars->device_name_utf8), OSNoRetain);

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 0x0f, 0, &ivars->receivebuffer);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to allocate USB receive buffer");
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 32768 * 1928, 0, ivars->usbTXBuffer.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create output ring buffer");
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 32768 * 2048, 0, ivars->usbRXBuffer.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create input ring buffer");
	for (i = 0; i < 32768; i++)
	{
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * 1928, 1928, ivars->usbTXBuffer.get(), ivars->usbTXBufferSegment[i].attach());
		FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create USB output SubMemoryDescriptor");
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * 2048, 2048, ivars->usbRXBuffer.get(), ivars->usbRXBufferSegment[i].attach());
		FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create USB input SubMemoryDescriptor");
	}

	// get firmware
	ret = ivars->device->DeviceRequest(this, 0xc0, 0x56, 0x00, 0x00, 0x0f, ivars->receivebuffer, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware version from device");
	ret = ivars->receivebuffer->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware address bytes");
	ivars->firmwarever = new char[range.length];
	memcpy(ivars->firmwarever, reinterpret_cast<const uint8_t *>(range.address), range.length);

	// get status
	ret = ivars->device->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->receivebuffer, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get status from device");

	// get samplerate
	ret = ivars->sampleratebytes->Create(kIOMemoryDirectionInOut, 0x03, 0, &ivars->sampleratebytes);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to allocate samplerate receive buffer");
	ret = ivars->device->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get current samplerate from device");

	// set 96 khz
	ret = ivars->sampleratebytes->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get samplerate bytes");
	memcpy(reinterpret_cast<void *>(range.address), sampleratebytes96, 3);
	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->device->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");

	// get samplerate
	ret = ivars->device->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get current samplerate from device");

	// get status
	ret = ivars->device->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get status from device");

	// allgood
	ret = ivars->device->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get allgood from device");

	// get the USB pipes
	ret = ivars->device->SetConfiguration(1, false);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set config 1 on USB device");
	ret = ivars->device->CreateInterfaceIterator(&interfaceIterator);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create the interface iterator");
	ret = ivars->device->CopyInterface(interfaceIterator, &ivars->interface0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy interface 0 from iterator");
	ret = ivars->interface0->Open(this, NULL, NULL);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to open interface 0");
	ret = ivars->interface0->SelectAlternateSetting(1);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to select alternate setting on interface 0");
	ret = ivars->device->CopyInterface(interfaceIterator, &ivars->interface1);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy interface 1 from iterator");
	ret = ivars->interface1->Open(this, NULL, NULL);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to open interface 1");
	ret = ivars->interface1->SelectAlternateSetting(1);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to select alternate setting on interface 1");
	ret = ivars->interface0->CopyPipe(MIDI_IN_EP, &ivars->MIDIinPipe);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy the MIDI in pipe");
	ret = ivars->interface1->CopyPipe(PCM_IN_EP, &ivars->PCMinPipe);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy the PCM in pipe");
	ret = ivars->interface0->CopyPipe(PCM_OUT_EP, &ivars->PCMoutPipe);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy the PCM out pipe");

	ret = OSAction::Create(this, PloytecDriver_PCMinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->PCMinCallback);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create the PCM in USB handler");

	ret = OSAction::Create(this, PloytecDriver_PCMoutHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->PCMoutCallback);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create the PCM out USB handler");

	ivars->m_work_queue = GetWorkQueue();
	FailIfNULL(ivars->m_work_queue.get(), ret = kIOReturnInvalid, Exit, "Invalid device");

	ivars->m_audio_device = OSSharedPtr(OSTypeAlloc(PloytecDevice), OSNoRetain);
	FailIfNULL(ivars->m_audio_device.get(), ret = kIOReturnNoMemory, Exit, "Cannot allocate memory for audio device");

	success = ivars->m_audio_device->init(this, false, device_uid.get(), model_uid.get(), ivars->manufacturer_uid.get(), k_zero_time_stamp_period, ivars->PCMinPipe, ivars->PCMinCallback, ivars->PCMoutPipe, ivars->PCMoutCallback, ivars->device, ivars->usbRXBuffer.get(), ivars->usbTXBuffer.get());
	FailIf(false == success, , Exit, "No memory");

	ivars->m_audio_device->SetName(ivars->device_name.get());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set name to audio device");

	AddObject(ivars->m_audio_device.get());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to add object to audio device");

	ret = RegisterService();
	FailIf(ret != kIOReturnSuccess, , Exit, "Cannot register service");
Exit:
	return ret;
}

kern_return_t IMPL(PloytecDriver, Stop)
{
	kern_return_t ret = kIOReturnSuccess;

	os_log(OS_LOG_DEFAULT, "stop ding %s", __FUNCTION__);

	return ret;
}

void PloytecDriver::free()
{
	if (ivars != nullptr)
	{
		ivars->m_work_queue.reset();
		ivars->m_audio_device.reset();
	}
	IOSafeDeleteNULL(ivars, PloytecDriver_IVars, 1);
	super::free();
}

kern_return_t PloytecDriver::NewUserClient_Impl(uint32_t in_type, IOUserClient** out_user_client)
{
	kern_return_t error = kIOReturnSuccess;
	
	if (in_type == kIOUserAudioDriverUserClientType)
	{
		error = super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
		FailIfError(error, , Failure, "Failed to create user client");
		FailIfNULL(*out_user_client, error = kIOReturnNoMemory, Failure, "Failed to create user client");
	}
	else
	{
		IOService* user_client_service = nullptr;
		error = Create(this, "PloytecDriverUserClientProperties", &user_client_service);
		FailIfError(error, , Failure, "failed to create the PloytecDriver user client");
		*out_user_client = OSDynamicCast(IOUserClient, user_client_service);
	}
	
Failure:
	return error;
}

kern_return_t PloytecDriver::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
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

kern_return_t PloytecDriver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
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

OSData* PloytecDriver::GetFirmwareVer()
{
	return OSData::withBytes(ivars->firmwarever, sizeof(&ivars->firmwarever));
}

OSData* PloytecDriver::GetDeviceName()
{
	return OSData::withBytes(ivars->device_name_utf8, strlen(ivars->device_name_utf8));
}

OSData* PloytecDriver::GetDeviceManufacturer()
{
	return OSData::withBytes(ivars->manufacturer_utf8, strlen(ivars->manufacturer_utf8));
}

kern_return_t PloytecDriver::GetPlaybackStats(playbackstats *stats)
{
	return ivars->m_audio_device->GetPlaybackStats(stats);
}

kern_return_t PloytecDriver::ChangeBufferSize(OSNumber *buffersize)
{
	return ivars->m_audio_device->RequestDeviceConfigurationChange(k_change_buffer_size_action, buffersize);
}

kern_return_t IMPL(PloytecDriver, PCMinHandler)
{
	kern_return_t ret;
	ret = ivars->m_audio_device->ReceivePCMfromDevice(completionTimestamp);
	FailIf(ret != kIOReturnSuccess, , Exit, "USB receive error");

Exit:
	return ret;
}

kern_return_t IMPL(PloytecDriver, PCMoutHandler)
{
	kern_return_t ret;
	ret = ivars->m_audio_device->SendPCMToDevice(completionTimestamp);
	FailIf(ret != kIOReturnSuccess, , Exit, "USB send error");
	ret = ivars->PCMoutPipe->AsyncIO(ivars->usbTXBufferSegment[ivars->currentsegment].get(), 1928, ivars->PCMoutCallback, 0);
	ivars->currentsegment++;
	if (ivars->currentsegment == 64)
		ivars->currentsegment = 0;

Exit:
	return ret;
}
