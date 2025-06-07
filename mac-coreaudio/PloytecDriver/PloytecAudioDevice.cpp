//
//  PloytecAudioDevice.cpp
//  PloytecDriver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecAudioDevice.h"

#define sampleRate44100				44100.0
#define sampleRate48000				48000.0
#define sampleRate88200				88200.0
#define sampleRate96000				96000.0

#define INITIAL_BUFFERSIZE			2560
//#define MAX_BUFFERSIZE				800000

#define PCM_N_PLAYBACK_CHANNELS			8
#define PCM_N_CAPTURE_CHANNELS			8

#define PLOYTEC_PCM_OUT_FRAME_SIZE		48
#define PLOYTEC_PCM_IN_FRAME_SIZE		64

#define COREAUDIO_BYTES_PER_SAMPLE		3 // S24_3LE
#define COREAUDIO_PLAYBACK_BYTES_PER_FRAME	(PCM_N_PLAYBACK_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE)
#define COREAUDIO_CAPTURE_BYTES_PER_FRAME	(PCM_N_CAPTURE_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE)

struct PloytecAudioDevice_IVars
{
	OSSharedPtr<IOUserAudioDriver>		audioDriver;
	OSSharedPtr<IODispatchQueue>		workQueue;
	
	OSSharedPtr<IOUserAudioStream>		pcmOutputStream;
	OSSharedPtr<IOBufferMemoryDescriptor> 	pcmOutputBuffer;
	OSSharedPtr<IOMemoryMap>		pcmOutputMemoryMap;
	uint8_t*				pcmOutputBufferAddr;
	
	OSSharedPtr<IOUserAudioStream>		pcmInputStream;
	OSSharedPtr<IOBufferMemoryDescriptor> 	pcmInputBuffer;
	OSSharedPtr<IOMemoryMap>		pcmInputMemoryMap;
	uint8_t*				pcmInputBufferAddr;
	
	IOUserAudioStreamBasicDescription	outputStreamFormat;
	IOUserAudioStreamBasicDescription	inputStreamFormat;
	
	uint32_t				buffersize;
	
	uint64_t				xruns;
	
	uint32_t				currentsample;
	
	bool					startpcmin;
	bool					startpcmout;
};

bool PloytecAudioDevice::init(IOUserAudioDriver* in_driver, bool in_supports_prewarming, OSString* in_device_uid, OSString* in_model_uid, OSString* in_manufacturer_uid, uint32_t in_zero_timestamp_period)
{
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice::init()");
	
	IOReturn ret = kIOReturnSuccess;
	
	auto success = super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period);
	if (!success) {
		return false;
	}
	ivars = IONewZero(PloytecAudioDevice_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 1");
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice: Found manufacturer: %s", in_manufacturer_uid->getCStringNoCopy());
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice: Found device: %s", in_device_uid->getCStringNoCopy());

	ivars->buffersize = INITIAL_BUFFERSIZE;
	ivars->startpcmin = false;
	ivars->startpcmout = false;
	//OSSharedPtr<OSString> name1 = OSSharedPtr(OSString::withCString("test1"), OSNoRetain);
	//OSSharedPtr<OSString> name2 = OSSharedPtr(OSString::withCString("test2"), OSNoRetain);
	OSSharedPtr<OSString> outputStreamName = OSSharedPtr(OSString::withCString("Ploytec PCM OUT"), OSNoRetain);
	OSSharedPtr<OSString> inputStreamName = OSSharedPtr(OSString::withCString("Ploytec PCM IN"), OSNoRetain);
	ivars->audioDriver = OSSharedPtr(in_driver, OSRetain);
	ivars->workQueue = GetWorkQueue();
	double sampleRates[4] = { 44100, 48000, 88200, 96000 };
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 2");
	
	IOUserAudioStreamBasicDescription outputStreamFormats[4] =
	{
		{
			44100, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_PLAYBACK_CHANNELS),
			24
		},
		{
			48000, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_PLAYBACK_CHANNELS),
			24
		},
		{
			88200, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_PLAYBACK_CHANNELS),
			24
		},
		{
			96000, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_PLAYBACK_CHANNELS),
			24
		}
	};
	IOUserAudioStreamBasicDescription inputStreamFormats[4] =
	{
		{
			44100, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_CAPTURE_CHANNELS),
			24
		},
		{
			48000, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_CAPTURE_CHANNELS),
			24
		},
		{
			88200, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_CAPTURE_CHANNELS),
			24
		},
		{
			96000, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_CAPTURE_CHANNELS),
			24
		}
	};

	// set the transport type in CoreAudio
	ret = SetTransportType(IOUserAudioTransportType::USB);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set transport type in CoreAudio");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 3");

	// allocate the CoreAudio ring buffers
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, INITIAL_BUFFERSIZE, 0, ivars->pcmOutputBuffer.attach());
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to create output ring buffer");
		goto Failure;
	}
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, INITIAL_BUFFERSIZE, 0, ivars->pcmInputBuffer.attach());
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to create input ring buffer");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 4");

	// create the streams
	ivars->pcmOutputStream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, ivars->pcmOutputBuffer.get());
	if (ivars->pcmOutputStream.get() == nullptr) {
		os_log(OS_LOG_DEFAULT, "Failed to create output stream");
		goto Failure;
	}
	ivars->pcmInputStream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Input, ivars->pcmInputBuffer.get());
	if (ivars->pcmInputStream.get() == nullptr) {
		os_log(OS_LOG_DEFAULT, "Failed to create input stream");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 5");

	// set names on input/output streams
	ret = ivars->pcmOutputStream->SetName(outputStreamName.get());
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set name on output stream");
		goto Failure;
	}
	ret = ivars->pcmInputStream->SetName(inputStreamName.get());
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set name on input stream");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 6");
	
	// set the available sample rates in CoreAudio
	ret = SetAvailableSampleRates(sampleRates, 4);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set available sample rates in CoreAudio");
		goto Failure;
	}

	// set the current sample rate in CoreAudio
	ret = SetSampleRate(96000);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set current sample rate in CoreAudio");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 7");

	// set the available sample formats on the streams
	ret = ivars->pcmOutputStream->SetAvailableStreamFormats(outputStreamFormats, 4);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set available stream formats on output stream");
		goto Failure;
	}
	ret = ivars->pcmInputStream->SetAvailableStreamFormats(inputStreamFormats, 4);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set available stream formats on input stream");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 8");

	// set the current stream format on the stream
	ret = ivars->pcmOutputStream->SetCurrentStreamFormat(&outputStreamFormats[3]);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set current stream format on output stream");
		goto Failure;
	}
	ret = ivars->pcmInputStream->SetCurrentStreamFormat(&inputStreamFormats[3]);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to set current stream format on input stream");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 9");

	// add the streams to CoreAudio
	ret = AddStream(ivars->pcmOutputStream.get());
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to add output stream");
		goto Failure;
	}
	ret = AddStream(ivars->pcmInputStream.get());
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to add input stream");
		goto Failure;
	}
	
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice 10");
	
	return true;

