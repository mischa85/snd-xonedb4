//
//  AudioDevice.cpp
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include "AudioDevice.h"

#include <AudioDriverKit/AudioDriverKit.h>

// Number of audio frames to buffer in the driver.
// Also sets the clocking.
// Should be divisible between both 10 and 8.
// Could theoratically be set as low as 40, but I've seen CoreAudio crap itself at those values.
// Beware.
constexpr uint16_t bufferframes = 640;

constexpr uint8_t bytesperframe = 3;
constexpr uint8_t numchannels = 8;

constexpr uint8_t s24_3leframe = bytesperframe * numchannels;

constexpr uint8_t numpacketsout = 8;
constexpr uint8_t numpacketsin = 10;

constexpr uint8_t framesperpacketout = 10;
constexpr uint16_t totalframesout = numpacketsout * framesperpacketout;
constexpr uint8_t framesperpacketin = 8;
constexpr uint16_t totalframesin = numpacketsin * framesperpacketin;

constexpr uint16_t pcmsizeout = numpacketsout * 512; // 80 frames
constexpr uint16_t pcmsizein = numpacketsin * 512; // 80 frames

constexpr uint8_t pcmouturbs = 1;
constexpr uint8_t pcminurbs = 1;

constexpr uint16_t xonedb4framesizeout = 48;
constexpr uint16_t xonedb4framesizein = 64;

struct AudioDevice_IVars
{
	IOUSBHostDevice						*device;

	OSSharedPtr<IOUserAudioDriver>		m_driver;
	OSSharedPtr<IODispatchQueue>		m_work_queue;
	IOUserAudioStreamBasicDescription	m_stream_format;
	OSSharedPtr<IOUserAudioStream>		m_output_stream;
	OSSharedPtr<IOUserAudioStream>		m_input_stream;
	OSSharedPtr<IOMemoryMap>			m_output_memory_map;
	OSSharedPtr<IOMemoryMap>			m_input_memory_map;

	IOUSBHostPipe*						PCMinPipe;
	IOBufferMemoryDescriptor*			PCMinData;
	OSAction*							PCMinCallback;
	IOUSBHostPipe*						PCMoutPipe;
	
	IOBufferMemoryDescriptor*			PCMinDataEmpty;
	IOBufferMemoryDescriptor*			PCMoutDataEmpty;
	uint8_t*							PCMoutDataEmptyAddr;
	IOBufferMemoryDescriptor*			PCMoutData;
	uint8_t*							PCMoutDataAddr;
	uint8_t*							PCMinDataAddr;
	OSAction*							PCMoutCallback;
	uint16_t							PCMPacketSize;
	uint16_t							currentsampleoutusb;
	
	uint8_t*							CoreAudioOutputBufferAddr;
	uint8_t*							CoreAudioInputBufferAddr;
	
	uint64_t							current_sample_time;
	uint64_t							out_sample_time;
	uint64_t							in_sample_time;
	uint64_t							in_sample_time_usb;
	uint64_t							out_sample_time_usb;

	bool								startclock;
	bool								startpcmin;
	bool								startpcmout;
};

