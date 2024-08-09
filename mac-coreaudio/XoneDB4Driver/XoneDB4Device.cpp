//
//  XoneDB4Device.cpp
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "XoneDB4Device.h"
#include "XoneDB4Driver.h"

// Number of audio frames to buffer in the driver.
// Also sets the clocking.
// Should be divisible between both 10 and 8.
// Could theoratically be set as low as 40, but I've seen CoreAudio crap itself at those values.
// Beware.
constexpr uint32_t buffersize = 2560;

// This is the size the buffer has at time of allocation, it can shrink according to settings
constexpr uint32_t maxbuffersize = 800000;

constexpr uint8_t bytesperframe = 3;
constexpr uint8_t numchannels = 8;

constexpr uint8_t s24_3leframe = bytesperframe * numchannels;

constexpr uint8_t numpacketsout = 4;
constexpr uint8_t numpacketsin = 4;

constexpr uint8_t framesperpacketout = 10;
constexpr uint16_t totalframesout = numpacketsout * framesperpacketout;
constexpr uint8_t framesperpacketin = 8;
constexpr uint16_t totalframesin = numpacketsin * framesperpacketin;

constexpr uint16_t pcmsizeout = numpacketsout * 482; // 40 frames
constexpr uint16_t pcmsizein = numpacketsin * 512; // 32 frames

constexpr uint8_t pcmouturbs = 8;
constexpr uint8_t pcminurbs = 8;

constexpr uint16_t xonedb4framesizeout = 48;
constexpr uint16_t xonedb4framesizein = 64;

struct XoneDB4Device_IVars
{
	OSSharedPtr<IOUserAudioDriver>			m_driver;
	OSSharedPtr<IODispatchQueue>			m_work_queue;
	IOUserAudioStreamBasicDescription		m_stream_format;
	OSSharedPtr<IOUserAudioStream>			m_output_stream;
	OSSharedPtr<IOUserAudioStream>			m_input_stream;
	OSSharedPtr<IOMemoryMap>				m_output_memory_map;
	OSSharedPtr<IOMemoryMap>				m_output_ploytec_memory_map;
	OSSharedPtr<IOMemoryMap>				m_input_memory_map;
	OSSharedPtr<IOMemoryMap>				m_input_ploytec_memory_map;

	IOUSBHostPipe*							PCMinPipe;
	OSAction*								PCMinCallback;
	IOUSBHostPipe*							PCMoutPipe;
	OSAction*								PCMoutCallback;
	
	IOBufferMemoryDescriptor*				PCMinDataEmpty;
	IOBufferMemoryDescriptor*				PCMoutDataEmpty;
	
	uint32_t								buffersize;
	
	OSSharedPtr<IOBufferMemoryDescriptor> 	output_io_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> 	output_ploytec_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> 	input_io_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> 	input_ploytec_ring_buffer;
	OSSharedPtr<IOMemoryDescriptor>			PCMoutData[maxbuffersize/totalframesout];
	OSSharedPtr<IOMemoryDescriptor>			PCMinData[maxbuffersize/totalframesin];
	
	uint8_t*								PCMoutDataAddr[maxbuffersize/totalframesout];
	uint8_t*								CoreAudioOutputBufferAddr;
	uint8_t*								CoreAudioInputBufferAddr;
	uint8_t*								PloytecOutputBufferAddr;
	uint8_t*								PloytecInputBufferAddr;
	
	uint64_t								in_sample_time_usb;
	uint64_t								out_sample_time_usb;
	uint64_t								out_hw_sample_time_usb;
	uint16_t								out_current_buffer_pos_usb;
	
	uint64_t								xruns;

	bool									startpcmin;
	bool									startpcmout;
};