Failure:
	ivars->audioDriver.reset();
	ivars->pcmOutputStream.reset();
	ivars->pcmOutputMemoryMap.reset();
	ivars->pcmInputStream.reset();
	ivars->pcmInputMemoryMap.reset();
	return false;
}

kern_return_t PloytecAudioDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
	os_log(OS_LOG_DEFAULT, "PloytecAudioDevice::StartIO()");
	
	__block kern_return_t ret = kIOReturnSuccess;
	
	ivars->workQueue->DispatchSync(^(){
		ret = super::StartIO(in_flags);
		if (ret != kIOReturnSuccess) {
			os_log(OS_LOG_DEFAULT, "Failed to start I/O in super class");
			return;
		}

		ret = ivars->pcmOutputBuffer->CreateMapping(0, 0, 0, 0, 0, ivars->pcmOutputMemoryMap.attach());
		if (ret != kIOReturnSuccess) {
			os_log(OS_LOG_DEFAULT, "Failed to create memory map for output buffer");
			return;
		}
		ivars->pcmOutputBufferAddr = reinterpret_cast<uint8_t*>(ivars->pcmOutputMemoryMap->GetAddress() + ivars->pcmOutputMemoryMap->GetOffset());
		
		ret = ivars->pcmInputBuffer->CreateMapping(0, 0, 0, 0, 0, ivars->pcmInputMemoryMap.attach());
		if (ret != kIOReturnSuccess) {
			os_log(OS_LOG_DEFAULT, "Failed to create memory map for input ring buffer");
			return;
		}
		ivars->pcmInputBufferAddr = reinterpret_cast<uint8_t*>(ivars->pcmInputMemoryMap->GetAddress() + ivars->pcmInputMemoryMap->GetOffset());
		
		ivars->startpcmin = true;
		ivars->startpcmout = true;
		
		ivars->currentsample = 0;

		UpdateCurrentZeroTimestamp(0, 0);
		
		return;

	Failure:
		super::StopIO(in_flags);
		ivars->pcmOutputMemoryMap.reset();
		ivars->pcmInputMemoryMap.reset();
		return;
	});

	return ret;
}

kern_return_t PloytecAudioDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret;

	ivars->workQueue->DispatchSync(^(){
		ret = super::StopIO(in_flags);
		ivars->startpcmin = false;
		ivars->startpcmout = false;
	});

	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to stop IO, error %d", ret);
	}

	return ret;
}

kern_return_t PloytecAudioDevice::PerformDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	kern_return_t ret = kIOReturnSuccess;
	switch (change_action) {
		case changeSampleRateAction:
			{}
			break;
			
		default:
			ret = super::PerformDeviceConfigurationChange(change_action, in_change_info);
			break;
	}
	
	return ret;
}

kern_return_t PloytecAudioDevice::AbortDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	return super::AbortDeviceConfigurationChange(change_action, in_change_info);
}

void PloytecAudioDevice::free()
{
	if (ivars != nullptr) {
		ivars->audioDriver.reset();
		ivars->pcmOutputStream.reset();
		ivars->pcmOutputMemoryMap.reset();
		ivars->pcmInputStream.reset();
		ivars->pcmInputMemoryMap.reset();
		ivars->workQueue.reset();
	}
	IOSafeDeleteNULL(ivars, PloytecAudioDevice_IVars, 1);
	super::free();
}

