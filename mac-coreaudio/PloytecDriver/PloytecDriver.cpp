#include <DriverKit/DriverKit.h>
#include <USBDriverKit/USBDriverKit.h>
#include <DriverKit/IOService.h>
#include "PloytecDriver.h"
#include "PloytecAudioDevice.h"

#define PCM_OUT_EP	0x05
#define PCM_IN_EP	0x86
#define MIDI_IN_EP	0x83
#define PCM_N_URBS	4

constexpr uint32_t k_zero_time_stamp_period = 32768;

constexpr static const uint8_t bytes44100[3] = { 0x44, 0xAC, 0x00 };
constexpr static const uint8_t bytes48000[3] = { 0x80, 0xBB, 0x00 };
constexpr static const uint8_t bytes88200[3] = { 0x88, 0x58, 0x01 };
constexpr static const uint8_t bytes96000[3] = { 0x00, 0x77, 0x01 };

struct PloytecDriver_IVars
{
	OSSharedPtr<PloytecAudioDevice>		audioDevice;
	
	OSSharedPtr<IODispatchQueue>		workQueue;
	
	IOUSBHostDevice				*usbDevice;
	IOUSBHostInterface			*usbInterface0;
	IOUSBHostInterface			*usbInterface1;
	IOUSBHostPipe				*usbMIDIinPipe;
	IOUSBHostPipe				*usbPCMoutPipe;
	IOUSBHostPipe				*usbPCMinPipe;
	IOUSBEndpointDescriptor			*usbPCMinDescriptor;
	IOUSBEndpointDescriptor			*usbPCMoutDescriptor;

	OSAction				*usbPCMoutCallback;
	OSAction				*usbPCMinCallback;

	OSSharedPtr<IOBufferMemoryDescriptor>	usbRXBuffer;
	uint8_t					*usbRXBufferAddr;
	OSSharedPtr<IOBufferMemoryDescriptor>	usbTXBuffer;
	uint8_t					*usbTXBufferAddr;

	OSSharedPtr<OSData>			firmwareVersion;
	OSSharedPtr<OSString>			manufacturerName;
	OSSharedPtr<OSString>			deviceName;
};

bool PloytecDriver::init()
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::init()");
	
	bool result = false;
	result = super::init();
	if (!result)
		return false;

	ivars = IONewZero(PloytecDriver_IVars, 1);
	if (ivars == nullptr)
		return false;

	return true;
}

