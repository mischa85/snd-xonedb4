#include <USBDriverKit/USBDriverKit.h>
#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecDriver.h"
#include "PloytecSharedTypes.iig"

static constexpr uint8_t PCM_OUT_EP = 0x05;
static constexpr uint8_t PCM_IN_EP = 0x86;
static constexpr uint8_t MIDI_IN_EP = 0x83;
static constexpr uint32_t K_ZERO_TIME_STAMP_PERIOD = 2560;
static constexpr uint32_t SAMPLE_RATE_44100 = 44100;
static constexpr uint32_t SAMPLE_RATE_48000 = 48000;
static constexpr uint32_t SAMPLE_RATE_88200 = 88200;
static constexpr uint32_t SAMPLE_RATE_96000 = 96000;
static constexpr uint32_t BUFFER_SIZE = 1048576;

bool PloytecDriver::init()
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::init: Initializing PloytecDriver...");

	bool success = false;

	success = super::init();
	if (!success) { os_log(OS_LOG_DEFAULT, "PloytecDriver::init: failed to init driver"); return false; }

	ivars = IONewZero(PloytecDriver_IVars, 1);
	if (!ivars) { os_log(OS_LOG_DEFAULT, "PloytecDriver::init: failed to init vars"); return false; }

	return true;
}

