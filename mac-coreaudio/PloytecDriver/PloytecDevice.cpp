//
//  PloytecDevice.cpp
//  PloytecDriver
//
//  Created by Marcel Bierling on 20/05/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecDevice.h"
#include "PloytecDriver.h"

#define INITIAL_BUFFERSIZE					2560
#define MAX_BUFFERSIZE						800000

#define PCM_N_URBS							4
#define PCM_N_PLAYBACK_CHANNELS				8
#define PCM_N_CAPTURE_CHANNELS				8

#define XDB4_UART_OUT_BYTES_PER_PACKET		8

#define XDB4_PCM_OUT_FRAMES_PER_PACKET		40
#define XDB4_PCM_OUT_FRAME_SIZE				48
#define XDB4_PCM_OUT_BYTES_PER_PACKET		(XDB4_PCM_OUT_FRAMES_PER_PACKET * XDB4_PCM_OUT_FRAME_SIZE)
#define XDB4_PCM_OUT_BULK_PACKET_SIZE		((XDB4_PCM_OUT_BYTES_PER_PACKET + XDB4_UART_OUT_BYTES_PER_PACKET + ((XDB4_PCM_OUT_FRAMES_PER_PACKET / 10) * 30)))
#define XDB4_PCM_OUT_INT_PACKET_SIZE		(XDB4_PCM_OUT_BYTES_PER_PACKET + XDB4_UART_OUT_BYTES_PER_PACKET)

#define XDB4_PCM_IN_FRAMES_PER_PACKET		32
#define XDB4_PCM_IN_FRAME_SIZE				64
#define XDB4_PCM_IN_PACKET_SIZE				(XDB4_PCM_IN_FRAMES_PER_PACKET * XDB4_PCM_IN_FRAME_SIZE)

#define COREAUDIO_BYTES_PER_SAMPLE			3 // S24_3LE
#define COREAUDIO_PLAYBACK_BYTES_PER_FRAME	(PCM_N_PLAYBACK_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE)
#define COREAUDIO_CAPTURE_BYTES_PER_FRAME	(PCM_N_CAPTURE_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE)
#define COREAUDIO_PCM_OUT_PACKET_SIZE		(PCM_N_PLAYBACK_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE * XDB4_PCM_OUT_FRAMES_PER_PACKET)
#define COREAUDIO_PCM_IN_PACKET_SIZE		(PCM_N_CAPTURE_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE * XDB4_PCM_IN_FRAMES_PER_PACKET)

struct PloytecDevice_IVars
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
	IOUSBEndpointDescriptor					PCMinDescriptor;
	OSAction*								PCMinCallback;
	IOUSBHostPipe*							PCMoutPipe;
	IOUSBEndpointDescriptor					PCMoutDescriptor;
	OSAction*								PCMoutCallback;
	
	IOBufferMemoryDescriptor*				PCMinDataEmpty;
	IOBufferMemoryDescriptor*				PCMoutDataEmpty;
	
	uint32_t								buffersize;
	
	OSSharedPtr<IOBufferMemoryDescriptor> 	output_io_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> 	output_ploytec_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> 	input_io_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> 	input_ploytec_ring_buffer;
	OSSharedPtr<IOMemoryDescriptor>			PCMoutData[MAX_BUFFERSIZE/XDB4_PCM_OUT_FRAMES_PER_PACKET];
	OSSharedPtr<IOMemoryDescriptor>			PCMinData[MAX_BUFFERSIZE/XDB4_PCM_IN_PACKET_SIZE];
	
	uint8_t*								PCMoutDataAddr[MAX_BUFFERSIZE/XDB4_PCM_OUT_FRAMES_PER_PACKET];
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