kern_return_t IMPL(PloytecDriver, Start)
{
	os_log(OS_LOG_DEFAULT, "PloytecDriver::start()");

	kern_return_t ret = kIOReturnSuccess;
	
	IOAddressSegment range;
	uint16_t bytesTransferred;
	bool success = false;
	uint32_t samplerate;
	int i = 0;
	
	//OSSharedPtr<OSString> name1 = OSSharedPtr(OSString::withCString("test1"), OSNoRetain);
	//OSSharedPtr<OSString> name2 = OSSharedPtr(OSString::withCString("test2"), OSNoRetain);
	//OSSharedPtr<OSString> name3 = OSSharedPtr(OSString::withCString("test3"), OSNoRetain);
	
	auto d = ivars->firmwareVersion;

	ret = Start(provider, SUPERDISPATCH);
	if (ret)
	{
		os_log(OS_LOG_DEFAULT, "Failed to start driver");
		goto Failure;
	}
	
	ivars->usbDevice = OSDynamicCast(IOUSBHostDevice, provider);
	if (ivars->usbDevice == nullptr)
	{
		os_log(OS_LOG_DEFAULT, "Failed to cast provider to IOUSBHostDevice");
		ret = kIOReturnNoDevice;
		goto Failure;
	}

	ret = ivars->usbDevice->Open(this, 0, NULL);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to open USB device");
		goto Failure;
	}
	
	PloytecDriver::GetHWManifacturer();
	
	os_log(OS_LOG_DEFAULT, "Found manufacturer: %s", ivars->manufacturerName->getCStringNoCopy());
	
	PloytecDriver::GetHWDeviceName();
	
	os_log(OS_LOG_DEFAULT, "Found device: %s", ivars->deviceName->getCStringNoCopy());
	
	ret = ivars->usbDevice->CreateIOBuffer(kIOMemoryDirectionInOut, 32768, ivars->usbRXBuffer.attach());
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to allocate USB receive buffer");
		goto Failure;
	}
	ret = ivars->usbRXBuffer->GetAddressRange(&range);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to get firmware address bytes");
		return false;
	}
	ivars->usbRXBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	
	ret = ivars->usbDevice->CreateIOBuffer(kIOMemoryDirectionInOut, 32768, ivars->usbTXBuffer.attach());
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to allocate USB transmit buffer");
		goto Failure;
	}
	ret = ivars->usbTXBuffer->GetAddressRange(&range);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to get firmware address bytes");
		return false;
	}
	ivars->usbTXBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	
	// get firmware
	PloytecDriver::GetHWFirmwareVer();
		
	if (d && d->getLength() >= 3) {
	    auto b = (const uint8_t*)d->getBytesNoCopy();
	    os_log(OS_LOG_DEFAULT, "Firmware version: 1.%d.%d", b[1], b[2]);
	}

	// get status
	PloytecDriver::GetHWStatus();

	// get samplerate
	PloytecDriver::GetHWSampleRate(&samplerate);
	os_log(OS_LOG_DEFAULT, "Current samplerate: %d", samplerate);

	// set 96 khz
	PloytecDriver::SetHWSampleRate(96000);

	// get samplerate
	PloytecDriver::GetHWSampleRate(&samplerate);
	os_log(OS_LOG_DEFAULT, "Current samplerate: %d", samplerate);
	
	// get status
	PloytecDriver::GetHWStatus();

	// allgood
	PloytecDriver::SendHWAllgood();
	
	// get the USB pipes
	PloytecDriver::CreateUSBPipes();
	
	PloytecDriver::GetUSBDescriptors();
	if (ivars->usbPCMoutDescriptor->bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk)
		os_log(OS_LOG_DEFAULT, "BULK STREAMING MODE");
	if (ivars->usbPCMoutDescriptor->bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt)
		os_log(OS_LOG_DEFAULT, "INT STREAMING MODE");

	ret = OSAction::Create(this, PloytecDriver_PCMinHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMinCallback);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to create the PCM in USB handler");
		goto Failure;
	}

	ret = OSAction::Create(this, PloytecDriver_PCMoutHandler_ID, IOUSBHostPipe_CompleteAsyncIO_ID, 0, &ivars->usbPCMoutCallback);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to create the PCM out USB handler");
		goto Failure;
	}

	for (i = 0; i < PCM_N_URBS; i++) {
		if (ivars->usbPCMoutDescriptor->bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
			ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBuffer.get(), 2048, ivars->usbPCMoutCallback, 0);
			if (ret != kIOReturnSuccess)
			{
				os_log(OS_LOG_DEFAULT, "USB FAIL: %s", StringFromReturn(ret));
				return false;
			}
		}
		ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBuffer.get(), 1928, ivars->usbPCMoutCallback, 0);
		os_log(OS_LOG_DEFAULT, "URBBBB %d", i);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "USB FAIL: %s", StringFromReturn(ret));
			return false;
		}
		ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBuffer.get(), 2048, ivars->usbPCMinCallback, 0);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "USB FAIL: %s", StringFromReturn(ret));
			return false;
		}
	}

	ivars->workQueue = GetWorkQueue();
	if (ivars->workQueue.get() == nullptr)
	{
		os_log(OS_LOG_DEFAULT, "Failed to get work queue");
		ret = kIOReturnInvalid;
		goto Failure;
	}
	
	ivars->audioDevice = OSSharedPtr(OSTypeAlloc(PloytecAudioDevice), OSNoRetain);
	if (ivars->audioDevice.get() == nullptr)
	{
		os_log(OS_LOG_DEFAULT, "Failed to allocate memory for audio device");
		ret = kIOReturnNoMemory;
		goto Failure;
	}
	
	success = ivars->audioDevice->init(this, false, ivars->deviceName.get(), ivars->deviceName.get(), ivars->manufacturerName.get(), k_zero_time_stamp_period);
	if (!success)
	{
		os_log(OS_LOG_DEFAULT, "Failed to initialize audio device");
		ret = kIOReturnNoMemory;
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 2");
	
	ivars->audioDevice->SetName(ivars->deviceName.get());
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set name to audio device");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 3");

	ret = AddObject(ivars->audioDevice.get());
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to add audio device object");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 4");
	
	ret = RegisterService();
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to register service");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "got in audio driver start 5");
	
	return ret;
	