kern_return_t IMPL(PloytecDriver, Start)
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Starting PloytecDriver...");

	bool success = false;
	kern_return_t ret = kIOReturnSuccess;
	uint16_t bytesTransferred;
	uint32_t sampleRate;

	ret = Start(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to start driver: %s", strerror(ret)); return ret; }

	ivars->usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
	if (!ivars->usbDevice) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: no IOUSBHostDevice provider"); return kIOReturnBadArgument; }

	ret = ivars->usbDevice->Open(this, 0, NULL);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to open device: %s", strerror(ret)); return ret; }

	ivars->manufacturerName = MakeStringFromDescriptor(ivars->usbDevice, 1);
	ivars->deviceName = MakeStringFromDescriptor(ivars->usbDevice, 2);
	ivars->modelName = OSSharedPtr(OSString::withCString("Model UID"), OSNoRetain);

	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Manufacturer: %s", ivars->manufacturerName.get()->getCStringNoCopy());
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Device Name: %s", ivars->deviceName.get()->getCStringNoCopy());

	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Allocating buffers...");

	ret = CreateBuffers();
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to create buffers: %s", strerror(ret)); return ret; }

	// Allocate
	ivars->firmwareVersion = IONewZero(FirmwareVersion, 1);
	if (!ivars->firmwareVersion) {
	    os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to alloc firmwareVersion");
	    return kIOReturnNoMemory;
	}

	// get firmware
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Fetching firmware version...");
	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x56, 0x00, 0x00, 0x0f, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to get firmware version from device: %s", strerror(ret)); return ret; }
	ivars->firmwareVersion->major = 1;
	ivars->firmwareVersion->minor = ivars->usbRXBufferCONTROLAddr[2] / 10;
	ivars->firmwareVersion->patch = ivars->usbRXBufferCONTROLAddr[2] % 10;
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Firmware version: %d.%d.%d", ivars->firmwareVersion->major, ivars->firmwareVersion->minor, ivars->firmwareVersion->patch);

	// get status
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Getting device status...");
	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to get status from device: %s", strerror(ret)); return ret; }

	// get samplerate
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Getting current samplerate...");
	ret = ivars->usbDevice->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to get current samplerate from device: %s", strerror(ret)); return ret; }
	if (bytesTransferred < 3) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: samplerate: short read (%u bytes)", (unsigned)bytesTransferred); return kIOReturnUnderrun; }
	sampleRate = ((uint32_t)ivars->usbRXBufferCONTROLAddr[0]) | ((uint32_t)ivars->usbRXBufferCONTROLAddr[1] << 8) | ((uint32_t)ivars->usbRXBufferCONTROLAddr[2] << 16);
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Current samplerate: %u", sampleRate);

	// set 96 khz
	do { uint32_t sr = SAMPLE_RATE_96000; uint8_t* p = ivars->usbTXBufferCONTROLAddr; p[0] = (uint8_t)(sr & 0xFF); p[1] = (uint8_t)((sr >> 8) & 0xFF); p[2] = (uint8_t)((sr >> 16) & 0xFF); } while (0);

	// set samplerate
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Setting samplerate to 96 kHz...");
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->usbTXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to set samplerate on device: %s", strerror(ret)); return ret; }
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->usbTXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to set samplerate on device: %s", strerror(ret)); return ret; }
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->usbTXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to set samplerate on device: %s", strerror(ret)); return ret; }
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->usbTXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to set samplerate on device: %s", strerror(ret)); return ret; }
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->usbTXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to set samplerate on device: %s", strerror(ret)); return ret; }

	// get samplerate
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Getting current samplerate...");
	ret = ivars->usbDevice->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to get current samplerate from device: %s", strerror(ret)); return ret; }
	if (bytesTransferred < 3) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: samplerate: short read (%u bytes)", (unsigned)bytesTransferred); return kIOReturnUnderrun; }
	sampleRate = ((uint32_t)ivars->usbRXBufferCONTROLAddr[0]) | ((uint32_t)ivars->usbRXBufferCONTROLAddr[1] << 8) | ((uint32_t)ivars->usbRXBufferCONTROLAddr[2] << 16);
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Current samplerate: %u", sampleRate);

	// get status
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Getting device status...");
	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to get status from device: %s", strerror(ret)); return ret; }

	// allgood
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Sending allgood...");
	ret = ivars->usbDevice->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, ivars->usbRXBufferCONTROL.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to get allgood from device: %s", strerror(ret)); return ret; }

	// get the USB pipes
	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Get USB pipes...");
	ret = CreateUSBPipes();
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to create USB pipes: %s", strerror(ret)); return ret; }

	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Getting USB descriptors...");

	IOUSBStandardEndpointDescriptors inDescriptor;
	IOUSBStandardEndpointDescriptors outDescriptor;

	ret = ivars->usbPCMinPipe->GetDescriptors(&inDescriptor, kIOUSBGetEndpointDescriptorOriginal);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to GetDescriptors from PCM in: %s", strerror(ret)); return ret; }
	ret = ivars->usbPCMoutPipe->GetDescriptors(&outDescriptor, kIOUSBGetEndpointDescriptorOriginal);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to GetDescriptors from PCM out: %s", strerror(ret)); return ret; }

	ivars->usbCurrentOutputFramesCount = 80;
	ivars->usbCurrentInputFramesCount = 80;

	if (outDescriptor.descriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk)
		ivars->transferMode = BULK;
	else if (outDescriptor.descriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt)
		ivars->transferMode = INTERRUPT;
	
	if (ivars->transferMode == BULK)
		ivars->usbMIDIbyteNo = 480;
	else if (ivars->transferMode == INTERRUPT)
		ivars->usbMIDIbyteNo = 432;

	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Transfer mode: %s", ivars->transferMode == BULK ? "BULK" : "INTERRUPT");

	if (ivars->transferMode == BULK)
	{
		for (size_t i = 480; i + 1 < BUFFER_SIZE; i += 512)
		{
			ivars->usbTXBufferPCMandUARTAddr[i] = 0xFD;
			ivars->usbTXBufferPCMandUARTAddr[i + 1] = 0xFD;
		}
		ivars->usbOutputPacketSize = (ivars->usbCurrentOutputFramesCount / 10) * 512;
	}
	else if (ivars->transferMode == INTERRUPT)
	{
		for (size_t i = 432; i + 1 < BUFFER_SIZE; i += 482)
		{
			ivars->usbTXBufferPCMandUARTAddr[i] = 0xFD;
			ivars->usbTXBufferPCMandUARTAddr[i + 1] = 0xFD;
		}
		ivars->usbOutputPacketSize = (ivars->usbCurrentOutputFramesCount / 10) * 482;
	}

	ret = CreateUSBTXBuffersPCMandUART(ivars->usbOutputPacketSize);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to create USB buffers: %s", strerror(ret)); return ret; }
	ivars->usbInputPacketSize = (ivars->usbCurrentInputFramesCount / 8) * 512;
	ret = CreateUSBRXBuffersPCM(ivars->usbInputPacketSize);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to create USB buffers: %s", strerror(ret)); return ret; }

	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Creating USB handlers...");

	ret = CreateUSBHandlers();
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to create USB handlers: %s", strerror(ret)); return ret; }

	ivars->usbCurrentUrbCount = 2;
	ret = SendPCMUrbs(ivars->usbCurrentUrbCount);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to send PCM URBs: %s", strerror(ret)); return ret; }
	ret = SendMIDIUrbs(1);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to send MIDI URBs: %s", strerror(ret)); return ret; }

	ivars->workQueue = GetWorkQueue();
	if (!ivars->workQueue) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to get workqueue"); return kIOReturnInvalid; }

	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Creating audio device...");

	ivars->audioDevice = OSSharedPtr(OSTypeAlloc(PloytecDevice), OSNoRetain);
	if (!ivars->audioDevice) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: cannot allocate memory for audio device"); return kIOReturnNoMemory; }

	os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: Initializing audio device...");

	success = ivars->audioDevice->init(this, false, ivars->deviceName.get(), ivars->modelName.get(), ivars->manufacturerName.get(), K_ZERO_TIME_STAMP_PERIOD, ivars->usbRXBufferPCM.get(), ivars->usbTXBufferPCMandUART.get(), ivars->transferMode);
	if (!success) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: No memory"); return kIOReturnNoMemory; }

	ivars->audioDevice->SetDispatchQueue(ivars->workQueue.get());

	ret = ivars->audioDevice->SetName(ivars->deviceName.get());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to set name on audio device: %s", strerror(ret)); return ret; }

	ret = AddObject(ivars->audioDevice.get());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to add object to audio device: %s", strerror(ret)); return ret; }

	ret = RegisterService();
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::Start: failed to register service: %s", strerror(ret)); return ret; }

	return ret;
}

