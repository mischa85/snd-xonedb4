//
//  PloytecDriver.cpp
//  PloytecDriver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecDriver.h"
#include "PloytecSharedTypes.iig"

constexpr uint32_t k_zero_time_stamp_period = 2560;

#define PCM_OUT_EP	0x05
#define PCM_IN_EP	0x86
#define MIDI_IN_EP	0x83

static const char8_t sampleratebytes48[3] = { 0x80, 0xBB, 0x00 };
static const char8_t sampleratebytes96[3] = { 0x00, 0x77, 0x01 };

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
	IOUSBStandardEndpointDescriptors indescriptor;
	IOUSBStandardEndpointDescriptors outdescriptor;
	bool success = false;
	auto device_uid = OSSharedPtr(OSString::withCString("ploytec"), OSNoRetain);
	auto model_uid = OSSharedPtr(OSString::withCString("Model UID"), OSNoRetain);

	ret = Start(provider, SUPERDISPATCH);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to start driver");

	ivars->usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
	FailIfNULL(ivars->usbDevice, ret = kIOReturnNoDevice, Exit, "No device");

	ret = ivars->usbDevice->Open(this, 0, NULL);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to open device");

	int i, j;
	ivars->manufacturer_utf8 = new char[(ivars->usbDevice->CopyStringDescriptor(1)->bLength / 2)];
	j = 0;
	for(i = 0; i < (ivars->usbDevice->CopyStringDescriptor(1)->bLength / 2); i++) {
		ivars->manufacturer_utf8[i] = ivars->usbDevice->CopyStringDescriptor(1)->bString[j];
		j+=2;
	}
	ivars->device_name_utf8 = new char[(ivars->usbDevice->CopyStringDescriptor(2)->bLength / 2)];
	j = 0;
	for(i = 0; i < (ivars->usbDevice->CopyStringDescriptor(2)->bLength / 2); i++) {
		ivars->device_name_utf8[i] = ivars->usbDevice->CopyStringDescriptor(2)->bString[j];
		j+=2;
	}

	ivars->manufacturer_uid = OSSharedPtr(OSString::withCString(ivars->manufacturer_utf8), OSNoRetain);
	ivars->device_name = OSSharedPtr(OSString::withCString(ivars->device_name_utf8), OSNoRetain);

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 0x0f, 0, ivars->usbRXBufferCONTROL.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to allocate USB receive buffer");
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 32768 * 1928, 0, ivars->usbTXBufferPCMandUART.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create output ring buffer");
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 32768 * 2048, 0, ivars->usbRXBufferPCM.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create input ring buffer");
	for (i = 0; i < 32768; i++)
	{
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * 1928, 1928, ivars->usbTXBufferPCMandUART.get(), ivars->usbTXBufferPCMandUARTSegment[i].attach());
		FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create USB output SubMemoryDescriptor");
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * 2048, 2048, ivars->usbRXBufferPCM.get(), ivars->usbRXBufferPCMSegment[i].attach());
		FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create USB input SubMemoryDescriptor");
	}
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 32768, 0, ivars->usbRXBufferMIDI.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create input MIDI buffer");
	ret = ivars->usbRXBufferMIDI->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware address bytes");
	ivars->usbRXBufferMIDIAddr = reinterpret_cast<uint8_t *>(range.address);

	// get firmware
	ret = ivars->usbDevice->DeviceRequest(this, 0xc0, 0x56, 0x00, 0x00, 0x0f, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware version from device");
	ret = ivars->usbRXBufferCONTROL->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware address bytes");
	ivars->firmwarever = new char[range.length];
	memcpy(ivars->firmwarever, reinterpret_cast<const uint8_t *>(range.address), range.length);

	// get status
	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get status from device");

	// get samplerate
	ret = ivars->sampleratebytes->Create(kIOMemoryDirectionInOut, 0x03, 0, &ivars->sampleratebytes);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to allocate samplerate receive buffer");
	ret = ivars->usbDevice->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get current samplerate from device");

	// set 96 khz
	ret = ivars->sampleratebytes->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get samplerate bytes");
	memcpy(reinterpret_cast<void *>(range.address), sampleratebytes96, 3);
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set samplerate on device");

	// get samplerate
	ret = ivars->usbDevice->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get current samplerate from device");

	// get status
	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get status from device");

	// allgood
	ret = ivars->usbDevice->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get allgood from device");

	// get the USB pipes
	ret = ivars->usbDevice->SetConfiguration(1, false);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set config 1 on USB device");
	ret = ivars->usbDevice->CreateInterfaceIterator(&interfaceIterator);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create the interface iterator");
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy interface 0 from iterator");
	ret = ivars->usbInterface0->Open(this, NULL, NULL);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to open interface 0");
	ret = ivars->usbInterface0->SelectAlternateSetting(1);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to select alternate setting on interface 0");
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface1);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy interface 1 from iterator");
	ret = ivars->usbInterface1->Open(this, NULL, NULL);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to open interface 1");
	ret = ivars->usbInterface1->SelectAlternateSetting(1);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to select alternate setting on interface 1");
	ret = ivars->usbInterface0->CopyPipe(MIDI_IN_EP, &ivars->usbMIDIinPipe);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy the MIDI in pipe");
	ret = ivars->usbInterface1->CopyPipe(PCM_IN_EP, &ivars->usbPCMinPipe);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy the PCM in pipe");
	ret = ivars->usbInterface0->CopyPipe(PCM_OUT_EP, &ivars->usbPCMoutPipe);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to copy the PCM out pipe");

	ret = ivars->usbPCMinPipe->GetDescriptors(&indescriptor, kIOUSBGetEndpointDescriptorOriginal);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to GetDescriptors from PCM in!");
	ret = ivars->usbPCMoutPipe->GetDescriptors(&outdescriptor, kIOUSBGetEndpointDescriptorOriginal);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to GetDescriptors from PCM out!");
	if (outdescriptor.descriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk)
		ivars->transferMode = BULK;
	else if (outdescriptor.descriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt)
		ivars->transferMode = INTERRUPT;

	ret = OSAction::Create(this, PloytecDriver_PCMinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMinCallback);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create the PCM in USB handler");

	if (ivars->transferMode == BULK)
		ret = OSAction::Create(this, PloytecDriver_PCMoutHandlerBulk_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMoutCallbackBulk);
	else if (ivars->transferMode == INTERRUPT)
		ret = OSAction::Create(this, PloytecDriver_PCMoutHandlerInterrupt_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMoutCallbackInterrupt);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create the PCM out USB handler");
	
	ret = OSAction::Create(this, PloytecDriver_MIDIinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbMIDIinCallback);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create the MIDI in USB handler");

	// send the empty urbs
	for (i = 0; i < 8; i++) {
		if (ivars->transferMode == BULK) {
			ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[0].get(), 2048, ivars->usbPCMoutCallbackBulk, 0);
		} else if (ivars->transferMode == INTERRUPT)
			ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[0].get(), 1928, ivars->usbPCMoutCallbackInterrupt, 0);
		ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBufferPCMSegment[0].get(), 2048, ivars->usbPCMinCallback, 0);
		ret = ivars->usbMIDIinPipe->AsyncIO(ivars->usbRXBufferMIDI.get(), 2048, ivars->usbMIDIinCallback, 0);
	}

	ivars->workQueue = GetWorkQueue();
	FailIfNULL(ivars->workQueue.get(), ret = kIOReturnInvalid, Exit, "Invalid device");

	ivars->audioDevice = OSSharedPtr(OSTypeAlloc(PloytecDevice), OSNoRetain);
	FailIfNULL(ivars->audioDevice.get(), ret = kIOReturnNoMemory, Exit, "Cannot allocate memory for audio device");

	success = ivars->audioDevice->init(this, false, device_uid.get(), model_uid.get(), ivars->manufacturer_uid.get(), k_zero_time_stamp_period, ivars->usbRXBufferPCM.get(), ivars->usbTXBufferPCMandUART.get(), ivars->transferMode);
	FailIf(false == success, , Exit, "No memory");

	ivars->audioDevice->SetName(ivars->device_name.get());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to set name to audio device");

	AddObject(ivars->audioDevice.get());
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
		ivars->workQueue.reset();
		ivars->audioDevice.reset();
	}
	IOSafeDeleteNULL(ivars, PloytecDriver_IVars, 1);
	super::free();
}