bool XoneDB4Device::init(IOUserAudioDriver* in_driver, bool in_supports_prewarming, OSString* in_device_uid, OSString* in_model_uid, OSString* in_manufacturer_uid, uint32_t in_zero_timestamp_period, IOUSBHostPipe* PCMinPipe, OSAction* PCMinCallback, IOUSBHostPipe* PCMoutPipe, OSAction* PCMoutCallback, IOUSBHostDevice* device)
{
	auto success = super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period);
	if (!success) {
		return false;
	}
	ivars = IONewZero(XoneDB4Device_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}
	
	ivars->buffersize = buffersize;

	auto device_uid = OSSharedPtr(OSString::withCString("xonedb4"), OSNoRetain);
	auto model_uid = OSSharedPtr(OSString::withCString("Model UID"), OSNoRetain);

	int i = 0;

	IOReturn ret = kIOReturnSuccess;
	IOAddressSegment range;
	IOOperationHandler io_operation = nullptr;
	uint8_t* PCMoutDataEmptyAddr;
	double sample_rates[] = {96000};
	
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
	
	IOUserAudioChannelLabel output_channel_layout[numchannels] = { IOUserAudioChannelLabel::Discrete_0, IOUserAudioChannelLabel::Discrete_1, IOUserAudioChannelLabel::Discrete_2, IOUserAudioChannelLabel::Discrete_3, IOUserAudioChannelLabel::Discrete_4, IOUserAudioChannelLabel::Discrete_5, IOUserAudioChannelLabel::Discrete_6, IOUserAudioChannelLabel::Discrete_7 };
	IOUserAudioChannelLabel input_channel_layout[numchannels] = { IOUserAudioChannelLabel::Discrete_0, IOUserAudioChannelLabel::Discrete_1, IOUserAudioChannelLabel::Discrete_2, IOUserAudioChannelLabel::Discrete_3, IOUserAudioChannelLabel::Discrete_4, IOUserAudioChannelLabel::Discrete_5, IOUserAudioChannelLabel::Discrete_6, IOUserAudioChannelLabel::Discrete_7 };
	
	OSSharedPtr<OSString> output_stream_name = OSSharedPtr(OSString::withCString("Xone:DB4 PCM OUT"), OSNoRetain);
	OSSharedPtr<OSString> input_stream_name = OSSharedPtr(OSString::withCString("Xone:DB4 PCM IN"), OSNoRetain);

	ivars->m_driver = OSSharedPtr(in_driver, OSRetain);
	ivars->m_work_queue = GetWorkQueue();
	ivars->PCMinPipe = PCMinPipe;
	ivars->PCMinCallback = PCMinCallback;
	ivars->PCMoutPipe = PCMoutPipe;
	ivars->PCMoutCallback = PCMoutCallback;
	
	ivars->startpcmin = false;
	ivars->startpcmout = false;
	
	ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, pcmsizein, &ivars->PCMinDataEmpty);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create output IOBufferMemoryDescriptor");
	ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, pcmsizeout, &ivars->PCMoutDataEmpty);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create output IOBufferMemoryDescriptor");
	
	ivars->PCMoutDataEmpty->GetAddressRange(&range);
	PCMoutDataEmptyAddr = reinterpret_cast<uint8_t*>(range.address);
	
	// UART
	PCMoutDataEmptyAddr[432] = 0xfd;
	PCMoutDataEmptyAddr[433] = 0xfd;
	PCMoutDataEmptyAddr[914] = 0xfd;
	PCMoutDataEmptyAddr[915] = 0xfd;
	PCMoutDataEmptyAddr[1396] = 0xfd;
	PCMoutDataEmptyAddr[1397] = 0xfd;
	PCMoutDataEmptyAddr[1878] = 0xfd;
	PCMoutDataEmptyAddr[1879] = 0xfd;
	
	ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, maxbuffersize * xonedb4framesizeout, ivars->output_ploytec_ring_buffer.attach());
	ivars->output_ploytec_ring_buffer->GetAddressRange(&range);
	ivars->PloytecOutputBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	 
	ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, maxbuffersize * xonedb4framesizein, ivars->input_ploytec_ring_buffer.attach());
	ivars->input_ploytec_ring_buffer->GetAddressRange(&range);
	ivars->PloytecInputBufferAddr = reinterpret_cast<uint8_t*>(range.address);

	SetAvailableSampleRates(sample_rates, 1);
	SetSampleRate(96000);
	
	for (i = 0; i < pcmouturbs; i++) {
		ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, pcmsizeout, ivars->PCMoutCallback, 0);
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to send PCMoutData urbs");
	}
	
	for (i = 0; i < pcminurbs; i++) {
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinDataEmpty, pcmsizein, ivars->PCMinCallback, 0);
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to send PCMinData urbs");
	}

	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, maxbuffersize * s24_3leframe, 0, ivars->output_io_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create output IOBufferMemoryDescriptor");
	
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, maxbuffersize * s24_3leframe, 0, ivars->input_io_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create input IOBufferMemoryDescriptor");

	ivars->m_output_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, ivars->output_io_ring_buffer.get());
	FailIfNULL(ivars->m_output_stream.get(), ret = kIOReturnNoMemory, Failure, "failed to create output stream");
	
	ivars->m_input_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Input, ivars->input_io_ring_buffer.get());
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
	
	io_operation = ^kern_return_t(IOUserAudioObjectID in_device, IOUserAudioIOOperation in_io_operation, uint32_t in_io_buffer_frame_size, uint64_t in_sample_time, uint64_t in_host_time)
	{
		__block int i = 0;
		
		if (in_io_operation == IOUserAudioIOOperationWriteEnd)
		{
			if(ivars->out_sample_time_usb < (in_sample_time - (ivars->buffersize - in_io_buffer_frame_size)) || ivars->out_sample_time_usb > in_sample_time)
			{
				ivars->xruns++;
				os_log(OS_LOG_DEFAULT, "RESYNC OUT");
				ivars->out_sample_time_usb = in_sample_time - (ivars->buffersize/2);
			}
			
			for (i = 0; i < in_io_buffer_frame_size; i++)
			{
				int sampleoffset = ((in_sample_time + i) % ivars->buffersize);
				int byteoffset = 0;
				
				if (sampleoffset >= 9) {
					byteoffset = ((sampleoffset - 9) / 10) * 2 + 2;
				}
				
				ploytec_convert_from_s24_3le(ivars->PloytecOutputBufferAddr + byteoffset + (sampleoffset * xonedb4framesizeout), ivars->CoreAudioOutputBufferAddr + (sampleoffset * s24_3leframe));
			}
		}
		else if (in_io_operation == IOUserAudioIOOperationBeginRead)
		{
			if(ivars->in_sample_time_usb > (in_sample_time + (ivars->buffersize - in_io_buffer_frame_size)) || ivars->in_sample_time_usb < in_sample_time)
			{
				ivars->xruns++;
				os_log(OS_LOG_DEFAULT, "RESYNC IN");
				ivars->in_sample_time_usb = in_sample_time + (ivars->buffersize/2);
			}
			
			for (i = 0; i < in_io_buffer_frame_size; i++)
			{
				ploytec_convert_to_s24_3le(ivars->CoreAudioInputBufferAddr + (((in_sample_time + i) % ivars->buffersize) * s24_3leframe), ivars->PloytecInputBufferAddr + (((in_sample_time + i) % ivars->buffersize) * xonedb4framesizein));
			}
		}
		
		return kIOReturnSuccess;
	};

	this->SetIOOperationHandler(io_operation);
	
	return true;