kern_return_t IMPL(PloytecDriver, Stop)
{
	if (ivars && ivars->usbPCMoutPipe) (void)ivars->usbPCMoutPipe->Abort(0, kIOReturnAborted, this);
	if (ivars && ivars->usbPCMinPipe)  (void)ivars->usbPCMinPipe->Abort(0, kIOReturnAborted, this);
	if (ivars && ivars->usbMIDIinPipe) (void)ivars->usbMIDIinPipe->Abort(0, kIOReturnAborted, this);
	if (ivars && ivars->usbInterface0) { (void)ivars->usbInterface0->Close(this, 0); ivars->usbInterface0 = nullptr; }
	if (ivars && ivars->usbInterface1) { (void)ivars->usbInterface1->Close(this, 0); ivars->usbInterface1 = nullptr; }
	if (ivars) {
		ivars->usbTXBufferCONTROLAddr = nullptr;
		ivars->usbRXBufferCONTROLAddr = nullptr;
	}

	kern_return_t ret = super::Stop(provider, SUPERDISPATCH);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "PloytecDriver::Stop: super::Stop failed: %s", strerror(ret));
	}

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
	kern_return_t ret = kIOReturnSuccess;

	ivars->midiClient = OSDynamicCast(PloytecDriverUserClient, *out_user_client);
	//if (!ivars->midiClient) { os_log(OS_LOG_DEFAULT, "PloytecDriver::NewUserClient_Impl: failed to create MIDI user client"); return kIOReturnNoMemory; }

	if (in_type == kIOUserAudioDriverUserClientType)
	{
		ret = super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::NewUserClient_Impl: failed to create user client: %s", strerror(ret)); return ret; }
		if (!*out_user_client) { os_log(OS_LOG_DEFAULT, "PloytecDriver::NewUserClient_Impl: failed to create user client"); return kIOReturnNoMemory; }
	}
	else
	{
		IOService* user_client_service = nullptr;
		ret = Create(this, "PloytecDriverUserClientProperties", &user_client_service);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::NewUserClient_Impl: failed to create the PloytecDriver user client: %s", strerror(ret)); return ret; }
		*out_user_client = OSDynamicCast(IOUserClient, user_client_service);
		if (!*out_user_client) { os_log(OS_LOG_DEFAULT, "PloytecDriver::NewUserClient_Impl: failed to create user client"); return kIOReturnNoMemory; }
	}

	return ret;
}