bool PloytecDevice::init(IOUserAudioDriver* in_driver, bool in_supports_prewarming, OSString* in_device_uid, OSString* in_model_uid, OSString* in_manufacturer_uid, uint32_t in_zero_timestamp_period, IOUSBHostPipe* PCMinPipe, OSAction* PCMinCallback, IOUSBHostPipe* PCMoutPipe, OSAction* PCMoutCallback, IOUSBHostDevice* device)
{
	auto success = super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period);
	if (!success) {
		return false;
	}
	ivars = IONewZero(PloytecDevice_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}
	
	ivars->buffersize = INITIAL_BUFFERSIZE;
	int i = 0;
	IOReturn ret = kIOReturnSuccess;
	IOAddressSegment range;
	IOOperationHandler io_operation = nullptr;
	uint8_t* PCMoutDataEmptyAddr;
	double sample_rates[] = {96000};
	OSSharedPtr<OSString> output_stream_name = OSSharedPtr(OSString::withCString("PLOYTEC PCM OUT"), OSNoRetain);
	OSSharedPtr<OSString> input_stream_name = OSSharedPtr(OSString::withCString("PLOYTEC PCM IN"), OSNoRetain);
	ivars->m_driver = OSSharedPtr(in_driver, OSRetain);
	ivars->m_work_queue = GetWorkQueue();
	ivars->PCMinPipe = PCMinPipe;
	ivars->PCMinCallback = PCMinCallback;
	ivars->PCMoutPipe = PCMoutPipe;
	ivars->PCMoutCallback = PCMoutCallback;
	ivars->startpcmin = false;
	ivars->startpcmout = false;
	IOUSBStandardEndpointDescriptors indescriptor;
	IOUSBStandardEndpointDescriptors outdescriptor;
	IOUserAudioStreamBasicDescription stream_formats[] =
	{
		{
			// S24_3LE
			96000, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_PLAYBACK_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_PLAYBACK_CHANNELS),
			24
		},
		{
			// S24_3LE
			96000, IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			1,
			static_cast<uint32_t>(COREAUDIO_CAPTURE_BYTES_PER_FRAME),
			static_cast<uint32_t>(PCM_N_CAPTURE_CHANNELS),
			24
		},
	};

	// get the USB descriptors
	ret = ivars->PCMinPipe->GetDescriptors(&indescriptor, kIOUSBGetEndpointDescriptorOriginal);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to GetDescriptors from PCM in!");
	ret = ivars->PCMoutPipe->GetDescriptors(&outdescriptor, kIOUSBGetEndpointDescriptorOriginal);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to GetDescriptors from PCM out!");
	ivars->PCMinDescriptor = indescriptor.descriptor;
	ivars->PCMoutDescriptor = outdescriptor.descriptor;

	// allocate the empty packets
	ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, XDB4_PCM_IN_PACKET_SIZE, &ivars->PCMinDataEmpty);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create empty input buffer");
	if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
		ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, XDB4_PCM_OUT_BULK_PACKET_SIZE, &ivars->PCMoutDataEmpty);
	}
	if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
		ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, XDB4_PCM_OUT_INT_PACKET_SIZE, &ivars->PCMoutDataEmpty);
	}
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create empty output buffer");
	ret = ivars->PCMoutDataEmpty->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to get address of empty output buffer");
	PCMoutDataEmptyAddr = reinterpret_cast<uint8_t*>(range.address);
	
	// set up the initial packet structure
	if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
		memset(PCMoutDataEmptyAddr + 0, 0, 480); // PCM
		memset(PCMoutDataEmptyAddr + 480, 0xfd, 1); // UART
		memset(PCMoutDataEmptyAddr + 481, 0xff, 1);
		memset(PCMoutDataEmptyAddr + 482, 0, 30);
		memset(PCMoutDataEmptyAddr + 512, 0, 480); // PCM
		memset(PCMoutDataEmptyAddr + 992, 0xfd, 1); // UART
		memset(PCMoutDataEmptyAddr + 993, 0xff, 1);
		memset(PCMoutDataEmptyAddr + 994, 0, 30);
		memset(PCMoutDataEmptyAddr + 1024, 0, 480); // PCM
		memset(PCMoutDataEmptyAddr + 1504, 0xfd, 1); // UART
		memset(PCMoutDataEmptyAddr + 1505, 0xff, 1);
		memset(PCMoutDataEmptyAddr + 1506, 0, 30);
		memset(PCMoutDataEmptyAddr + 1536, 0, 480); // PCM
		memset(PCMoutDataEmptyAddr + 2016, 0xfd, 1); // UART
		memset(PCMoutDataEmptyAddr + 2017, 0xff, 1);
		memset(PCMoutDataEmptyAddr + 2018, 0, 30);
	}
	if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
		memset(PCMoutDataEmptyAddr + 0, 0, 432); // PCM
		memset(PCMoutDataEmptyAddr + 432, 0xfd, 2); // UART
		memset(PCMoutDataEmptyAddr + 434, 0, 480); // PCM
		memset(PCMoutDataEmptyAddr + 914, 0xfd, 2); // UART
		memset(PCMoutDataEmptyAddr + 916, 0, 480); // PCM
		memset(PCMoutDataEmptyAddr + 1396, 0xfd, 2); // UART
		memset(PCMoutDataEmptyAddr + 1398, 0, 480); // PCM
		memset(PCMoutDataEmptyAddr + 1878, 0xfd, 2); // UART
		memset(PCMoutDataEmptyAddr + 1880, 0, 48); // PCM
	}
	
	// allocate the USB ring buffers
	ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, MAX_BUFFERSIZE * XDB4_PCM_OUT_FRAME_SIZE, ivars->output_ploytec_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create output ring buffer");
	ret = ivars->output_ploytec_ring_buffer->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to get address of output ring buffer");
	ivars->PloytecOutputBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	ret = device->CreateIOBuffer(kIOMemoryDirectionInOut, MAX_BUFFERSIZE * XDB4_PCM_IN_FRAME_SIZE, ivars->input_ploytec_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create input ring buffer");
	ivars->input_ploytec_ring_buffer->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to get address of input ring buffer");
	ivars->PloytecInputBufferAddr = reinterpret_cast<uint8_t*>(range.address);

	// set the available sample rates in CoreAudio
	ret = SetAvailableSampleRates(sample_rates, 1);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set the available samplerates in CoreAudio");

	// set the current sample rate in CoreAudio
	ret = SetSampleRate(96000);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set samplerate in CoreAudio");

	// set the transport type in CoreAudio
	ret = SetTransportType(IOUserAudioTransportType::USB);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set transport type in CoreAudio");

	// send the empty urbs
	for (i = 0; i < PCM_N_URBS; i++) {
		if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
			ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, XDB4_PCM_OUT_BULK_PACKET_SIZE, ivars->PCMoutCallback, 0);
		}
		if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
			ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, XDB4_PCM_OUT_INT_PACKET_SIZE, ivars->PCMoutCallback, 0);
		}
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to send PCM out urbs");
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinDataEmpty, XDB4_PCM_IN_PACKET_SIZE, ivars->PCMinCallback, 0);
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to send PCM in urbs");
	}

	// allocate the CoreAudio ring buffers
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, MAX_BUFFERSIZE * COREAUDIO_PLAYBACK_BYTES_PER_FRAME, 0, ivars->output_io_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create output ring buffer");
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, MAX_BUFFERSIZE * COREAUDIO_PLAYBACK_BYTES_PER_FRAME, 0, ivars->input_io_ring_buffer.attach());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create input ring buffer");

	// create the streams
	ivars->m_output_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, ivars->output_io_ring_buffer.get());
	FailIfNULL(ivars->m_output_stream.get(), ret = kIOReturnNoMemory, Failure, "Failed to create output stream");
	ivars->m_input_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Input, ivars->input_io_ring_buffer.get());
	FailIfNULL(ivars->m_input_stream.get(), ret = kIOReturnNoMemory, Failure, "Failed to create input stream");

	// set names on input/output streams
	ret = ivars->m_output_stream->SetName(output_stream_name.get());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set name on output stream");
	ret = ivars->m_input_stream->SetName(input_stream_name.get());
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set name on input stream");

	// set the available sample formats on the streams
	ret = ivars->m_output_stream->SetAvailableStreamFormats(stream_formats, 1);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set available stream formats on output stream");
	ret = ivars->m_input_stream->SetAvailableStreamFormats(stream_formats, 1);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set available stream formats on input stream");

	// set the current stream format on the stream
	ivars->m_stream_format = stream_formats[0];
	ret = ivars->m_output_stream->SetCurrentStreamFormat(&ivars->m_stream_format);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set current stream format on output stream");
	ivars->m_stream_format = stream_formats[1];
	ret = ivars->m_input_stream->SetCurrentStreamFormat(&ivars->m_stream_format);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set current stream format on input stream");

	// add the streams to CoreAudio
	ret = AddStream(ivars->m_output_stream.get());
	FailIfError(ret, , Failure, "Failed to add output stream");
	ret = AddStream(ivars->m_input_stream.get());
	FailIfError(ret, , Failure, "Failed to add input stream");

	// this block converts the audio from CoreAudioOutputBuffer to the format the USB device wants in PloytecOutputBuffer
	io_operation = ^kern_return_t(IOUserAudioObjectID in_device, IOUserAudioIOOperation in_io_operation, uint32_t in_io_buffer_frame_size, uint64_t in_sample_time, uint64_t in_host_time)
	{
		__block int i = 0;
		
		if (in_io_operation == IOUserAudioIOOperationWriteEnd) {
			if(ivars->out_sample_time_usb < (in_sample_time - (ivars->buffersize - in_io_buffer_frame_size)) || ivars->out_sample_time_usb > in_sample_time)
			{
				ivars->xruns++;
				os_log(OS_LOG_DEFAULT, "RESYNC OUT");
				ivars->out_sample_time_usb = in_sample_time - (ivars->buffersize/2);
			}
			
			int sampleoffset;
			int byteoffset;

			if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
				for (i = 0; i < in_io_buffer_frame_size; i++)
				{
					sampleoffset = ((in_sample_time + i) % ivars->buffersize);

					if (sampleoffset >= 10) {
						byteoffset = ((sampleoffset - 10) / 10) * 32 + 32;
					} else {
						byteoffset = 0;
					}

					ploytec_convert_from_s24_3le(ivars->PloytecOutputBufferAddr + byteoffset + (sampleoffset * XDB4_PCM_OUT_FRAME_SIZE), ivars->CoreAudioOutputBufferAddr + (sampleoffset * COREAUDIO_PLAYBACK_BYTES_PER_FRAME));
				}
			} else if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
				for (i = 0; i < in_io_buffer_frame_size; i++)
				{
					sampleoffset = ((in_sample_time + i) % ivars->buffersize);

					if (sampleoffset >= 9) {
						byteoffset = ((sampleoffset - 9) / 10) * 2 + 2;
					} else {
						byteoffset = 0;
					}

					ploytec_convert_from_s24_3le(ivars->PloytecOutputBufferAddr + byteoffset + (sampleoffset * XDB4_PCM_OUT_FRAME_SIZE), ivars->CoreAudioOutputBufferAddr + (sampleoffset * COREAUDIO_PLAYBACK_BYTES_PER_FRAME));
				}
			}
		} else if (in_io_operation == IOUserAudioIOOperationBeginRead) {
			if(ivars->in_sample_time_usb > (in_sample_time + (ivars->buffersize - in_io_buffer_frame_size)) || ivars->in_sample_time_usb < in_sample_time)
			{
				ivars->xruns++;
				os_log(OS_LOG_DEFAULT, "RESYNC IN");
				ivars->in_sample_time_usb = in_sample_time + (ivars->buffersize/2);
			}

			for (i = 0; i < in_io_buffer_frame_size; i++)
			{
				ploytec_convert_to_s24_3le(ivars->CoreAudioInputBufferAddr + (((in_sample_time + i) % ivars->buffersize) * COREAUDIO_CAPTURE_BYTES_PER_FRAME), ivars->PloytecInputBufferAddr + (((in_sample_time + i) % ivars->buffersize) * XDB4_PCM_IN_FRAME_SIZE));
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

kern_return_t PloytecDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
	__block int i = 0;
	__block kern_return_t ret = kIOReturnSuccess;
	__block OSSharedPtr<IOMemoryDescriptor> output_iomd;
	__block OSSharedPtr<IOMemoryDescriptor> input_iomd;
	__block uint64_t length = 0;
	
	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StartIO(in_flags);
		FailIfError(ret, , Failure, "Failed to start I/O");

		output_iomd = ivars->m_output_stream->GetIOMemoryDescriptor();
		FailIfNULL(output_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get output stream");
		ret = output_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_output_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from output stream");
		ivars->CoreAudioOutputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_output_memory_map->GetAddress() + ivars->m_output_memory_map->GetOffset());
		
		ret = ivars->output_ploytec_ring_buffer->CreateMapping(0, 0, 0, 0, 0, ivars->m_output_ploytec_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from output buffer");

		for (i = 0; i < (ivars->buffersize/XDB4_PCM_OUT_FRAMES_PER_PACKET); i++) {
			if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
				ret = ivars->output_ploytec_ring_buffer->CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * XDB4_PCM_OUT_BULK_PACKET_SIZE, XDB4_PCM_OUT_BULK_PACKET_SIZE, ivars->output_ploytec_ring_buffer.get(), ivars->PCMoutData[i].attach());
				FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create USB output SubMemoryDescriptor");
			} else if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
				ret = ivars->output_ploytec_ring_buffer->CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * XDB4_PCM_OUT_INT_PACKET_SIZE, XDB4_PCM_OUT_INT_PACKET_SIZE, ivars->output_ploytec_ring_buffer.get(), ivars->PCMoutData[i].attach());
				FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create USB output SubMemoryDescriptor");
			} else {
				ret = kIOReturnError;
			}

			if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
				ivars->PCMoutDataAddr[i] = ivars->PloytecOutputBufferAddr + (i * XDB4_PCM_OUT_BULK_PACKET_SIZE);
			}
			if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
				ivars->PCMoutDataAddr[i] = ivars->PloytecOutputBufferAddr + (i * XDB4_PCM_OUT_INT_PACKET_SIZE);
			}

			if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
				memset(ivars->PCMoutDataAddr[i] + 0, 0, 480); // PCM
				memset(ivars->PCMoutDataAddr[i] + 480, 0xfd, 1); // UART
				memset(ivars->PCMoutDataAddr[i] + 481, 0xff, 1);
				memset(ivars->PCMoutDataAddr[i] + 482, 0, 30);
				memset(ivars->PCMoutDataAddr[i] + 512, 0, 480); // PCM
				memset(ivars->PCMoutDataAddr[i] + 992, 0xfd, 1); // UART
				memset(ivars->PCMoutDataAddr[i] + 993, 0xff, 1);
				memset(ivars->PCMoutDataAddr[i] + 994, 0, 30);
				memset(ivars->PCMoutDataAddr[i] + 1024, 0, 480); // PCM
				memset(ivars->PCMoutDataAddr[i] + 1504, 0xfd, 1); // UART
				memset(ivars->PCMoutDataAddr[i] + 1505, 0xff, 1);
				memset(ivars->PCMoutDataAddr[i] + 1506, 0, 30);
				memset(ivars->PCMoutDataAddr[i] + 1536, 0, 480); // PCM
				memset(ivars->PCMoutDataAddr[i] + 2016, 0xfd, 1); // UART
				memset(ivars->PCMoutDataAddr[i] + 2017, 0xff, 1);
				memset(ivars->PCMoutDataAddr[i] + 2018, 0, 30);
			}
			if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
				memset(ivars->PCMoutDataAddr[i] + 0, 0, 432); // PCM
				memset(ivars->PCMoutDataAddr[i] + 432, 0xfd, 2); // UART
				memset(ivars->PCMoutDataAddr[i] + 434, 0, 480); // PCM
				memset(ivars->PCMoutDataAddr[i] + 914, 0xfd, 2); // UART
				memset(ivars->PCMoutDataAddr[i] + 916, 0, 480); // PCM
				memset(ivars->PCMoutDataAddr[i] + 1396, 0xfd, 2); // UART
				memset(ivars->PCMoutDataAddr[i] + 1398, 0, 480); // PCM
				memset(ivars->PCMoutDataAddr[i] + 1878, 0xfd, 2); // UART
				memset(ivars->PCMoutDataAddr[i] + 1880, 0, 48); // PCM
			}
		}
		
		input_iomd = ivars->m_input_stream->GetIOMemoryDescriptor();
		FailIfNULL(input_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get input stream");
		ret = input_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_input_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from input stream");
		ivars->CoreAudioInputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_input_memory_map->GetAddress() + ivars->m_input_memory_map->GetOffset());
		
		ret = ivars->input_ploytec_ring_buffer->CreateMapping(0, 0, 0, 0, 0, ivars->m_input_ploytec_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map for USB ring buffer");
		
		for (i = 0; i < (ivars->buffersize/XDB4_PCM_IN_FRAMES_PER_PACKET); i++) {
			ret = ivars->input_ploytec_ring_buffer->CreateSubMemoryDescriptor(kIOMemoryDirectionInOut, i * XDB4_PCM_IN_PACKET_SIZE, XDB4_PCM_IN_PACKET_SIZE, ivars->input_ploytec_ring_buffer.get(), ivars->PCMinData[i].attach());
			FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create USB input SubMemoryDescriptor");
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

kern_return_t PloytecDevice::StopIO(IOUserAudioStartStopFlags in_flags)
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

kern_return_t PloytecDevice::PerformDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
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

kern_return_t PloytecDevice::AbortDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	return super::AbortDeviceConfigurationChange(change_action, in_change_info);
}

void PloytecDevice::free()
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
	IOSafeDeleteNULL(ivars, PloytecDevice_IVars, 1);
	super::free();
}