Failure:
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
	IOSafeDeleteNULL(ivars, PloytecDriver_IVars, 1);
	super::free();
}

kern_return_t PloytecDriver::StartDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret = kIOReturnSuccess;
	
	if (in_object_id != ivars->audioDevice->GetObjectID())
	{
		os_log(OS_LOG_DEFAULT, "AudioDriver::StartDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	ivars->workQueue->DispatchSync(^(){
		ret = super::StartDevice(in_object_id, in_flags);
	});
	if (ret == kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "we getting here StartDevice");
		// Enable any custom driver-related things here.
	}
	return ret;
}

kern_return_t PloytecDriver::StopDevice(IOUserAudioObjectID in_object_id, IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret;

	if (in_object_id != ivars->audioDevice->GetObjectID())
	{
		os_log(OS_LOG_DEFAULT, "AudioDriver::StopDevice - unknown object id %u", in_object_id);
		return kIOReturnBadArgument;
	}

	ivars->workQueue->DispatchSync(^(){
		ret = super::StopDevice(in_object_id, in_flags);
	});

	if (ret == kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "we getting here StopDevice");
		// Disable any custom driver-related things here.
	}
	return ret;
}

kern_return_t PloytecDriver::NewUserClient_Impl(uint32_t in_type, IOUserClient** out_user_client)
{
	__block kern_return_t ret = kIOReturnSuccess;
	
	if (in_type == kIOUserAudioDriverUserClientType)
	{
		ret = super::NewUserClient(in_type, out_user_client, SUPERDISPATCH);
		if (ret != kIOReturnSuccess)
			return ret;
	}
	else
	{
		IOService* user_client_service = nullptr;
		ret = Create(this, "PloytecDriverUserClientProperties", &user_client_service);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "Failed to create user client service");
			return ret;
		}
		*out_user_client = OSDynamicCast(IOUserClient, user_client_service);
	}
}

bool PloytecDriver::GetHWFirmwareVer()
{
	__block kern_return_t ret = kIOReturnSuccess;
	uint16_t bytesTransferred;

	ret = ivars->usbDevice->DeviceRequest(this, 0xc0, 0x56, 0x00, 0x00, 0x0f, ivars->usbRXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to get firmware version from device");
		return false;
	}
	
	os_log(OS_LOG_DEFAULT, "got here in functionnn");
	
	ivars->firmwareVersion->withBytesNoCopy(ivars->usbRXBufferAddr, 3);
	
	os_log(OS_LOG_DEFAULT, "functionnn finished: bytes transferred: %d", bytesTransferred);
	
	return true;
}

bool PloytecDriver::GetHWSampleRate(uint32_t *samplerate)
{
	__block kern_return_t ret;
	uint16_t bytesTransferred;

	ret = ivars->usbDevice->DeviceRequest(this, 0xA2, 0x81, 0x0100, 0, 0x03, ivars->usbRXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to get samplerate from device");
		return false;
	}
	if (ivars->usbRXBufferAddr[0] == bytes44100[0] && ivars->usbRXBufferAddr[1] == bytes44100[1] && ivars->usbRXBufferAddr[2] == bytes44100[2])
		*samplerate = 44100;
	else if (ivars->usbRXBufferAddr[0] == bytes48000[0] && ivars->usbRXBufferAddr[1] == bytes48000[1] && ivars->usbRXBufferAddr[2] == bytes48000[2])
		*samplerate = 48000;
	else if (ivars->usbRXBufferAddr[0] == bytes88200[0] && ivars->usbRXBufferAddr[1] == bytes88200[1] && ivars->usbRXBufferAddr[2] == bytes88200[2])
		*samplerate = 88200;
	else if (ivars->usbRXBufferAddr[0] == bytes96000[0] && ivars->usbRXBufferAddr[1] == bytes96000[1] && ivars->usbRXBufferAddr[2] == bytes96000[2])
		*samplerate = 96000;
	else
	{
		os_log(OS_LOG_DEFAULT, "Unknown samplerate bytes: %02X %02X %02X", ivars->usbRXBufferAddr[0], ivars->usbRXBufferAddr[1], ivars->usbRXBufferAddr[2]);
		return false;
	}
	return true;
}