kern_return_t PloytecDriver::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->audioDevice->GetObjectID()) { os_log(OS_LOG_DEFAULT, "PloytecDriver::StartDevice: AudioDriver::StartDevice - unknown object id %u", in_object_id); return kIOReturnBadArgument; }

	__block kern_return_t ret = kIOReturnError;

	ivars->workQueue->DispatchSync(^{
		ret = super::StartDevice(in_object_id, in_flags);
	});

	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::StartDevice: failed to start device: %s", strerror(ret)); }

	return ret;
}

kern_return_t PloytecDriver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	if (in_object_id != ivars->audioDevice->GetObjectID()) { os_log(OS_LOG_DEFAULT, "PloytecDriver::StopDevice: AudioDriver::StopDevice - unknown object id %u", in_object_id); return kIOReturnBadArgument; }

	__block kern_return_t ret = kIOReturnError;

	ivars->workQueue->DispatchSync(^(){
		ret = super::StopDevice(in_object_id, in_flags);
	});

	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::StopDevice: failed to stop device: %s", strerror(ret)); }

	return ret;
}

FirmwareVersion* PloytecDriver::GetFirmwareVer()
{
	return ivars->firmwareVersion;
}

OSSharedPtr<OSString> PloytecDriver::GetDeviceName()
{
	return ivars->deviceName;
}

OSSharedPtr<OSString> PloytecDriver::GetDeviceManufacturer()
{
	return ivars->manufacturerName;
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
	if (length == 0 || length > 6) { os_log(OS_LOG_DEFAULT, "PloytecDriver::WriteMIDIBytes: WriteMIDIBytes: Invalid length: %u", length); return; }

	for (uint8_t i = 0; i < length; ++i) {
		uint8_t byte = static_cast<uint8_t>((msg >> (8 * (i + 1))) & 0xFF);

		uint32_t next = (ivars->midiRingHead + 1) % 256;
		if (next == ivars->midiRingTail) { os_log(OS_LOG_DEFAULT, "PloytecDriver::WriteMIDIBytes: WriteMIDIBytes: MIDI ring buffer overflow"); break; }

		ivars->MIDIBufferAddr[ivars->midiRingHead] = byte;
		ivars->midiRingHead = next;
	}
}

bool PloytecDriver::ReadMIDIByte(uint8_t &outByte)
{
	if (ivars->midiRingHead == ivars->midiRingTail) return false;

	outByte = ivars->MIDIBufferAddr[ivars->midiRingTail];
	ivars->midiRingTail = (ivars->midiRingTail + 1) % 256;
	return true;
}

kern_return_t PloytecDriver::SendPCMUrbs(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::SendPCMUrbs: DRIVER: %s", __func__);

	kern_return_t ret = kIOReturnSuccess;

	ivars->usbCurrentUrbCount = num;

	ivars->usbShutdownInProgress = false;

	for (int i = 0; i < num; i++) {
		ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBufferPCMSegment[0].get(), ivars->usbInputPacketSize, ivars->usbPCMinCallback, 0);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::SendPCMUrbs: WriteMIDIBytes: usbPCMinPipe send error: %s", strerror(ret)); return ret; }
		ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[0].get(), ivars->usbOutputPacketSize, ivars->usbPCMoutCallback, 0);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::SendPCMUrbs: WriteMIDIBytes: usbPCMoutPipe send error: %s", strerror(ret)); return ret; }
	}

	return ret;
}

kern_return_t PloytecDriver::SendMIDIUrbs(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::SendMIDIUrbs: DRIVER: %s", __func__);

	kern_return_t ret = kIOReturnSuccess;

	ivars->usbShutdownInProgress = false;

	ret = ivars->usbMIDIinPipe->AsyncIO(ivars->usbRXBufferMIDI.get(), 512, ivars->usbMIDIinCallback, 0);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::SendMIDIUrbs: failed to send MIDI URB: %s", strerror(ret)); return ret; }

	return ret;
}