kern_return_t PloytecDriver::NewUserClient_Impl(uint32_t in_type, IOUserClient** out_user_client)
{
	kern_return_t error = kIOReturnSuccess;

	ivars->midiClient = OSDynamicCast(PloytecDriverUserClient, *out_user_client);

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
	if (in_object_id != ivars->audioDevice->GetObjectID())
	{
		os_log(OS_LOG_DEFAULT, "AudioDriver::StartDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	__block kern_return_t ret;
	__block int i = 0;
	ivars->workQueue->DispatchSync(^(){
		ret = super::StartDevice(in_object_id, in_flags);
	});
	if (ret == kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "%s", __FUNCTION__);
	}
	return ret;
}

kern_return_t PloytecDriver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
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
		os_log(OS_LOG_DEFAULT, "%s", __FUNCTION__);
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
	return ivars->audioDevice->GetPlaybackStats(stats);
}

kern_return_t IMPL(PloytecDriver, PCMinHandler)
{
	kern_return_t ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBufferPCMSegment[ivars->usbRXBufferPCMCurrentSegment].get(), 2048, ivars->usbPCMinCallback, 0);
	ivars->audioDevice->Capture(ivars->usbRXBufferPCMCurrentSegment, completionTimestamp);
	return ret;
}

kern_return_t IMPL(PloytecDriver, PCMoutHandlerBulk)
{
	ivars->audioDevice->Playback(ivars->usbTXBufferPCMandUARTCurrentSegment, completionTimestamp);
	kern_return_t ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[ivars->usbTXBufferPCMandUARTCurrentSegment].get(), 2048, ivars->usbPCMoutCallbackBulk, 0);
	return ret;
}

kern_return_t IMPL(PloytecDriver, PCMoutHandlerInterrupt)
{
	ivars->audioDevice->Playback(ivars->usbTXBufferPCMandUARTCurrentSegment, completionTimestamp);
	kern_return_t ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[ivars->usbTXBufferPCMandUARTCurrentSegment].get(), 1928, ivars->usbPCMoutCallbackInterrupt, 0);
	return ret;
}

kern_return_t IMPL(PloytecDriver, MIDIinHandler)
{
	os_log(OS_LOG_DEFAULT, "MIDI in????");
	uint64_t msg = ivars->usbRXBufferMIDIAddr[0] | ivars->usbRXBufferMIDIAddr[1] << 8 | ivars->usbRXBufferMIDIAddr[2] << 16;

	if (ivars->midiCount < 255) {
		ivars->midiRingBuffer[ivars->midiWriteIndex] = msg;
		ivars->midiWriteIndex = (ivars->midiWriteIndex + 1) % 255;
		ivars->midiCount++;
	}
	if (ivars->midiClient) {
		ivars->midiClient->postMIDIMessage(msg);  // delegate to user client
	}

	kern_return_t ret = ivars->usbMIDIinPipe->AsyncIO(ivars->usbRXBufferMIDI.get(), 2048, ivars->usbMIDIinCallback, 0);
	return ret;
}