bool PloytecAudioDevice::FillPCMPacketBulk(uint8_t* buffer, uint32_t packetSize, uint64_t completionTimestamp)
{
	/*
	 __block kern_return_t ret;
	__block IOAddressSegment range;
	ret = buffer->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to get address range for buffer");
		return false;
	}
	uint8_t* usbOutputBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	uint64_t pcmOutCurrentSample;
	GetCurrentClientSampleTime(nullptr, &pcmOutCurrentSample);
	 */
	
	if (ivars->startpcmout == true)
		os_log(OS_LOG_DEFAULT, "startpcmout true");
	else
		os_log(OS_LOG_DEFAULT, "startpcmout false");
		//ploytecPCMencode(buffer, ivars->pcmOutputBufferAddr);

	/*
	ploytecPCMencode(usbOutputBufferAddr + (0 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (0 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (1 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (1 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (2 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (2 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (3 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (3 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (4 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (4 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (5 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (5 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (6 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (6 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (7 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (7 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (8 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (8 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
	ploytecPCMencode(usbOutputBufferAddr + (9 * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (9 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));

	if(pcmOutCurrentSample == ivars->buffersize) {
		uint64_t pcmOutCurrentZeroTimestamp;
		GetCurrentZeroTimestamp(&pcmOutCurrentZeroTimestamp, nullptr);
		pcmOutCurrentZeroTimestamp += ivars->buffersize;
		UpdateCurrentZeroTimestamp(pcmOutCurrentZeroTimestamp, completionTimestamp);
	}
	 */

	return true;
}

