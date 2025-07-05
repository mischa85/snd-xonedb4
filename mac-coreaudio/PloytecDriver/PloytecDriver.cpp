#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecDriver.h"
#include "PloytecSharedTypes.iig"

constexpr uint32_t k_zero_time_stamp_period = 2560;

#define PCM_OUT_EP	0x05
#define PCM_IN_EP	0x86
#define MIDI_IN_EP	0x83

static const char8_t sampleratebytes48[3] = { 0x80, 0xBB, 0x00 };
static const char8_t sampleratebytes96[3] = { 0x00, 0x77, 0x01 };

constexpr uint32_t buffersize = 1048576;

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
	os_log(OS_LOG_DEFAULT, "Starting PloytecDriver...");

	kern_return_t ret;
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

	os_log(OS_LOG_DEFAULT, "Manufacturer: %s", ivars->manufacturer_utf8);
	os_log(OS_LOG_DEFAULT, "Device Name: %s", ivars->device_name_utf8);

	os_log(OS_LOG_DEFAULT, "Allocating buffers...");

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 0x0f, 0, ivars->usbRXBufferCONTROL.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to allocate USB receive buffer");
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 256, 0, ivars->MIDIBuffer.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to allocate MIDI buffer");
	ret = ivars->MIDIBuffer->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get address range for MIDI buffer");
	ivars->MIDIBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 32768, 0, ivars->usbRXBufferMIDI.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create input MIDI buffer");
	ret = ivars->usbRXBufferMIDI->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware address bytes");
	ivars->usbRXBufferMIDIAddr = reinterpret_cast<uint8_t *>(range.address);

	os_log(OS_LOG_DEFAULT, "Fetching firmware version...");

	// get firmware
	ret = ivars->usbDevice->DeviceRequest(this, 0xc0, 0x56, 0x00, 0x00, 0x0f, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware version from device");
	ret = ivars->usbRXBufferCONTROL->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get firmware address bytes");
	ivars->firmwarever = new char[range.length];
	memcpy(ivars->firmwarever, reinterpret_cast<const uint8_t *>(range.address), range.length);

	os_log(OS_LOG_DEFAULT, "Getting device status...");

	// get status
	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get status from device");

	os_log(OS_LOG_DEFAULT, "Getting samplerate...");

	// get samplerate
	ret = ivars->sampleratebytes->Create(kIOMemoryDirectionInOut, 0x03, 0, &ivars->sampleratebytes);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to allocate samplerate receive buffer");
	ret = ivars->usbDevice->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get current samplerate from device");

	os_log(OS_LOG_DEFAULT, "Setting samplerate to 96 kHz...");

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

	os_log(OS_LOG_DEFAULT, "Getting current samplerate...");

	// get samplerate
	ret = ivars->usbDevice->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get current samplerate from device");

	os_log(OS_LOG_DEFAULT, "Getting device status...");

	// get status
	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get status from device");

	os_log(OS_LOG_DEFAULT, "Sending allgood...");

	// allgood
	ret = ivars->usbDevice->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, ivars->sampleratebytes, &bytesTransferred, 0);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get allgood from device");

	os_log(OS_LOG_DEFAULT, "Get USB pipes...");

	// get the USB pipes
	ret = CreateUSBPipes();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "CreateUSBPipes Fail: %s", strerror(ret));
		return ret;
	}

	os_log(OS_LOG_DEFAULT, "Getting USB descriptors...");

	ret = ivars->usbPCMinPipe->GetDescriptors(&indescriptor, kIOUSBGetEndpointDescriptorOriginal);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to GetDescriptors from PCM in!");
	ret = ivars->usbPCMoutPipe->GetDescriptors(&outdescriptor, kIOUSBGetEndpointDescriptorOriginal);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to GetDescriptors from PCM out!");
	if (outdescriptor.descriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk)
		ivars->transferMode = BULK;
	else if (outdescriptor.descriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt)
		ivars->transferMode = INTERRUPT;
	
	if (ivars->transferMode == BULK)
		ivars->usbMIDIbyteNo = 480;
	else if (ivars->transferMode == INTERRUPT)
		ivars->usbMIDIbyteNo = 432;

	os_log(OS_LOG_DEFAULT, "Transfer mode: %s", ivars->transferMode == BULK ? "BULK" : "INTERRUPT");

	os_log(OS_LOG_DEFAULT, "Allocating ring buffers...");
	
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, buffersize, 0, ivars->usbRXBufferPCM.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create input ring buffer");
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, buffersize, 0, ivars->usbTXBufferPCMandUART.attach());
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create output ring buffer");
	ret = ivars->usbTXBufferPCMandUART->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Exit, "Failed to get output ring buffer address bytes");
	ivars->usbTXBufferPCMandUARTAddr = reinterpret_cast<uint8_t *>(range.address);
	if (ivars->transferMode == BULK)
	{
		for (size_t i = 480; i + 1 < buffersize; i += 512)
		{
			ivars->usbTXBufferPCMandUARTAddr[i] = 0xFD;
			ivars->usbTXBufferPCMandUARTAddr[i + 1] = 0xFD;
		}
	}
	else if (ivars->transferMode == INTERRUPT)
	{
		for (size_t i = 432; i + 1 < buffersize; i += 482)
		{
			ivars->usbTXBufferPCMandUARTAddr[i] = 0xFD;
			ivars->usbTXBufferPCMandUARTAddr[i + 1] = 0xFD;
		}
	}
	ivars->usbCurrentOutputFramesCount = 80;
	if (ivars->transferMode == BULK)
		ivars->usbOutputPacketSize = (ivars->usbCurrentOutputFramesCount / 10) * 512;
	else if (ivars->transferMode == INTERRUPT)
		ivars->usbOutputPacketSize = (ivars->usbCurrentOutputFramesCount / 10) * 482;
	ret = CreateUSBTXBuffersPCMandUART(ivars->usbOutputPacketSize);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to create USB buffers: %s", strerror(ret));
		return ret;
	}
	ivars->usbCurrentInputFramesCount = 80;
	ivars->usbInputPacketSize = (ivars->usbCurrentInputFramesCount / 8) * 512;
	ret = CreateUSBRXBuffersPCM(ivars->usbInputPacketSize);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to create USB buffers: %s", strerror(ret));
		return ret;
	}

	os_log(OS_LOG_DEFAULT, "Creating USB handlers...");

	ret = CreateUSBHandlers();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to create USB handlers: %s", strerror(ret));
		return ret;
	}
	
	ivars->usbCurrentUrbCount = 2;
	//SetCurrentUrbCount(4);
	ret = SendPCMUrbs(ivars->usbCurrentUrbCount);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to send PCM URBs: %s", strerror(ret));
		return ret;
	}
	ret = SendMIDIUrbs(1);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to send MIDI URBs: %s", strerror(ret));
		return ret;
	}

	ivars->workQueue = GetWorkQueue();
	FailIfNULL(ivars->workQueue.get(), ret = kIOReturnInvalid, Exit, "Invalid device");

	os_log(OS_LOG_DEFAULT, "Creating audio device...");

	ivars->audioDevice = OSSharedPtr(OSTypeAlloc(PloytecDevice), OSNoRetain);
	FailIfNULL(ivars->audioDevice.get(), ret = kIOReturnNoMemory, Exit, "Cannot allocate memory for audio device");

	os_log(OS_LOG_DEFAULT, "Initializing audio device...");

	success = ivars->audioDevice->init(this, false, device_uid.get(), model_uid.get(), ivars->manufacturer_uid.get(), k_zero_time_stamp_period, ivars->usbRXBufferPCM.get(), ivars->usbTXBufferPCMandUART.get(), ivars->transferMode);
	FailIf(false == success, , Exit, "No memory");
	
	ivars->audioDevice->SetDispatchQueue(ivars->workQueue.get());

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
	stats->usbPCMoutFramesCount = ivars->usbPCMoutFramesCount;
	stats->usbPCMinFramesCount = ivars->usbPCMinFramesCount;
	stats->usbMIDIoutBytesCount = ivars->usbMIDIoutBytesCount;
	stats->usbMIDIinBytesCount = ivars->usbMIDIinBytesCount;
	return ivars->audioDevice->GetPlaybackStats(stats);
}