bool PloytecDriver::SetHWSampleRate(uint32_t samplerate)
{
	__block kern_return_t ret;
	uint16_t bytesTransferred;

	switch (samplerate)
	{
		case 44100: memcpy(ivars->usbTXBufferAddr, bytes44100, 3); break;
		case 48000: memcpy(ivars->usbTXBufferAddr, bytes48000, 3); break;
		case 88200: memcpy(ivars->usbTXBufferAddr, bytes88200, 3); break;
		case 96000: memcpy(ivars->usbTXBufferAddr, bytes96000, 3); break;
		default:
			os_log(OS_LOG_DEFAULT, "Unsupported sample rate: %d", samplerate);
			return false;
	}

	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->usbTXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set samplerate on device");
		return false;
	}
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->usbTXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set samplerate on device");
		return false;
	}
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->usbTXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set samplerate on device");
		return false;
	}
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->usbTXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set samplerate on device");
		return false;
	}	
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0005, 0x03, ivars->usbTXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set samplerate on device");
		return false;
	}
	ret = ivars->usbDevice->DeviceRequest(this, 0x22, 0x01, 0x0100, 0x0086, 0x03, ivars->usbTXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set samplerate on device");
		return false;
	}
	return true;
}

bool PloytecDriver::GetHWStatus()
{
	__block kern_return_t ret;
	uint16_t bytesTransferred;

	ret = ivars->usbDevice->DeviceRequest(this, 0xC0, 0x49, 0x0000, 0x0000, 0x01, ivars->usbRXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to get status from device");
		return false;
	}
}

bool PloytecDriver::SendHWAllgood()
{
	__block kern_return_t ret;
	uint16_t bytesTransferred;

	ret = ivars->usbDevice->DeviceRequest(this, 0x40, 0x49, 0xFFB2, 0x0000, 0x00, ivars->usbTXBuffer.get(), &bytesTransferred, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to send allgood to device");
		return false;
	}
}

void PloytecDriver::GetHWManifacturer()
{
	auto desc = ivars->usbDevice->CopyStringDescriptor(1, 0x0409);
	if (!desc)
		return false;

	uint32_t len = (desc->bLength - 2) / 2;
	char* utf8 = new char[len + 1];

	for (uint32_t i = 0; i < len; ++i)
		utf8[i] = desc->bString[i * 2];
	utf8[len] = '\0';

	ivars->manufacturerName = OSSharedPtr<OSString>(OSString::withCString(utf8), OSNoRetain);
	delete[] utf8;

	return;
}

void PloytecDriver::GetHWDeviceName()
{
	auto desc = ivars->usbDevice->CopyStringDescriptor(2, 0x0409);
	if (!desc)
		return false;

	uint32_t len = (desc->bLength - 2) / 2;
	char* utf8 = new char[len + 1];

	for (uint32_t i = 0; i < len; ++i)
		utf8[i] = desc->bString[i * 2];
	utf8[len] = '\0';

	ivars->deviceName = OSSharedPtr<OSString>(OSString::withCString(utf8), OSNoRetain);
	delete[] utf8;

	return;
}

bool PloytecDriver::SendEmptyUrbs()
{
	int i = 0;
	__block kern_return_t ret;
	
	os_log(OS_LOG_DEFAULT, "sendemptyurbs()");
	
	if (ivars->usbPCMoutDescriptor->bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk)
		os_log(OS_LOG_DEFAULT, "BAA BULK STREAMING MODE");
	if (ivars->usbPCMoutDescriptor->bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt)
		os_log(OS_LOG_DEFAULT, "BAA INT STREAMING MODE");
	
	for (i = 0; i < PCM_N_URBS; i++) {
		/*
		if (ivars->usbPCMoutDescriptor->bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
			ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBuffer.get(), 2048, ivars->usbPCMoutCallback, 0);
			os_log(OS_LOG_DEFAULT, "sendemptyurbsbulk()");
			if (ret != kIOReturnSuccess)
				return false;
		}
		*/
		os_log(OS_LOG_DEFAULT, "sendemptyurbsint()?????");
		ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBuffer.get(), 1928, ivars->usbPCMoutCallback, 0);
		os_log(OS_LOG_DEFAULT, "sendemptyurbsint()");
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "niet maak1: %s", StringFromReturn(ret));
			return false;
		}
		ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBuffer.get(), 2048, ivars->usbPCMinCallback, 0);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "niet maak2");
			return false;
		}
	}
}