bool PloytecAudioDevice::FillPCMPacketInt(uint8_t* buffer, uint32_t packetSize, uint64_t completionTimestamp)
{
	uint64_t pcmOutCurrentSample;
	GetCurrentClientSampleTime(nullptr, &pcmOutCurrentSample);
	
	//os_log(OS_LOG_DEFAULT, "completionTimestamp: %llu", completionTimestamp);
	
	if (ivars->startpcmout == true)
	{
		if(ivars->currentsample == ivars->buffersize) {
			ivars->currentsample = 0;
			uint64_t pcmOutCurrentZeroTimestamp;
			GetCurrentZeroTimestamp(&pcmOutCurrentZeroTimestamp, nullptr);
			pcmOutCurrentZeroTimestamp += ivars->buffersize;
			UpdateCurrentZeroTimestamp(pcmOutCurrentZeroTimestamp, completionTimestamp);
		}
		
		ploytecPCMencode(buffer + (0 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 0, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (0 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (1 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (1 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (2 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (2 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (3 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (3 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (4 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (4 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (5 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (5 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (6 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (6 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (7 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (7 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (8 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 2, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (8 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		ploytecPCMencode(buffer + (9 * PLOYTEC_PCM_OUT_FRAME_SIZE) + 4, ivars->pcmOutputBufferAddr + ((pcmOutCurrentSample * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) + (9 * COREAUDIO_PLAYBACK_BYTES_PER_FRAME) % ivars->buffersize));
		
		ivars->currentsample += 40;
	}

	return true;
}

bool PloytecAudioDevice::ReadPCMPacketBulk(uint8_t* buffer, uint32_t packetSize, uint64_t completionTimestamp)
{}

bool PloytecAudioDevice::ReadPCMPacketInt(uint8_t* buffer, uint32_t packetSize, uint64_t completionTimestamp)
{}

/* Takes 24 bytes, outputs 48 bytes */
void PloytecAudioDevice::ploytecPCMencode(uint8_t *dest, uint8_t *src)
{
    //                        =============== channel 1 ===============   =============== channel 3 ===============   =============== channel 5 ===============   =============== channel 7 ===============
    ((uint8_t *)dest)[0x00] = ((((uint8_t *)src)[0x02] & 0x80) >> 0x07) | ((((uint8_t *)src)[0x08] & 0x80) >> 0x06) | ((((uint8_t *)src)[0x0E] & 0x80) >> 0x05) | ((((uint8_t *)src)[0x14] & 0x80) >> 0x04);
    ((uint8_t *)dest)[0x01] = ((((uint8_t *)src)[0x02] & 0x40) >> 0x06) | ((((uint8_t *)src)[0x08] & 0x40) >> 0x05) | ((((uint8_t *)src)[0x0E] & 0x40) >> 0x04) | ((((uint8_t *)src)[0x14] & 0x40) >> 0x03);
    ((uint8_t *)dest)[0x02] = ((((uint8_t *)src)[0x02] & 0x20) >> 0x05) | ((((uint8_t *)src)[0x08] & 0x20) >> 0x04) | ((((uint8_t *)src)[0x0E] & 0x20) >> 0x03) | ((((uint8_t *)src)[0x14] & 0x20) >> 0x02);
    ((uint8_t *)dest)[0x03] = ((((uint8_t *)src)[0x02] & 0x10) >> 0x04) | ((((uint8_t *)src)[0x08] & 0x10) >> 0x03) | ((((uint8_t *)src)[0x0E] & 0x10) >> 0x02) | ((((uint8_t *)src)[0x14] & 0x10) >> 0x01);
    ((uint8_t *)dest)[0x04] = ((((uint8_t *)src)[0x02] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x08] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x0E] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x14] & 0x08) >> 0x00);
    ((uint8_t *)dest)[0x05] = ((((uint8_t *)src)[0x02] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x08] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x0E] & 0x04) >> 0x00) | ((((uint8_t *)src)[0x14] & 0x04) << 0x01);
    ((uint8_t *)dest)[0x06] = ((((uint8_t *)src)[0x02] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x08] & 0x02) >> 0x00) | ((((uint8_t *)src)[0x0E] & 0x02) << 0x01) | ((((uint8_t *)src)[0x14] & 0x02) << 0x02);
    ((uint8_t *)dest)[0x07] = ((((uint8_t *)src)[0x02] & 0x01) >> 0x00) | ((((uint8_t *)src)[0x08] & 0x01) << 0x01) | ((((uint8_t *)src)[0x0E] & 0x01) << 0x02) | ((((uint8_t *)src)[0x14] & 0x01) << 0x03);
    ((uint8_t *)dest)[0x08] = ((((uint8_t *)src)[0x01] & 0x80) >> 0x07) | ((((uint8_t *)src)[0x07] & 0x80) >> 0x06) | ((((uint8_t *)src)[0x0D] & 0x80) >> 0x05) | ((((uint8_t *)src)[0x13] & 0x80) >> 0x04);
    ((uint8_t *)dest)[0x09] = ((((uint8_t *)src)[0x01] & 0x40) >> 0x06) | ((((uint8_t *)src)[0x07] & 0x40) >> 0x05) | ((((uint8_t *)src)[0x0D] & 0x40) >> 0x04) | ((((uint8_t *)src)[0x13] & 0x40) >> 0x03);
    ((uint8_t *)dest)[0x0A] = ((((uint8_t *)src)[0x01] & 0x20) >> 0x05) | ((((uint8_t *)src)[0x07] & 0x20) >> 0x04) | ((((uint8_t *)src)[0x0D] & 0x20) >> 0x03) | ((((uint8_t *)src)[0x13] & 0x20) >> 0x02);
    ((uint8_t *)dest)[0x0B] = ((((uint8_t *)src)[0x01] & 0x10) >> 0x04) | ((((uint8_t *)src)[0x07] & 0x10) >> 0x03) | ((((uint8_t *)src)[0x0D] & 0x10) >> 0x02) | ((((uint8_t *)src)[0x13] & 0x10) >> 0x01);
    ((uint8_t *)dest)[0x0C] = ((((uint8_t *)src)[0x01] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x07] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x0D] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x13] & 0x08) >> 0x00);
    ((uint8_t *)dest)[0x0D] = ((((uint8_t *)src)[0x01] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x07] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x0D] & 0x04) >> 0x00) | ((((uint8_t *)src)[0x13] & 0x04) << 0x01);
    ((uint8_t *)dest)[0x0E] = ((((uint8_t *)src)[0x01] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x07] & 0x02) >> 0x00) | ((((uint8_t *)src)[0x0D] & 0x02) << 0x01) | ((((uint8_t *)src)[0x13] & 0x02) << 0x02);
    ((uint8_t *)dest)[0x0F] = ((((uint8_t *)src)[0x01] & 0x01) >> 0x00) | ((((uint8_t *)src)[0x07] & 0x01) << 0x01) | ((((uint8_t *)src)[0x0D] & 0x01) << 0x02) | ((((uint8_t *)src)[0x13] & 0x01) << 0x03);
    ((uint8_t *)dest)[0x10] = ((((uint8_t *)src)[0x00] & 0x80) >> 0x07) | ((((uint8_t *)src)[0x06] & 0x80) >> 0x06) | ((((uint8_t *)src)[0x0C] & 0x80) >> 0x05) | ((((uint8_t *)src)[0x12] & 0x80) >> 0x04);
    ((uint8_t *)dest)[0x11] = ((((uint8_t *)src)[0x00] & 0x40) >> 0x06) | ((((uint8_t *)src)[0x06] & 0x40) >> 0x05) | ((((uint8_t *)src)[0x0C] & 0x40) >> 0x04) | ((((uint8_t *)src)[0x12] & 0x40) >> 0x03);
    ((uint8_t *)dest)[0x12] = ((((uint8_t *)src)[0x00] & 0x20) >> 0x05) | ((((uint8_t *)src)[0x06] & 0x20) >> 0x04) | ((((uint8_t *)src)[0x0C] & 0x20) >> 0x03) | ((((uint8_t *)src)[0x12] & 0x20) >> 0x02);
    ((uint8_t *)dest)[0x13] = ((((uint8_t *)src)[0x00] & 0x10) >> 0x04) | ((((uint8_t *)src)[0x06] & 0x10) >> 0x03) | ((((uint8_t *)src)[0x0C] & 0x10) >> 0x02) | ((((uint8_t *)src)[0x12] & 0x10) >> 0x01);
    ((uint8_t *)dest)[0x14] = ((((uint8_t *)src)[0x00] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x06] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x0C] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x12] & 0x08) >> 0x00);
    ((uint8_t *)dest)[0x15] = ((((uint8_t *)src)[0x00] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x06] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x0C] & 0x04) >> 0x00) | ((((uint8_t *)src)[0x12] & 0x04) << 0x01);
    ((uint8_t *)dest)[0x16] = ((((uint8_t *)src)[0x00] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x06] & 0x02) >> 0x00) | ((((uint8_t *)src)[0x0C] & 0x02) << 0x01) | ((((uint8_t *)src)[0x12] & 0x02) << 0x02);
    ((uint8_t *)dest)[0x17] = ((((uint8_t *)src)[0x00] & 0x01) >> 0x00) | ((((uint8_t *)src)[0x06] & 0x01) << 0x01) | ((((uint8_t *)src)[0x0C] & 0x01) << 0x02) | ((((uint8_t *)src)[0x12] & 0x01) << 0x03);
    //                        =============== channel 2 ===============   =============== channel 4 ===============   =============== channel 6 ===============   =============== channel 8 ===============
    ((uint8_t *)dest)[0x18] = ((((uint8_t *)src)[0x05] & 0x80) >> 0x07) | ((((uint8_t *)src)[0x0B] & 0x80) >> 0x06) | ((((uint8_t *)src)[0x11] & 0x80) >> 0x05) | ((((uint8_t *)src)[0x17] & 0x80) >> 0x04);
    ((uint8_t *)dest)[0x19] = ((((uint8_t *)src)[0x05] & 0x40) >> 0x06) | ((((uint8_t *)src)[0x0B] & 0x40) >> 0x05) | ((((uint8_t *)src)[0x11] & 0x40) >> 0x04) | ((((uint8_t *)src)[0x17] & 0x40) >> 0x03);
    ((uint8_t *)dest)[0x1A] = ((((uint8_t *)src)[0x05] & 0x20) >> 0x05) | ((((uint8_t *)src)[0x0B] & 0x20) >> 0x04) | ((((uint8_t *)src)[0x11] & 0x20) >> 0x03) | ((((uint8_t *)src)[0x17] & 0x20) >> 0x02);
    ((uint8_t *)dest)[0x1B] = ((((uint8_t *)src)[0x05] & 0x10) >> 0x04) | ((((uint8_t *)src)[0x0B] & 0x10) >> 0x03) | ((((uint8_t *)src)[0x11] & 0x10) >> 0x02) | ((((uint8_t *)src)[0x17] & 0x10) >> 0x01);
    ((uint8_t *)dest)[0x1C] = ((((uint8_t *)src)[0x05] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x0B] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x11] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x17] & 0x08) >> 0x00);
    ((uint8_t *)dest)[0x1D] = ((((uint8_t *)src)[0x05] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x0B] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x11] & 0x04) >> 0x00) | ((((uint8_t *)src)[0x17] & 0x04) << 0x01);
    ((uint8_t *)dest)[0x1E] = ((((uint8_t *)src)[0x05] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x0B] & 0x02) >> 0x00) | ((((uint8_t *)src)[0x11] & 0x02) << 0x01) | ((((uint8_t *)src)[0x17] & 0x02) << 0x02);
    ((uint8_t *)dest)[0x1F] = ((((uint8_t *)src)[0x05] & 0x01) >> 0x00) | ((((uint8_t *)src)[0x0B] & 0x01) << 0x01) | ((((uint8_t *)src)[0x11] & 0x01) << 0x02) | ((((uint8_t *)src)[0x17] & 0x01) << 0x03);
    ((uint8_t *)dest)[0x20] = ((((uint8_t *)src)[0x04] & 0x80) >> 0x07) | ((((uint8_t *)src)[0x0A] & 0x80) >> 0x06) | ((((uint8_t *)src)[0x10] & 0x80) >> 0x05) | ((((uint8_t *)src)[0x16] & 0x80) >> 0x04);
    ((uint8_t *)dest)[0x21] = ((((uint8_t *)src)[0x04] & 0x40) >> 0x06) | ((((uint8_t *)src)[0x0A] & 0x40) >> 0x05) | ((((uint8_t *)src)[0x10] & 0x40) >> 0x04) | ((((uint8_t *)src)[0x16] & 0x40) >> 0x03);
    ((uint8_t *)dest)[0x22] = ((((uint8_t *)src)[0x04] & 0x20) >> 0x05) | ((((uint8_t *)src)[0x0A] & 0x20) >> 0x04) | ((((uint8_t *)src)[0x10] & 0x20) >> 0x03) | ((((uint8_t *)src)[0x16] & 0x20) >> 0x02);
    ((uint8_t *)dest)[0x23] = ((((uint8_t *)src)[0x04] & 0x10) >> 0x04) | ((((uint8_t *)src)[0x0A] & 0x10) >> 0x03) | ((((uint8_t *)src)[0x10] & 0x10) >> 0x02) | ((((uint8_t *)src)[0x16] & 0x10) >> 0x01);
    ((uint8_t *)dest)[0x24] = ((((uint8_t *)src)[0x04] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x0A] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x10] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x16] & 0x08) >> 0x00);
    ((uint8_t *)dest)[0x25] = ((((uint8_t *)src)[0x04] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x0A] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x10] & 0x04) >> 0x00) | ((((uint8_t *)src)[0x16] & 0x04) << 0x01);
    ((uint8_t *)dest)[0x26] = ((((uint8_t *)src)[0x04] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x0A] & 0x02) >> 0x00) | ((((uint8_t *)src)[0x10] & 0x02) << 0x01) | ((((uint8_t *)src)[0x16] & 0x02) << 0x02);
    ((uint8_t *)dest)[0x27] = ((((uint8_t *)src)[0x04] & 0x01) >> 0x00) | ((((uint8_t *)src)[0x0A] & 0x01) << 0x01) | ((((uint8_t *)src)[0x10] & 0x01) << 0x02) | ((((uint8_t *)src)[0x16] & 0x01) << 0x03);
    ((uint8_t *)dest)[0x28] = ((((uint8_t *)src)[0x03] & 0x80) >> 0x07) | ((((uint8_t *)src)[0x09] & 0x80) >> 0x06) | ((((uint8_t *)src)[0x0F] & 0x80) >> 0x05) | ((((uint8_t *)src)[0x15] & 0x80) >> 0x04);
    ((uint8_t *)dest)[0x29] = ((((uint8_t *)src)[0x03] & 0x40) >> 0x06) | ((((uint8_t *)src)[0x09] & 0x40) >> 0x05) | ((((uint8_t *)src)[0x0F] & 0x40) >> 0x04) | ((((uint8_t *)src)[0x15] & 0x40) >> 0x03);
    ((uint8_t *)dest)[0x2A] = ((((uint8_t *)src)[0x03] & 0x20) >> 0x05) | ((((uint8_t *)src)[0x09] & 0x20) >> 0x04) | ((((uint8_t *)src)[0x0F] & 0x20) >> 0x03) | ((((uint8_t *)src)[0x15] & 0x20) >> 0x02);
    ((uint8_t *)dest)[0x2B] = ((((uint8_t *)src)[0x03] & 0x10) >> 0x04) | ((((uint8_t *)src)[0x09] & 0x10) >> 0x03) | ((((uint8_t *)src)[0x0F] & 0x10) >> 0x02) | ((((uint8_t *)src)[0x15] & 0x10) >> 0x01);
    ((uint8_t *)dest)[0x2C] = ((((uint8_t *)src)[0x03] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x09] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x0F] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x15] & 0x08) >> 0x00);
    ((uint8_t *)dest)[0x2D] = ((((uint8_t *)src)[0x03] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x09] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x0F] & 0x04) >> 0x00) | ((((uint8_t *)src)[0x15] & 0x04) << 0x01);
    ((uint8_t *)dest)[0x2E] = ((((uint8_t *)src)[0x03] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x09] & 0x02) >> 0x00) | ((((uint8_t *)src)[0x0F] & 0x02) << 0x01) | ((((uint8_t *)src)[0x15] & 0x02) << 0x02);
    ((uint8_t *)dest)[0x2F] = ((((uint8_t *)src)[0x03] & 0x01) >> 0x00) | ((((uint8_t *)src)[0x09] & 0x01) << 0x01) | ((((uint8_t *)src)[0x0F] & 0x01) << 0x02) | ((((uint8_t *)src)[0x15] & 0x01) << 0x03);
}