bool AudioDevice::init(IOUserAudioDriver* in_driver, bool in_supports_prewarming, OSString* in_device_uid, OSString* in_model_uid, OSString* in_manufacturer_uid, uint32_t in_zero_timestamp_period, IOUSBHostPipe* PCMinPipe, OSAction* PCMinCallback, IOUSBHostPipe* PCMoutPipe, OSAction* PCMoutCallback, uint16_t PCMPacketSize, IOUSBHostDevice* device)
{
	auto success = super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period);
	if (!success) {
		return false;
	}
	ivars = IONewZero(AudioDevice_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}

	auto device_uid = OSSharedPtr(OSString::withCString("xonedb4"), OSNoRetain);
	auto model_uid = OSSharedPtr(OSString::withCString("Model UID"), OSNoRetain);

	int i = 0;

	IOReturn ret = kIOReturnSuccess;
	IOAddressSegment range;

	ivars->m_driver = OSSharedPtr(in_driver, OSRetain);
	ivars->m_work_queue = GetWorkQueue();
	ivars->PCMinPipe = PCMinPipe;
	ivars->PCMinCallback = PCMinCallback;
	ivars->PCMoutPipe = PCMoutPipe;
	ivars->PCMoutCallback = PCMoutCallback;
	ivars->PCMPacketSize = PCMPacketSize;
	ivars->device = device;
	
	ivars->startpcmin = false;
	ivars->startpcmout = false;
	
	ret = ivars->device->CreateIOBuffer(kIOMemoryDirectionInOut, pcmsizein, &ivars->PCMinDataEmpty);
	ret = ivars->device->CreateIOBuffer(kIOMemoryDirectionInOut, pcmsizeout, &ivars->PCMoutDataEmpty);
	ivars->PCMoutDataEmpty->GetAddressRange(&range);
	ivars->PCMoutDataEmptyAddr = reinterpret_cast<uint8_t*>(range.address);
	ret = ivars->device->CreateIOBuffer(kIOMemoryDirectionInOut, pcmsizein, &ivars->PCMinData);
	ivars->PCMinData->GetAddressRange(&range);
	ivars->PCMinDataAddr = reinterpret_cast<uint8_t*>(range.address);
	ret = ivars->device->CreateIOBuffer(kIOMemoryDirectionInOut, pcmsizeout, &ivars->PCMoutData);
	ivars->PCMoutData->GetAddressRange(&range);
	ivars->PCMoutDataAddr = reinterpret_cast<uint8_t*>(range.address);

	double sample_rates[] = {96000};
	const auto buffer_size_bytes = bufferframes * s24_3leframe;

	OSSharedPtr<IOBufferMemoryDescriptor> output_io_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> input_io_ring_buffer;

	OSSharedPtr<OSString> output_stream_name = OSSharedPtr(OSString::withCString("Xone:DB4 PCM OUT"), OSNoRetain);
	OSSharedPtr<OSString> input_stream_name = OSSharedPtr(OSString::withCString("Xone:DB4 PCM IN"), OSNoRetain);

	IOUserAudioChannelLabel output_channel_layout[numchannels] = { IOUserAudioChannelLabel::Discrete_0, IOUserAudioChannelLabel::Discrete_1, IOUserAudioChannelLabel::Discrete_2, IOUserAudioChannelLabel::Discrete_3, IOUserAudioChannelLabel::Discrete_4, IOUserAudioChannelLabel::Discrete_5, IOUserAudioChannelLabel::Discrete_6, IOUserAudioChannelLabel::Discrete_7 };
	IOUserAudioChannelLabel input_channel_layout[numchannels] = { IOUserAudioChannelLabel::Discrete_0, IOUserAudioChannelLabel::Discrete_1, IOUserAudioChannelLabel::Discrete_2, IOUserAudioChannelLabel::Discrete_3, IOUserAudioChannelLabel::Discrete_4, IOUserAudioChannelLabel::Discrete_5, IOUserAudioChannelLabel::Discrete_6, IOUserAudioChannelLabel::Discrete_7 };

	IOUserAudioStreamBasicDescription stream_formats[] =
	{
		{
			// S24_3LE
			96000, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(s24_3leframe),
			1,
			static_cast<uint32_t>(s24_3leframe),
			static_cast<uint32_t>(numchannels),
			24
		},
	};

	SetAvailableSampleRates(sample_rates, 1);
	SetSampleRate(96000);
	
	SetZeroTimeStampPeriod(bufferframes);
	
	for (i = 0; i < pcminurbs; i++)  {
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinDataEmpty, pcmsizein, ivars->PCMinCallback, 0);
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to send PCMinData urbs");
	}

	for (i = 0; i < pcmouturbs; i++)  {
		ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, pcmsizeout, ivars->PCMoutCallback, 0);
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to send PCMoutData urbs");
	}
	
	os_log(OS_LOG_DEFAULT, "and here?");

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, buffer_size_bytes, 0, output_io_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create output IOBufferMemoryDescriptor");
	
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, buffer_size_bytes, 0, input_io_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create input IOBufferMemoryDescriptor");

	ivars->m_output_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, output_io_ring_buffer.get());
	FailIfNULL(ivars->m_output_stream.get(), ret = kIOReturnNoMemory, Failure, "failed to create output stream");
	
	ivars->m_input_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Input, input_io_ring_buffer.get());
	FailIfNULL(ivars->m_input_stream.get(), ret = kIOReturnNoMemory, Failure, "failed to create input stream");

	ivars->m_output_stream->SetName(output_stream_name.get());
	ivars->m_output_stream->SetAvailableStreamFormats(stream_formats, 1);
	ivars->m_stream_format = stream_formats[0];
	ivars->m_output_stream->SetCurrentStreamFormat(&ivars->m_stream_format);
	
	ivars->m_input_stream->SetName(input_stream_name.get());
	ivars->m_input_stream->SetAvailableStreamFormats(stream_formats, 1);
	ivars->m_input_stream->SetCurrentStreamFormat(&ivars->m_stream_format);

	ret = AddStream(ivars->m_output_stream.get());
	FailIfError(ret, , Failure, "failed to add output stream");
	
	ret = AddStream(ivars->m_input_stream.get());
	FailIfError(ret, , Failure, "failed to add input stream");

	SetPreferredOutputChannelLayout(output_channel_layout, numchannels);
	SetTransportType(IOUserAudioTransportType::USB);
	SetPreferredInputChannelLayout(input_channel_layout, numchannels);
	SetTransportType(IOUserAudioTransportType::USB);
	
	for(i = 0; i < numpacketsout; i++) {
		ploytec_sync_bytes(ivars->PCMoutDataAddr + 480 + (i * PCMPacketSize));
		ploytec_sync_bytes(ivars->PCMoutDataEmptyAddr + 480 + (i * PCMPacketSize));
	}
	
	return true;