/* This needs to be called in async context! */
kern_return_t PloytecDriver::AbortUSBUrbs(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::AbortUSBUrbs: AbortUSBUrbs: start");

	kern_return_t ret = kIOReturnSuccess;

	ivars->usbShutdownInProgress = true;

	for (int timeout = 0; timeout < 1000; ++timeout)
	{
		if ((ivars->usbPCMoutAbortedCount >= num) && (ivars->usbPCMinAbortedCount >= num)) break;
		IOSleep(1);
	}

	ret = ivars->usbDevice->SetConfiguration(0, true);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::AbortUSBUrbs: failed to SetConfiguration: %s", strerror(ret)); return ret; }

	IOSleep(10);

	ret = CreateUSBPipes();
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::AbortUSBUrbs: failed to create USB pipes: %s", strerror(ret)); return ret; }

	ret = ivars->usbDevice->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, nullptr, nullptr, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "PloytecDriver::AbortUSBUrbs: failed to flush: %s", strerror(ret));
		return ret;
	}

	ret = SendMIDIUrbs(1);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "PloytecDriver::AbortUSBUrbs: failed to send MIDI URBs: %s", strerror(ret));
		return ret;
	}

	ivars->usbPCMoutAbortedCount = 0;
	ivars->usbPCMinAbortedCount = 0;
	ivars->usbShutdownInProgress = false;

	os_log(OS_LOG_DEFAULT, "PloytecDriver::AbortUSBUrbs: AbortUSBUrbs: done");

	return ret;
}

kern_return_t PloytecDriver::CreateUSBTXBuffersPCMandUART(uint32_t outputPacketSize)
{
	kern_return_t ret = kIOReturnSuccess;

	ivars->usbOutputPacketSize = outputPacketSize;

	for (int i = 0; i < (BUFFER_SIZE / outputPacketSize); i++)
	{
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * outputPacketSize, outputPacketSize, ivars->usbTXBufferPCMandUART.get(), ivars->usbTXBufferPCMandUARTSegment[i].attach());
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "PloytecDriver::CreateUSBTXBuffersPCMandUART: failed to create USB output SubMemoryDescriptor: %s", strerror(ret));
			return ret;
		}
		ivars->usbTXBufferPCMandUARTSegmentAddr[i] = ivars->usbTXBufferPCMandUARTAddr + (i * outputPacketSize);
	}

	return ret;
}

kern_return_t PloytecDriver::CreateUSBRXBuffersPCM(uint32_t inputPacketSize)
{
	kern_return_t ret = kIOReturnSuccess;

	ivars->usbInputPacketSize = inputPacketSize;

	for (int i = 0; i < (BUFFER_SIZE / ivars->usbInputPacketSize); i++)
	{
		ret = IOMemoryDescriptor::CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * ivars->usbInputPacketSize, ivars->usbInputPacketSize, ivars->usbRXBufferPCM.get(), ivars->usbRXBufferPCMSegment[i].attach());
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::CreateUSBRXBuffersPCM: failed to create USB input SubMemoryDescriptor: %s", strerror(ret)); return ret; }
	}

	return ret;
}

kern_return_t PloytecDriver::CreateUSBHandlers()
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::CreateUSBHandlers: DRIVER: %s", __func__);
	
	kern_return_t ret = kIOReturnSuccess;

	ret = OSAction::Create(this, PloytecDriver_PCMinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMinCallback);
	if (ret != kIOReturnSuccess) return ret;
	ret = OSAction::Create(this, PloytecDriver_PCMoutHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMoutCallback);
	if (ret != kIOReturnSuccess) return ret;
	ret = OSAction::Create(this, PloytecDriver_MIDIinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbMIDIinCallback);
	if (ret != kIOReturnSuccess) return ret;

	return ret;
}