Failure:
	ivars->m_driver.reset();
	ivars->m_output_stream.reset();
	ivars->m_input_stream.reset();
	ivars->m_output_memory_map.reset();
	ivars->m_output_ploytec_memory_map.reset();
	ivars->m_input_memory_map.reset();
	ivars->m_input_ploytec_memory_map.reset();
	return false;
}

kern_return_t XoneDB4Device::StartIO(IOUserAudioStartStopFlags in_flags)
{
	__block int i = 0;
	__block kern_return_t ret = kIOReturnSuccess;
	__block OSSharedPtr<IOMemoryDescriptor> output_iomd;
	__block OSSharedPtr<IOMemoryDescriptor> output_ploytec_iomd;
	__block OSSharedPtr<IOMemoryDescriptor> input_iomd;
	__block OSSharedPtr<IOMemoryDescriptor> input_ploytec_iomd;
	__block uint64_t length = 0;
	
	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StartIO(in_flags);
		FailIfError(ret, , Failure, "Failed to start I/O");

		output_iomd = ivars->m_output_stream->GetIOMemoryDescriptor();
		FailIfNULL(output_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get output stream IOMemoryDescriptor");
		ret = output_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_output_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from output stream IOMemoryDescriptor");

		ivars->CoreAudioOutputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_output_memory_map->GetAddress() + ivars->m_output_memory_map->GetOffset());
		
		output_ploytec_iomd = ivars->output_ploytec_ring_buffer;
		FailIfNULL(output_ploytec_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get output ploytec IOMemoryDescriptor");
		ret = output_ploytec_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_output_ploytec_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from output ploytec IOMemoryDescriptor");
		
		for (i = 0; i < (ivars->buffersize/totalframesout); i++) {
			ret = output_ploytec_iomd->CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * pcmsizeout, pcmsizeout, output_ploytec_iomd.get(), ivars->PCMoutData[i].attach());
			FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create output SubMemoryDescriptor");
			ivars->PCMoutDataAddr[i] = ivars->PloytecOutputBufferAddr + (i * pcmsizeout);
		}
		
		input_iomd = ivars->m_input_stream->GetIOMemoryDescriptor();
		FailIfNULL(input_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get input stream IOMemoryDescriptor");
		ret = input_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_input_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from input stream IOMemoryDescriptor");

		ivars->CoreAudioInputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_input_memory_map->GetAddress() + ivars->m_input_memory_map->GetOffset());
		
		input_ploytec_iomd = ivars->input_ploytec_ring_buffer;
		FailIfNULL(input_ploytec_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get output ploytec IOMemoryDescriptor");
		ret = input_ploytec_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_input_ploytec_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from output ploytec IOMemoryDescriptor");
		
		for (i = 0; i < (ivars->buffersize/totalframesin); i++) {
			ret = input_ploytec_iomd->CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * pcmsizein, pcmsizein, input_ploytec_iomd.get(), ivars->PCMinData[i].attach());
			FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create input SubMemoryDescriptor");
		}
		
		UpdateCurrentZeroTimestamp(0, 0);

		ivars->out_sample_time_usb = 0;
		ivars->in_sample_time_usb = 0;
			
		ivars->startpcmout = true;
		ivars->startpcmin = true;
		
		return;

	Failure:
		super::StopIO(in_flags);
		ivars->m_output_memory_map.reset();
		ivars->m_output_ploytec_memory_map.reset();
		ivars->m_input_memory_map.reset();
		ivars->m_input_ploytec_memory_map.reset();
		return;
	});

	return ret;
}