void PloytecDriver::WriteMIDIBytes(const uint64_t msg) {
	uint8_t length = msg & 0xFF;
	if (length == 0 || length > 6) {
		os_log(OS_LOG_DEFAULT, "WriteMIDIBytes: Invalid length: %u", length);
		return;
	}

	for (uint8_t i = 0; i < length; ++i) {
		uint8_t byte = static_cast<uint8_t>((msg >> (8 * (i + 1))) & 0xFF);

		uint32_t next = (ivars->midiRingHead + 1) % 256;
		if (next == ivars->midiRingTail) {
			os_log(OS_LOG_DEFAULT, "WriteMIDIBytes: MIDI ring buffer overflow");
			break;
		}

		ivars->MIDIBufferAddr[ivars->midiRingHead] = byte;
		ivars->midiRingHead = next;
	}
}

bool PloytecDriver::ReadMIDIByte(uint8_t &outByte) {
	if (ivars->midiRingHead == ivars->midiRingTail) return false; // Buffer empty
	outByte = ivars->MIDIBufferAddr[ivars->midiRingTail];
	ivars->midiRingTail = (ivars->midiRingTail + 1) % 256;
	return true;
}

kern_return_t PloytecDriver::SendPCMUrbs(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);

	kern_return_t ret = kIOReturnSuccess;
	
	ivars->usbCurrentUrbCount = num;

	ivars->usbShutdownInProgress = false;

	for (int i = 0; i < num; i++) {
		ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBufferPCMSegment[0].get(), ivars->usbInputPacketSize, ivars->usbPCMinCallback, 0);
		if (ret != kIOReturnSuccess)
			return ret;
		ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[0].get(), ivars->usbOutputPacketSize, ivars->usbPCMoutCallback, 0);
		if (ret != kIOReturnSuccess)
			return ret;
	}
	return ret;
}