kern_return_t PloytecDriver::CreateUSBPipes()
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::CreateUSBPipes: DRIVER: %s", __func__);

	kern_return_t ret = kIOReturnSuccess;
	uintptr_t interfaceIterator;

	ret = ivars->usbDevice->SetConfiguration(1, false);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbDevice->CreateInterfaceIterator(&interfaceIterator);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface0);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface1);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbDevice->DestroyInterfaceIterator(interfaceIterator);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbInterface0->Open(this, 0, NULL);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbInterface0->SelectAlternateSetting(1);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbInterface0->CopyPipe(MIDI_IN_EP, &ivars->usbMIDIinPipe);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbInterface0->CopyPipe(PCM_OUT_EP, &ivars->usbPCMoutPipe);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbInterface1->Open(this, 0, NULL);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbInterface1->SelectAlternateSetting(1);
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbInterface1->CopyPipe(PCM_IN_EP, &ivars->usbPCMinPipe);
	if (ret != kIOReturnSuccess) return ret;

	return ret;
}

uint8_t PloytecDriver::GetCurrentUrbCount()
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::GetCurrentUrbCount: DRIVER: %s", __func__);

	return ivars->usbCurrentUrbCount;
}

kern_return_t PloytecDriver::SetCurrentUrbCount(uint8_t num)
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::SetCurrentUrbCount: DRIVER: %s", __func__);

	__block kern_return_t ret = kIOReturnSuccess;

	ivars->workQueue->DispatchAsync(^(){
		AbortUSBUrbs(ivars->usbCurrentUrbCount);
		ret = SendPCMUrbs(num);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::SetCurrentUrbCount: SendPCMUrbs: %s", strerror(ret)); return; }
	});

	return ret;
}

uint16_t PloytecDriver::GetCurrentInputFramesCount()
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::GetCurrentInputFramesCount: DRIVER: %s", __func__);

	return ivars->usbCurrentInputFramesCount;
}

kern_return_t PloytecDriver::SetFrameCount(uint16_t inputCount, uint16_t outputCount)
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::SetFrameCount: DRIVER: %s", __func__);
	os_log(OS_LOG_DEFAULT, "PloytecDriver::SetFrameCount: %s: CHANGING FRAMECOUNT TO: INPUT %d OUTPUT %d", __func__, inputCount, outputCount);

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
		if (ret != kIOReturnSuccess) return;
		ret = CreateUSBRXBuffersPCM(ivars->usbInputPacketSize);
		if (ret != kIOReturnSuccess) return;
		ret = SendPCMUrbs(ivars->usbCurrentUrbCount);
		if (ret != kIOReturnSuccess) return;
	});

	return ret;
}

uint16_t PloytecDriver::GetCurrentOutputFramesCount()
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::GetCurrentOutputFramesCount: DRIVER: %s", __func__);

	return ivars->usbCurrentOutputFramesCount;
}

OSSharedPtr<OSString> PloytecDriver::MakeStringFromDescriptor(IOUSBHostDevice* device, uint8_t index)
{
	const IOUSBStringDescriptor* desc = device->CopyStringDescriptor(index);
	if (!desc || desc->bLength < 2)
		return nullptr;

	uint8_t length = desc->bLength;
	size_t charCount = (length - 2) / 2;
	if (charCount > 255)
		charCount = 255;

	char buffer[256];
	for (size_t i = 0; i < charCount; i++)
		buffer[i] = (char)desc->bString[i * 2];

	buffer[charCount] = '\0';
	return OSSharedPtr<OSString>(OSString::withCString(buffer), OSNoRetain);
}

kern_return_t PloytecDriver::CreateBuffers()
{
	kern_return_t ret = kIOReturnSuccess;
	IOAddressSegment range;

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 0x0f, 0, ivars->usbRXBufferCONTROL.attach());
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbRXBufferCONTROL->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) return ret;
	ivars->usbRXBufferCONTROLAddr = reinterpret_cast<uint8_t*>(range.address);

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 0x0f, 0, ivars->usbTXBufferCONTROL.attach());
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbTXBufferCONTROL->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) return ret;
	ivars->usbTXBufferCONTROLAddr = reinterpret_cast<uint8_t*>(range.address);

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 256, 0, ivars->MIDIBuffer.attach());
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->MIDIBuffer->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) return ret;
	ivars->MIDIBufferAddr = reinterpret_cast<uint8_t*>(range.address);

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, 32768, 0, ivars->usbRXBufferMIDI.attach());
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbRXBufferMIDI->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) return ret;
	ivars->usbRXBufferMIDIAddr = reinterpret_cast<uint8_t *>(range.address);

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, BUFFER_SIZE, 0, ivars->usbRXBufferPCM.attach());
	if (ret != kIOReturnSuccess) return ret;

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, BUFFER_SIZE, 0, ivars->usbTXBufferPCMandUART.attach());
	if (ret != kIOReturnSuccess) return ret;
	ret = ivars->usbTXBufferPCMandUART->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) return ret;
	ivars->usbTXBufferPCMandUARTAddr = reinterpret_cast<uint8_t *>(range.address);

	return ret;
}