/* Takes 64 bytes, outputs 24 bytes */
void PloytecAudioDevice::ploytecPCMdecode(uint8_t *dest, uint8_t *src)
{
	// channel 1
	((uint8_t *)dest)[0x00] = ((((uint8_t *)src)[0x17] & 0x01) << 0x00) | ((((uint8_t *)src)[0x16] & 0x01) << 0x01) | ((((uint8_t *)src)[0x15] & 0x01) << 0x02) | ((((uint8_t *)src)[0x14] & 0x01) << 0x03) | ((((uint8_t *)src)[0x13] & 0x01) << 0x04) | ((((uint8_t *)src)[0x12] & 0x01) << 0x05) | ((((uint8_t *)src)[0x11] & 0x01) << 0x06) | ((((uint8_t *)src)[0x10] & 0x01) << 0x07);
	((uint8_t *)dest)[0x01] = ((((uint8_t *)src)[0x0F] & 0x01) << 0x00) | ((((uint8_t *)src)[0x0E] & 0x01) << 0x01) | ((((uint8_t *)src)[0x0D] & 0x01) << 0x02) | ((((uint8_t *)src)[0x0C] & 0x01) << 0x03) | ((((uint8_t *)src)[0x0B] & 0x01) << 0x04) | ((((uint8_t *)src)[0x0A] & 0x01) << 0x05) | ((((uint8_t *)src)[0x09] & 0x01) << 0x06) | ((((uint8_t *)src)[0x08] & 0x01) << 0x07);
	((uint8_t *)dest)[0x02] = ((((uint8_t *)src)[0x07] & 0x01) << 0x00) | ((((uint8_t *)src)[0x06] & 0x01) << 0x01) | ((((uint8_t *)src)[0x05] & 0x01) << 0x02) | ((((uint8_t *)src)[0x04] & 0x01) << 0x03) | ((((uint8_t *)src)[0x03] & 0x01) << 0x04) | ((((uint8_t *)src)[0x02] & 0x01) << 0x05) | ((((uint8_t *)src)[0x01] & 0x01) << 0x06) | ((((uint8_t *)src)[0x00] & 0x01) << 0x07);
	// channel 2
	((uint8_t *)dest)[0x03] = ((((uint8_t *)src)[0x37] & 0x01) << 0x00) | ((((uint8_t *)src)[0x36] & 0x01) << 0x01) | ((((uint8_t *)src)[0x35] & 0x01) << 0x02) | ((((uint8_t *)src)[0x34] & 0x01) << 0x03) | ((((uint8_t *)src)[0x33] & 0x01) << 0x04) | ((((uint8_t *)src)[0x32] & 0x01) << 0x05) | ((((uint8_t *)src)[0x31] & 0x01) << 0x06) | ((((uint8_t *)src)[0x30] & 0x01) << 0x07);
	((uint8_t *)dest)[0x04] = ((((uint8_t *)src)[0x2F] & 0x01) << 0x00) | ((((uint8_t *)src)[0x2E] & 0x01) << 0x01) | ((((uint8_t *)src)[0x2D] & 0x01) << 0x02) | ((((uint8_t *)src)[0x2C] & 0x01) << 0x03) | ((((uint8_t *)src)[0x2B] & 0x01) << 0x04) | ((((uint8_t *)src)[0x2A] & 0x01) << 0x05) | ((((uint8_t *)src)[0x29] & 0x01) << 0x06) | ((((uint8_t *)src)[0x28] & 0x01) << 0x07);
	((uint8_t *)dest)[0x05] = ((((uint8_t *)src)[0x27] & 0x01) << 0x00) | ((((uint8_t *)src)[0x26] & 0x01) << 0x01) | ((((uint8_t *)src)[0x25] & 0x01) << 0x02) | ((((uint8_t *)src)[0x24] & 0x01) << 0x03) | ((((uint8_t *)src)[0x23] & 0x01) << 0x04) | ((((uint8_t *)src)[0x22] & 0x01) << 0x05) | ((((uint8_t *)src)[0x21] & 0x01) << 0x06) | ((((uint8_t *)src)[0x20] & 0x01) << 0x07);
	// channel 3
	((uint8_t *)dest)[0x06] = ((((uint8_t *)src)[0x17] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x16] & 0x02) << 0x00) | ((((uint8_t *)src)[0x15] & 0x02) << 0x01) | ((((uint8_t *)src)[0x14] & 0x02) << 0x02) | ((((uint8_t *)src)[0x13] & 0x02) << 0x03) | ((((uint8_t *)src)[0x12] & 0x02) << 0x04) | ((((uint8_t *)src)[0x11] & 0x02) << 0x05) | ((((uint8_t *)src)[0x10] & 0x02) << 0x06);
	((uint8_t *)dest)[0x07] = ((((uint8_t *)src)[0x0F] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x0E] & 0x02) << 0x00) | ((((uint8_t *)src)[0x0D] & 0x02) << 0x01) | ((((uint8_t *)src)[0x0C] & 0x02) << 0x02) | ((((uint8_t *)src)[0x0B] & 0x02) << 0x03) | ((((uint8_t *)src)[0x0A] & 0x02) << 0x04) | ((((uint8_t *)src)[0x09] & 0x02) << 0x05) | ((((uint8_t *)src)[0x08] & 0x02) << 0x06);
	((uint8_t *)dest)[0x08] = ((((uint8_t *)src)[0x07] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x06] & 0x02) << 0x00) | ((((uint8_t *)src)[0x05] & 0x02) << 0x01) | ((((uint8_t *)src)[0x04] & 0x02) << 0x02) | ((((uint8_t *)src)[0x03] & 0x02) << 0x03) | ((((uint8_t *)src)[0x02] & 0x02) << 0x04) | ((((uint8_t *)src)[0x01] & 0x02) << 0x05) | ((((uint8_t *)src)[0x00] & 0x02) << 0x06);
	// channel 4
	((uint8_t *)dest)[0x09] = ((((uint8_t *)src)[0x37] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x36] & 0x02) << 0x00) | ((((uint8_t *)src)[0x35] & 0x02) << 0x01) | ((((uint8_t *)src)[0x34] & 0x02) << 0x02) | ((((uint8_t *)src)[0x33] & 0x02) << 0x03) | ((((uint8_t *)src)[0x32] & 0x02) << 0x04) | ((((uint8_t *)src)[0x31] & 0x02) << 0x05) | ((((uint8_t *)src)[0x30] & 0x02) << 0x06);
	((uint8_t *)dest)[0x0A] = ((((uint8_t *)src)[0x2F] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x2E] & 0x02) << 0x00) | ((((uint8_t *)src)[0x2D] & 0x02) << 0x01) | ((((uint8_t *)src)[0x2C] & 0x02) << 0x02) | ((((uint8_t *)src)[0x2B] & 0x02) << 0x03) | ((((uint8_t *)src)[0x2A] & 0x02) << 0x04) | ((((uint8_t *)src)[0x29] & 0x02) << 0x05) | ((((uint8_t *)src)[0x28] & 0x02) << 0x06);
	((uint8_t *)dest)[0x0B] = ((((uint8_t *)src)[0x27] & 0x02) >> 0x01) | ((((uint8_t *)src)[0x26] & 0x02) << 0x00) | ((((uint8_t *)src)[0x25] & 0x02) << 0x01) | ((((uint8_t *)src)[0x24] & 0x02) << 0x02) | ((((uint8_t *)src)[0x23] & 0x02) << 0x03) | ((((uint8_t *)src)[0x22] & 0x02) << 0x04) | ((((uint8_t *)src)[0x21] & 0x02) << 0x05) | ((((uint8_t *)src)[0x20] & 0x02) << 0x06);
	// channel 5
	((uint8_t *)dest)[0x0C] = ((((uint8_t *)src)[0x17] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x16] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x15] & 0x04) << 0x00) | ((((uint8_t *)src)[0x14] & 0x04) << 0x01) | ((((uint8_t *)src)[0x13] & 0x04) << 0x02) | ((((uint8_t *)src)[0x12] & 0x04) << 0x03) | ((((uint8_t *)src)[0x11] & 0x04) << 0x04) | ((((uint8_t *)src)[0x10] & 0x04) << 0x05);
	((uint8_t *)dest)[0x0D] = ((((uint8_t *)src)[0x0F] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x0E] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x0D] & 0x04) << 0x00) | ((((uint8_t *)src)[0x0C] & 0x04) << 0x01) | ((((uint8_t *)src)[0x0B] & 0x04) << 0x02) | ((((uint8_t *)src)[0x0A] & 0x04) << 0x03) | ((((uint8_t *)src)[0x09] & 0x04) << 0x04) | ((((uint8_t *)src)[0x08] & 0x04) << 0x05);
	((uint8_t *)dest)[0x0E] = ((((uint8_t *)src)[0x07] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x06] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x05] & 0x04) << 0x00) | ((((uint8_t *)src)[0x04] & 0x04) << 0x01) | ((((uint8_t *)src)[0x03] & 0x04) << 0x02) | ((((uint8_t *)src)[0x02] & 0x04) << 0x03) | ((((uint8_t *)src)[0x01] & 0x04) << 0x04) | ((((uint8_t *)src)[0x00] & 0x04) << 0x05);
	// channel 6
	((uint8_t *)dest)[0x0F] = ((((uint8_t *)src)[0x37] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x36] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x35] & 0x04) << 0x00) | ((((uint8_t *)src)[0x34] & 0x04) << 0x01) | ((((uint8_t *)src)[0x33] & 0x04) << 0x02) | ((((uint8_t *)src)[0x32] & 0x04) << 0x03) | ((((uint8_t *)src)[0x31] & 0x04) << 0x04) | ((((uint8_t *)src)[0x30] & 0x04) << 0x05);
	((uint8_t *)dest)[0x10] = ((((uint8_t *)src)[0x2F] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x2E] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x2D] & 0x04) << 0x00) | ((((uint8_t *)src)[0x2C] & 0x04) << 0x01) | ((((uint8_t *)src)[0x2B] & 0x04) << 0x02) | ((((uint8_t *)src)[0x2A] & 0x04) << 0x03) | ((((uint8_t *)src)[0x29] & 0x04) << 0x04) | ((((uint8_t *)src)[0x28] & 0x04) << 0x05);
	((uint8_t *)dest)[0x11] = ((((uint8_t *)src)[0x27] & 0x04) >> 0x02) | ((((uint8_t *)src)[0x26] & 0x04) >> 0x01) | ((((uint8_t *)src)[0x25] & 0x04) << 0x00) | ((((uint8_t *)src)[0x24] & 0x04) << 0x01) | ((((uint8_t *)src)[0x23] & 0x04) << 0x02) | ((((uint8_t *)src)[0x22] & 0x04) << 0x03) | ((((uint8_t *)src)[0x21] & 0x04) << 0x04) | ((((uint8_t *)src)[0x20] & 0x04) << 0x05);
	// channel 7
	((uint8_t *)dest)[0x12] = ((((uint8_t *)src)[0x17] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x16] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x15] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x14] & 0x08) << 0x00) | ((((uint8_t *)src)[0x13] & 0x08) << 0x01) | ((((uint8_t *)src)[0x12] & 0x08) << 0x02) | ((((uint8_t *)src)[0x11] & 0x08) << 0x03) | ((((uint8_t *)src)[0x10] & 0x08) << 0x04);
	((uint8_t *)dest)[0x13] = ((((uint8_t *)src)[0x0F] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x0E] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x0D] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x0C] & 0x08) << 0x00) | ((((uint8_t *)src)[0x0B] & 0x08) << 0x01) | ((((uint8_t *)src)[0x0A] & 0x08) << 0x02) | ((((uint8_t *)src)[0x09] & 0x08) << 0x03) | ((((uint8_t *)src)[0x08] & 0x08) << 0x04);
	((uint8_t *)dest)[0x14] = ((((uint8_t *)src)[0x07] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x06] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x05] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x04] & 0x08) << 0x00) | ((((uint8_t *)src)[0x03] & 0x08) << 0x01) | ((((uint8_t *)src)[0x02] & 0x08) << 0x02) | ((((uint8_t *)src)[0x01] & 0x08) << 0x03) | ((((uint8_t *)src)[0x00] & 0x08) << 0x04);
	// channel 8
	((uint8_t *)dest)[0x15] = ((((uint8_t *)src)[0x37] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x36] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x35] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x34] & 0x08) << 0x00) | ((((uint8_t *)src)[0x33] & 0x08) << 0x01) | ((((uint8_t *)src)[0x32] & 0x08) << 0x02) | ((((uint8_t *)src)[0x31] & 0x08) << 0x03) | ((((uint8_t *)src)[0x30] & 0x08) << 0x04);
	((uint8_t *)dest)[0x16] = ((((uint8_t *)src)[0x2F] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x2E] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x2D] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x2C] & 0x08) << 0x00) | ((((uint8_t *)src)[0x2B] & 0x08) << 0x01) | ((((uint8_t *)src)[0x2A] & 0x08) << 0x02) | ((((uint8_t *)src)[0x29] & 0x08) << 0x03) | ((((uint8_t *)src)[0x28] & 0x08) << 0x04);
	((uint8_t *)dest)[0x17] = ((((uint8_t *)src)[0x27] & 0x08) >> 0x03) | ((((uint8_t *)src)[0x26] & 0x08) >> 0x02) | ((((uint8_t *)src)[0x25] & 0x08) >> 0x01) | ((((uint8_t *)src)[0x24] & 0x08) << 0x00) | ((((uint8_t *)src)[0x23] & 0x08) << 0x01) | ((((uint8_t *)src)[0x22] & 0x08) << 0x02) | ((((uint8_t *)src)[0x21] & 0x08) << 0x03) | ((((uint8_t *)src)[0x20] & 0x08) << 0x04);
}