kern_return_t PloytecDriver::SendMIDIUrbs(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);

	kern_return_t ret = kIOReturnSuccess;

	ivars->usbShutdownInProgress = false;

	ret = ivars->usbMIDIinPipe->AsyncIO(ivars->usbRXBufferMIDI.get(), 512, ivars->usbMIDIinCallback, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to send MIDI URB: %s", strerror(ret));
		return ret;
	}
	return ret;
}

/* This needs to be called in async context! */
void PloytecDriver::AbortUSBUrbs(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "AbortUSBUrbs: start");
	
	kern_return_t ret = kIOReturnSuccess;

	ivars->usbShutdownInProgress = true;

	for (int timeout = 0; timeout < 1000; ++timeout)
	{
		if ((ivars->usbPCMoutAbortedCount >= num) && (ivars->usbPCMinAbortedCount >= num))
			break;
		IOSleep(1);
	}

	ret = ivars->usbDevice->SetConfiguration(0, true);
	if (ret != kIOReturnSuccess)
		os_log(OS_LOG_DEFAULT, "SetConfiguration failed: %s", strerror(ret));

	IOSleep(10);

	ret = CreateUSBPipes();
	if (ret != kIOReturnSuccess)
		os_log(OS_LOG_DEFAULT, "CreateUSBPipes failed: %s", strerror(ret));
	
	ret = ivars->usbDevice->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, nullptr, nullptr, 0);
	if (ret != kIOReturnSuccess)
		os_log(OS_LOG_DEFAULT, "Failed to flush: %s", strerror(ret));
	
	ret = SendMIDIUrbs(1);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to send MIDI URBs: %s", strerror(ret));
		return ret;
	}

	ivars->usbPCMoutAbortedCount = 0;
	ivars->usbPCMinAbortedCount = 0;
	ivars->usbShutdownInProgress = false;

	os_log(OS_LOG_DEFAULT, "AbortUSBUrbs: done");
}

kern_return_t PloytecDriver::CreateUSBTXBuffersPCMandUART(uint32_t outputPacketSize)
{
	kern_return_t ret = kIOReturnSuccess;
	
	ivars->usbOutputPacketSize = outputPacketSize;

	for (int i = 0; i < (buffersize / outputPacketSize); i++)
	{
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * outputPacketSize, outputPacketSize, ivars->usbTXBufferPCMandUART.get(), ivars->usbTXBufferPCMandUARTSegment[i].attach());
		FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create USB output SubMemoryDescriptor");
		ivars->usbTXBufferPCMandUARTSegmentAddr[i] = ivars->usbTXBufferPCMandUARTAddr + (i * outputPacketSize);
	}
Exit:
	return ret;
}