kern_return_t XoneDB4Device::StopIO(IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret;

	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StopIO(in_flags);
		
		ivars->startpcmout = false;
		ivars->startpcmin = false;
	});

	if (ret != kIOReturnSuccess) {
		os_log(OS_LOG_DEFAULT, "Failed to stop IO, error %d", ret);
	}

	return ret;
}

kern_return_t XoneDB4Device::PerformDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	kern_return_t ret = kIOReturnSuccess;
	switch (change_action) {
		case k_change_buffer_size_action:
		{
			auto change_info_string = OSDynamicCast(OSNumber, in_change_info);
			ivars->buffersize = change_info_string->unsigned32BitValue();
			SetZeroTimeStampPeriod(change_info_string->unsigned32BitValue());
		}
			break;
			
		default:
			ret = super::PerformDeviceConfigurationChange(change_action, in_change_info);
			break;
	}
	
	return ret;
}

kern_return_t XoneDB4Device::AbortDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	return super::AbortDeviceConfigurationChange(change_action, in_change_info);
}

void XoneDB4Device::free()
{
	if (ivars != nullptr) {
		ivars->m_driver.reset();
		ivars->m_output_stream.reset();
		ivars->m_output_memory_map.reset();
		ivars->m_output_ploytec_memory_map.reset();
		ivars->m_input_stream.reset();
		ivars->m_input_memory_map.reset();
		ivars->m_input_ploytec_memory_map.reset();
		ivars->m_work_queue.reset();
	}
	IOSafeDeleteNULL(ivars, XoneDB4Device_IVars, 1);
	super::free();
}