kern_return_t PloytecDevice::GetPlaybackStats(playbackstats *stats)
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

kern_return_t PloytecDevice::SendPCMToDevice(uint64_t completionTimestamp)
{
	__block kern_return_t ret;
	__block int i = 0;
	
	if(ivars->startpcmout == true) {
		int currentpos = (ivars->out_sample_time_usb % ivars->buffersize) / XDB4_PCM_OUT_FRAMES_PER_PACKET;
		
		// UART
		if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
			memset(ivars->PCMoutDataAddr[currentpos] + 480, 0xfd, 1); // UART
			memset(ivars->PCMoutDataAddr[currentpos] + 481, 0xff, 1);
			memset(ivars->PCMoutDataAddr[currentpos] + 992, 0xfd, 1); // UART
			memset(ivars->PCMoutDataAddr[currentpos] + 993, 0xff, 1);
			memset(ivars->PCMoutDataAddr[currentpos] + 1504, 0xfd, 1); // UART
			memset(ivars->PCMoutDataAddr[currentpos] + 1505, 0xff, 1);
			memset(ivars->PCMoutDataAddr[currentpos] + 2016, 0xfd, 1); // UART
			memset(ivars->PCMoutDataAddr[currentpos] + 2017, 0xff, 1);
		} else if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
			memset(ivars->PCMoutDataAddr[currentpos] + 432, 0xfd, 2); // UART
			memset(ivars->PCMoutDataAddr[currentpos] + 914, 0xfd, 2); // UART
			memset(ivars->PCMoutDataAddr[currentpos] + 1396, 0xfd, 2); // UART
			memset(ivars->PCMoutDataAddr[currentpos] + 1878, 0xfd, 2); // UART
		}
		
		if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
			ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutData[currentpos].get(), XDB4_PCM_OUT_BULK_PACKET_SIZE, ivars->PCMoutCallback, 0);
		} else if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
			ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutData[currentpos].get(), XDB4_PCM_OUT_INT_PACKET_SIZE, ivars->PCMoutCallback, 0);
		} else {
			ret = kIOReturnError;
		}
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
		
		ivars->out_sample_time_usb += XDB4_PCM_OUT_FRAMES_PER_PACKET;
		ivars->out_current_buffer_pos_usb += XDB4_PCM_OUT_FRAMES_PER_PACKET;
	} else {
		if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeBulk) {
			ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, XDB4_PCM_OUT_BULK_PACKET_SIZE, ivars->PCMoutCallback, 0);
		} else if (ivars->PCMoutDescriptor.bmAttributes == kIOUSBEndpointDescriptorTransferTypeInterrupt) {
			ret = ivars->PCMoutPipe->AsyncIO(ivars->PCMoutDataEmpty, XDB4_PCM_OUT_INT_PACKET_SIZE, ivars->PCMoutCallback, 0);
		} else {
			ret = kIOReturnError;
		}
		if (ret != kIOReturnSuccess) {
			os_log(OS_LOG_DEFAULT, "SEND ERROR %d", ret);
		}
	}
	return ret;
}

kern_return_t PloytecDevice::ReceivePCMfromDevice(uint64_t completionTimestamp)
{
	__block int i;
	__block kern_return_t ret;
	__block int currentpos;
	
	if(ivars->startpcmin == true) {
		currentpos = (ivars->in_sample_time_usb % ivars->buffersize) / XDB4_PCM_IN_PACKET_SIZE;
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinData[currentpos].get(), XDB4_PCM_IN_PACKET_SIZE, ivars->PCMinCallback, 0);
		ivars->in_sample_time_usb += XDB4_PCM_IN_FRAMES_PER_PACKET;
	} else {
		ret = ivars->PCMinPipe->AsyncIO(ivars->PCMinDataEmpty, XDB4_PCM_IN_PACKET_SIZE, ivars->PCMinCallback, 0);
	}

	return ret;
}

/* Takes 24 bytes, outputs 48 bytes */
void PloytecDevice::ploytec_convert_from_s24_3le(uint8_t *dest, uint8_t *src)
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
void PloytecDevice::ploytec_convert_to_s24_3le(uint8_t *dest, uint8_t *src)
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