kern_return_t PloytecDriver::CreateUSBRXBuffersPCM(uint32_t inputPacketSize)
{
	kern_return_t ret = kIOReturnSuccess;

	ivars->usbInputPacketSize = inputPacketSize;

	for (int i = 0; i < (buffersize / ivars->usbInputPacketSize); i++)
	{
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * ivars->usbInputPacketSize, ivars->usbInputPacketSize, ivars->usbRXBufferPCM.get(), ivars->usbRXBufferPCMSegment[i].attach());
		FailIf(ret != kIOReturnSuccess, , Exit, "Failed to create USB input SubMemoryDescriptor");
	}
Exit:
	return ret;
}

kern_return_t PloytecDriver::CreateUSBHandlers()
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);
	
	kern_return_t ret = kIOReturnSuccess;

	ret = OSAction::Create(this, PloytecDriver_PCMinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMinCallback);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = OSAction::Create(this, PloytecDriver_PCMoutHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMoutCallback);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = OSAction::Create(this, PloytecDriver_MIDIinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbMIDIinCallback);
	if (ret != kIOReturnSuccess)
		return ret;

	return ret;
}

kern_return_t PloytecDriver::CreateUSBPipes()
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);
	
	kern_return_t ret = kIOReturnSuccess;
	
	uintptr_t interfaceIterator;
	
	ret = ivars->usbDevice->SetConfiguration(1, false);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbDevice->CreateInterfaceIterator(&interfaceIterator);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface0);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface1);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbDevice->DestroyInterfaceIterator(interfaceIterator);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbInterface0->Open(this, NULL, NULL);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbInterface0->SelectAlternateSetting(1);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbInterface0->CopyPipe(MIDI_IN_EP, &ivars->usbMIDIinPipe);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbInterface0->CopyPipe(PCM_OUT_EP, &ivars->usbPCMoutPipe);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbInterface1->Open(this, NULL, NULL);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbInterface1->SelectAlternateSetting(1);
	if (ret != kIOReturnSuccess)
		return ret;
	ret = ivars->usbInterface1->CopyPipe(PCM_IN_EP, &ivars->usbPCMinPipe);
	if (ret != kIOReturnSuccess)
		return ret;

	return ret;
}

uint8_t PloytecDriver::GetCurrentUrbCount()
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);

	return ivars->usbCurrentUrbCount;
}

void PloytecDriver::SetCurrentUrbCount(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);
	
	__block kern_return_t ret = kIOReturnSuccess;
	
	ivars->workQueue->DispatchAsync(^(){
		AbortUSBUrbs(ivars->usbCurrentUrbCount);
		ret = SendPCMUrbs(num);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "HELP: %s", __func__);
			return;
		}
	});
}

uint16_t PloytecDriver::GetCurrentInputFramesCount()
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);

	return ivars->usbCurrentInputFramesCount;
}

void PloytecDriver::SetFrameCount(uint16_t inputCount, uint16_t outputCount)
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);
	os_log(OS_LOG_DEFAULT, "%s: CHANGING FRAMECOUNT TO: INPUT %d OUTPUT %d", __func__, inputCount, outputCount);

	__block kern_return_t ret = kIOReturnSuccess;

	ivars->workQueue->DispatchAsync(^(){
		AbortUSBUrbs(ivars->usbCurrentUrbCount);
		ivars->usbCurrentOutputFramesCount = outputCount;
		ivars->usbCurrentInputFramesCount = inputCount;
		if (ivars->transferMode == BULK)
			ivars->usbOutputPacketSize = (outputCount / 10) * 512;
		else if (ivars->transferMode == INTERRUPT)
			ivars->usbOutputPacketSize = (outputCount / 10) * 482;
		ivars->usbInputPacketSize = (inputCount / 8) * 512;
		ret = CreateUSBTXBuffersPCMandUART(ivars->usbOutputPacketSize);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "HELP: %s", __func__);
			return;
		}
		ret = CreateUSBRXBuffersPCM(ivars->usbInputPacketSize);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "HELP: %s", __func__);
			return;
		}
		ret = SendPCMUrbs(ivars->usbCurrentUrbCount);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "HELP: %s", __func__);
			return;
		}
	});
}

uint16_t PloytecDriver::GetCurrentOutputFramesCount()
{
	os_log(OS_LOG_DEFAULT, "DRIVER: %s", __func__);

	return ivars->usbCurrentOutputFramesCount;
}