Failure:
	ivars->m_driver.reset();
	ivars->m_output_stream.reset();
	ivars->m_input_stream.reset();
	ivars->m_output_memory_map.reset();
	ivars->m_input_memory_map.reset();
	return false;
}

kern_return_t AudioDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
	os_log(OS_LOG_DEFAULT, "is startio being called???");
	
	__block kern_return_t ret = kIOReturnSuccess;
	__block OSSharedPtr<IOMemoryDescriptor> output_iomd;
	__block OSSharedPtr<IOMemoryDescriptor> input_iomd;

	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StartIO(in_flags);
		FailIfError(ret, , Failure, "Failed to start I/O");

		output_iomd = ivars->m_output_stream->GetIOMemoryDescriptor();
		FailIfNULL(output_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get output stream IOMemoryDescriptor");
		ret = output_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_output_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from output stream IOMemoryDescriptor");
		
		input_iomd = ivars->m_input_stream->GetIOMemoryDescriptor();
		FailIfNULL(input_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get input stream IOMemoryDescriptor");
		ret = input_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_input_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from input stream IOMemoryDescriptor");
		
		ivars->CoreAudioOutputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_output_memory_map->GetAddress() + ivars->m_output_memory_map->GetOffset());
		ivars->CoreAudioInputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_input_memory_map->GetAddress() + ivars->m_input_memory_map->GetOffset());
		
		UpdateCurrentZeroTimestamp(0, 0);

		ivars->out_sample_time_usb = 0;
		ivars->in_sample_time_usb = 0;
		ivars->startclock = true;
		ivars->startpcmout = true;
		ivars->startpcmin = true;
		
		return;

	Failure:
		super::StopIO(in_flags);
		ivars->m_output_memory_map.reset();
		ivars->m_input_memory_map.reset();
		return;
	});

	return ret;
}

kern_return_t AudioDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret;

	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StopIO(in_flags);
		
		ivars->startclock = false;
		ivars->startpcmout = false;
		ivars->startpcmin = false;
		ivars->out_sample_time_usb = 0;
		ivars->in_sample_time_usb = 0;
	});

	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to stop IO, error %d", ret);
	}

	return ret;
}

void AudioDevice::free()
{
	if (ivars != nullptr) {
		ivars->m_driver.reset();
		ivars->m_output_stream.reset();
		ivars->m_output_memory_map.reset();
		ivars->m_input_stream.reset();
		ivars->m_input_memory_map.reset();
		ivars->m_work_queue.reset();
	}
	IOSafeDeleteNULL(ivars, AudioDevice_IVars, 1);
	super::free();
}

kern_return_t AudioDevice::SendPCMToDevice(uint64_t completionTimestamp)
{
	__block kern_return_t ret;
	__block int i = 0;
	
	if(ivars->startpcmout == true) {
		if(ivars->currentsampleoutusb == bufferframes) {
			ivars->currentsampleoutusb = 0;
			GetCurrentZeroTimestamp(&ivars->current_sample_time, nullptr);
			ivars->current_sample_time += bufferframes;
			UpdateCurrentZeroTimestamp(ivars->current_sample_time, completionTimestamp);
		}

		GetCurrentClientIOTime(0, &ivars->out_sample_time, nullptr);
		
		if((ivars->out_sample_time > 0) && (ivars->out_sample_time - ivars->out_sample_time_usb) > bufferframes)
		{
			ivars->out_sample_time_usb = ivars->out_sample_time - (bufferframes/2);
			os_log(OS_LOG_DEFAULT, "OUTSAMPLETIME: %llu | SAMPLEOUTUSB CORRECTED TO: %llu", ivars->out_sample_time, ivars->out_sample_time_usb);
		}
		
		for(i = 0; i < totalframesout; i++) {
			ploytec_convert_from_s24_3le(ivars->PCMoutDataAddr + ((i / framesperpacketout) * 512) + ((i % framesperpacketout) * xonedb4framesizeout), ivars->CoreAudioOutputBufferAddr + ((ivars->out_sample_time_usb % bufferframes) * s24_3leframe));
			ivars->out_sample_time_usb++;
			ivars->currentsampleoutusb++;
		}
		
	ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutData, pcmsizeout, ivars->PCMoutCallback, 0);
	if (ret != kIOReturnSuccess)
	{
		os_log(OS_LOG_DEFAULT, "SEND ERROR %d", ret);
	}
	} else {
		ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, pcmsizeout, ivars->PCMoutCallback, 0);
		if (ret != kIOReturnSuccess) {
			os_log(OS_LOG_DEFAULT, "SEND ERROR %d", ret);
		}
	}
	return ret;
}