bool PloytecDriver::CreateUSBPipes()
{
	__block kern_return_t ret;

	uintptr_t interfaceIterator;

	ret = ivars->usbDevice->SetConfiguration(1, false);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to set configuration 1 on USB device");
		return false;
	}
	ret = ivars->usbDevice->CreateInterfaceIterator(&interfaceIterator);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to create interface iterator");
		return false;
	}
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to copy interface 0 from USB device");
		return false;
	}
	ret = ivars->usbInterface0->Open(this, NULL, NULL);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to open interface 0 on USB device");
		return false;
	}
	ret = ivars->usbInterface0->SelectAlternateSetting(1);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to select alternate setting 1 on interface 0");
		return false;
	}
	ret = ivars->usbDevice->CopyInterface(interfaceIterator, &ivars->usbInterface1);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to copy interface 1 from USB device");
		return false;
	}
	ret = ivars->usbInterface1->Open(this, NULL, NULL);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to open interface 1 on USB device");
		return false;
	}
	ret = ivars->usbInterface1->SelectAlternateSetting(1);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to select alternate setting 1 on interface 1");
		return false;
	}
	ret = ivars->usbInterface0->CopyPipe(MIDI_IN_EP, &ivars->usbMIDIinPipe);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to copy MIDI in pipe from interface 0");
		return false;
	}
	ret = ivars->usbInterface1->CopyPipe(PCM_IN_EP, &ivars->usbPCMinPipe);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to copy PCM in pipe from interface 1");
		return false;
	}
	ret = ivars->usbInterface0->CopyPipe(PCM_OUT_EP, &ivars->usbPCMoutPipe);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to copy PCM out pipe from interface 0");
		return false;
	}
	return true;
}

bool PloytecDriver::GetUSBDescriptors()
{
	__block kern_return_t ret;

	IOUSBStandardEndpointDescriptors indescriptor;
	IOUSBStandardEndpointDescriptors outdescriptor;
	ret = ivars->usbPCMinPipe->GetDescriptors(&indescriptor, kIOUSBGetEndpointDescriptorOriginal);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to GetDescriptors from PCM in!");
		return false;
	}
	ret = ivars->usbPCMoutPipe->GetDescriptors(&outdescriptor, kIOUSBGetEndpointDescriptorOriginal);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to GetDescriptors from PCM out!");
		return false;
	}
	ivars->usbPCMinDescriptor = &indescriptor.descriptor;
	ivars->usbPCMoutDescriptor = &outdescriptor.descriptor;
}

OSData* PloytecDriver::GetFirmwareVer()
{
	return OSData::withBytes(ivars->firmwareVersion->getBytesNoCopy(), ivars->firmwareVersion->getLength());
}

OSData* PloytecDriver::GetDeviceName()
{
	return OSData::withBytes(ivars->deviceName->getCStringNoCopy(), ivars->deviceName->getLength());
}

OSData* PloytecDriver::GetDeviceManufacturer()
{
	return OSData::withBytes(ivars->manufacturerName->getCStringNoCopy(), ivars->manufacturerName->getLength());
}

kern_return_t IMPL(PloytecDriver, PCMinHandler)
{
	__block kern_return_t ret;
	
	//os_log(OS_LOG_DEFAULT, "PCMINHANDLER");
	
	ret = ivars->usbPCMinPipe->AsyncIO(ivars->usbRXBuffer.get(), 2048, ivars->usbPCMinCallback, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to read PCM packet from USB");
		return ret;
	}
	//ivars->audioDevice->ReadPCMPacketInt(ivars->usbRXBuffer.get(), 2048, completionTimestamp);

	return ret;
}

kern_return_t IMPL(PloytecDriver, PCMoutHandler)
{
	__block kern_return_t ret;
	
	//os_log(OS_LOG_DEFAULT, "PCMOUTHANDLER");
	
	ivars->audioDevice->FillPCMPacketInt(ivars->usbTXBufferAddr, 1928, completionTimestamp);
	ret = ivars->usbPCMoutPipe->AsyncIO(ivars->usbTXBuffer.get(), 1928, ivars->usbPCMoutCallback, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "Failed to send PCM packet to USB");
		return ret;
	}

	return ret;
}