kern_return_t IMPL(PloytecDriver, PCMinHandler)
{
	ivars->workQueue->DispatchAsync(^{
		if (ivars->usbShutdownInProgress)
		{
			os_log(OS_LOG_DEFAULT, "Callback aborted: %s", __func__);
			ivars->usbPCMinPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, NULL);
			ivars->usbPCMinAbortedCount++;
			return;
		}
		
		ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBufferPCMSegment[ivars->usbRXBufferPCMCurrentSegment].get(), ivars->usbInputPacketSize, ivars->usbPCMinCallback, 0);
		ivars->audioDevice->Capture(ivars->usbRXBufferPCMCurrentSegment, ivars->usbCurrentInputFramesCount, completionTimestamp);
		ivars->usbPCMinFramesCount += ivars->usbCurrentInputFramesCount;
	});

	return kIOReturnSuccess;
}

kern_return_t IMPL(PloytecDriver, PCMoutHandler)
{
	ivars->workQueue->DispatchAsync(^{
		if (ivars->usbShutdownInProgress)
		{
			os_log(OS_LOG_DEFAULT, "Callback aborted: %s", __func__);
			ivars->usbPCMoutPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, NULL);
			ivars->usbPCMoutAbortedCount++;
			return;
		}
		
		ivars->audioDevice->Playback(ivars->usbTXBufferPCMandUARTCurrentSegment, ivars->usbCurrentOutputFramesCount, completionTimestamp);
		uint8_t byte;
		if (ReadMIDIByte(byte))
		{
			ivars->usbTXBufferPCMandUARTSegmentAddr[ivars->usbTXBufferPCMandUARTCurrentSegment][ivars->usbMIDIbyteNo] = byte;
			ivars->usbMIDIoutBytesCount++;
		}
		else
		{
			ivars->usbTXBufferPCMandUARTSegmentAddr[ivars->usbTXBufferPCMandUARTCurrentSegment][ivars->usbMIDIbyteNo] = 0xFD;
		}
		ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[ivars->usbTXBufferPCMandUARTCurrentSegment].get(), ivars->usbOutputPacketSize, ivars->usbPCMoutCallback, 0);
		ivars->usbPCMoutFramesCount += ivars->usbCurrentOutputFramesCount;
	});

	return kIOReturnSuccess;
}

kern_return_t IMPL(PloytecDriver, MIDIinHandler)
{
	uint8_t expectedLen = 0;

	for (uint32_t i = 0; i < actualByteCount; ++i)
	{
		uint8_t byte = ivars->usbRXBufferMIDIAddr[i];

		if (byte == 0xFD || byte == 0xFF)
			continue;

		if (byte >= 0xF8) {
			uint64_t msg = 0x01 | ((uint64_t)byte << 8);
			if (ivars->midiClient)
				ivars->midiClient->postMIDIMessage(msg);
			continue;
		}

		if (byte & 0x80) {
			ivars->midiParserRunningStatus = byte;
			ivars->midiParserBytes[0] = byte;
			ivars->midiParserIndex = 1;
		} else if (ivars->midiParserRunningStatus) {
			if (ivars->midiParserIndex < 3)
				ivars->midiParserBytes[ivars->midiParserIndex++] = byte;
		}

		switch (ivars->midiParserRunningStatus & 0xF0) {
			case 0xC0:
			case 0xD0: expectedLen = 2; break;
			case 0x80:
			case 0x90:
			case 0xA0:
			case 0xB0:
			case 0xE0: expectedLen = 3; break;
			default: expectedLen = 3; break;
		}

		if (ivars->midiParserIndex == expectedLen)
		{
			uint64_t msg = expectedLen | ((uint64_t)ivars->midiParserBytes[0] << 8) | ((uint64_t)ivars->midiParserBytes[1] << 16) | ((uint64_t)ivars->midiParserBytes[2] << 24);
			if (ivars->midiClient)
				ivars->midiClient->postMIDIMessage(msg);

			ivars->midiParserIndex = 1;
		}
	}
	ivars->usbMIDIinBytesCount += actualByteCount;
	return ivars->usbMIDIinPipe->AsyncIO(ivars->usbRXBufferMIDI.get(), 512, ivars->usbMIDIinCallback, 0);
}