kern_return_t XoneDB4Device::GetPlaybackStats(playbackstats *stats)
{
	uint64_t out_sample_time, in_sample_time;
	
	GetCurrentClientIOTime(0, &out_sample_time, nullptr);
	GetCurrentClientIOTime(1, &in_sample_time, nullptr);

	stats->playing = ivars->startpcmout;
	stats->recording = ivars->startpcmin;
	stats->out_sample_time = out_sample_time;
	stats->out_sample_time_usb = ivars->out_sample_time_usb;
	stats->out_sample_time_diff = ivars->out_sample_time_usb - out_sample_time;
	stats->in_sample_time = in_sample_time;
	stats->in_sample_time_usb = ivars->in_sample_time_usb;
	stats->in_sample_time_diff = ivars->in_sample_time_usb - in_sample_time;
	stats->xruns = ivars->xruns;
	
	return kIOReturnSuccess;
}

kern_return_t XoneDB4Device::SendPCMToDevice(uint64_t completionTimestamp)
{
	__block kern_return_t ret;
	__block int i = 0;
	
	if(ivars->startpcmout == true) {
		int currentpos = (ivars->out_sample_time_usb % ivars->buffersize) / totalframesout;
		
		// UART
		ivars->PCMoutDataAddr[currentpos][432] = 0xfd;
		ivars->PCMoutDataAddr[currentpos][433] = 0xfd;
		ivars->PCMoutDataAddr[currentpos][914] = 0xfd;
		ivars->PCMoutDataAddr[currentpos][915] = 0xfd;
		ivars->PCMoutDataAddr[currentpos][1396] = 0xfd;
		ivars->PCMoutDataAddr[currentpos][1397] = 0xfd;
		ivars->PCMoutDataAddr[currentpos][1878] = 0xfd;
		ivars->PCMoutDataAddr[currentpos][1879] = 0xfd;
		
		ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutData[currentpos].get(), pcmsizeout, ivars->PCMoutCallback, 0);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "SEND ERROR %d", ret);
		}

		if(ivars->out_current_buffer_pos_usb == ivars->buffersize) {
			ivars->out_current_buffer_pos_usb = 0;
			GetCurrentZeroTimestamp(&ivars->out_hw_sample_time_usb, nullptr);
			ivars->out_hw_sample_time_usb += ivars->buffersize;
			UpdateCurrentZeroTimestamp(ivars->out_hw_sample_time_usb, completionTimestamp);
		}
		
		ivars->out_sample_time_usb += totalframesout;
		ivars->out_current_buffer_pos_usb += totalframesout;
	} else {
		ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, pcmsizeout, ivars->PCMoutCallback, 0);
		if (ret != kIOReturnSuccess) {
			os_log(OS_LOG_DEFAULT, "SEND ERROR %d", ret);
		}
	}
	return ret;
}

kern_return_t XoneDB4Device::ReceivePCMfromDevice(uint64_t completionTimestamp)
{
	__block int i;
	__block kern_return_t ret;
	
	if(ivars->startpcmin == true) {
		int currentpos = (ivars->in_sample_time_usb % ivars->buffersize) / totalframesin;
		
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinData[currentpos].get(), pcmsizein, ivars->PCMinCallback, 0);
		if (ret != kIOReturnSuccess)
		{
			os_log(OS_LOG_DEFAULT, "RECEIVE ERROR %d", ret);
		}
		
		ivars->in_sample_time_usb += totalframesin;
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
void XoneDB4Device::ploytec_convert_from_s24_3le(uint8_t *dest, uint8_t *src)
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
void XoneDB4Device::ploytec_convert_to_s24_3le(uint8_t *dest, uint8_t *src)
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