kern_return_t IMPL(PloytecDriver, PCMinHandler)
{
	__block kern_return_t ret = kIOReturnSuccess;
	__block bool success = true;

	ivars->workQueue->DispatchAsync(^{
		if (ivars->usbShutdownInProgress)
		{
			os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMinHandler: Callback aborted: %s", __func__);
			ret = ivars->usbPCMinPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, NULL);
			if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMinHandler: Abort: %s", strerror(ret)); return; }
			ivars->usbPCMinAbortedCount++;
			return;
		}

		ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBufferPCMSegment[ivars->usbRXBufferPCMCurrentSegment].get(), ivars->usbInputPacketSize, ivars->usbPCMinCallback, 0);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMinHandler: Send: %s", strerror(ret)); return; }
		success = ivars->audioDevice->Capture(ivars->usbRXBufferPCMCurrentSegment, ivars->usbCurrentInputFramesCount, completionTimestamp);
		if (!success) { os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMinHandler: Capture fail"); ret = kIOReturnError; return; }
		ivars->usbPCMinFramesCount += ivars->usbCurrentInputFramesCount;
	});

	return ret;
}

kern_return_t IMPL(PloytecDriver, PCMoutHandler)
{
	__block kern_return_t ret = kIOReturnSuccess;
	__block bool success = true;

	ivars->workQueue->DispatchAsync(^{
		if (ivars->usbShutdownInProgress)
		{
			os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMoutHandler: Callback aborted: %s", __func__);
			ret = ivars->usbPCMoutPipe->Abort(kIOUSBAbortSynchronous, kIOReturnAborted, NULL);
			if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMoutHandler: Abort: %s", strerror(ret)); return; }
			ivars->usbPCMoutAbortedCount++;
			return;
		}

		success = ivars->audioDevice->Playback(ivars->usbTXBufferPCMandUARTCurrentSegment, ivars->usbCurrentOutputFramesCount, completionTimestamp);
		if (!success) { os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMoutHandler: Playback fail"); ret = kIOReturnError; return; }
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
		ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBufferPCMandUARTSegment[ivars->usbTXBufferPCMandUARTCurrentSegment].get(), ivars->usbOutputPacketSize, ivars->usbPCMoutCallback, 0);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDriver::PCMoutHandler: Send: %s", strerror(ret)); return; }
		ivars->usbPCMoutFramesCount += ivars->usbCurrentOutputFramesCount;
	});

	return ret;
}

kern_return_t IMPL(PloytecDriver, MIDIinHandler)
{
	uint8_t expectedLen = 0;

	for (uint32_t i = 0; i < actualByteCount; ++i)
	{
		uint8_t byte = ivars->usbRXBufferMIDIAddr[i];

		if (byte == 0xFD || byte == 0xFF)
			continue;

		if (byte >= 0xF8)
		{
			uint64_t msg = 0x01 | ((uint64_t)byte << 8);
			if (ivars->midiClient)
				ivars->midiClient->postMIDIMessage(msg);
			continue;
		}

		if (byte & 0x80)
		{
			ivars->midiParserRunningStatus = byte;
			ivars->midiParserBytes[0] = byte;
			ivars->midiParserIndex = 1;
		}
		else if (ivars->midiParserRunningStatus)
		{
			if (ivars->midiParserIndex < 3)
				ivars->midiParserBytes[ivars->midiParserIndex++] = byte;
		}

		switch (ivars->midiParserRunningStatus & 0xF0)
		{
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