kern_return_t AudioDevice::ReceivePCMfromDevice(uint64_t completionTimestamp)
{
	__block int i;
	__block kern_return_t ret;
	
	if(ivars->startpcmin == true) {
		GetCurrentClientIOTime(1, &ivars->in_sample_time, nullptr);
		
		if((ivars->in_sample_time > 0) && (ivars->in_sample_time_usb - ivars->in_sample_time) > bufferframes)
		{
			ivars->in_sample_time_usb = ivars->in_sample_time + (bufferframes/2);
			os_log(OS_LOG_DEFAULT, "INSAMPLETIME: %llu | SAMPLEINUSB CORRECTED TO: %llu", ivars->in_sample_time, ivars->in_sample_time_usb);
		}
			
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinData, pcmsizein, ivars->PCMinCallback, 0);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "RECEIVE ERROR %d", ret);
		}
		for(i = 0; i < totalframesin; i++) {
			ploytec_convert_to_s24_3le(ivars->CoreAudioInputBufferAddr + ((ivars->in_sample_time_usb % bufferframes) * s24_3leframe), ivars->PCMinDataAddr + (i * xonedb4framesizein));
			ivars->in_sample_time_usb++;
		}
	} else {
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinDataEmpty, pcmsizein, ivars->PCMinCallback, 0);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "RECEIVE ERROR %d", ret);
		}
	}

	return ret;
}

/* Takes 24 bytes, outputs 48 bytes */
void AudioDevice::ploytec_convert_from_s24_3le(uint8_t *dest, uint8_t *src)
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
void AudioDevice::ploytec_convert_to_s24_3le(uint8_t *dest, uint8_t *src)
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

/* outputs 0xFD 0xFF 000........ */
void AudioDevice::ploytec_sync_bytes(uint8_t *dest)
{
    ((uint8_t *)dest)[0x00] = 0xFD;
    ((uint8_t *)dest)[0x01] = 0xFF;
    ((uint8_t *)dest)[0x02] = 0x00;
    ((uint8_t *)dest)[0x03] = 0x00;
    ((uint8_t *)dest)[0x04] = 0x00;
    ((uint8_t *)dest)[0x05] = 0x00;
    ((uint8_t *)dest)[0x06] = 0x00;
    ((uint8_t *)dest)[0x07] = 0x00;
    ((uint8_t *)dest)[0x08] = 0x00;
    ((uint8_t *)dest)[0x09] = 0x00;
    ((uint8_t *)dest)[0x0A] = 0x00;
    ((uint8_t *)dest)[0x0B] = 0x00;
    ((uint8_t *)dest)[0x0C] = 0x00;
    ((uint8_t *)dest)[0x0D] = 0x00;
    ((uint8_t *)dest)[0x0E] = 0x00;
    ((uint8_t *)dest)[0x0F] = 0x00;
    ((uint8_t *)dest)[0x10] = 0x00;
    ((uint8_t *)dest)[0x11] = 0x00;
    ((uint8_t *)dest)[0x12] = 0x00;
    ((uint8_t *)dest)[0x13] = 0x00;
    ((uint8_t *)dest)[0x14] = 0x00;
    ((uint8_t *)dest)[0x15] = 0x00;
    ((uint8_t *)dest)[0x16] = 0x00;
    ((uint8_t *)dest)[0x17] = 0x00;
    ((uint8_t *)dest)[0x18] = 0x00;
    ((uint8_t *)dest)[0x19] = 0x00;
    ((uint8_t *)dest)[0x1A] = 0x00;
    ((uint8_t *)dest)[0x1B] = 0x00;
    ((uint8_t *)dest)[0x1C] = 0x00;
    ((uint8_t *)dest)[0x1D] = 0x00;
    ((uint8_t *)dest)[0x1E] = 0x00;
    ((uint8_t *)dest)[0x1F] = 0x00;
}
