#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecDevice.h"

#define INITIAL_BUFFERSIZE			2560
#define MAX_BUFFERSIZE				800000

#define PCM_N_PLAYBACK_CHANNELS			8
#define PCM_N_CAPTURE_CHANNELS			8

#define XDB4_PCM_OUT_FRAME_SIZE			48
#define XDB4_PCM_IN_FRAME_SIZE			64

#define COREAUDIO_BYTES_PER_SAMPLE		3 // S24_3LE
#define COREAUDIO_PLAYBACK_BYTES_PER_FRAME	(PCM_N_PLAYBACK_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE)
#define COREAUDIO_CAPTURE_BYTES_PER_FRAME	(PCM_N_CAPTURE_CHANNELS * COREAUDIO_BYTES_PER_SAMPLE)

struct PloytecDevice_IVars
{
	OSSharedPtr<IOUserAudioDriver>		m_driver;
	OSSharedPtr<IODispatchQueue>		m_work_queue;
	IOUserAudioStreamBasicDescription	m_stream_format;
	OSSharedPtr<IOUserAudioStream>		m_output_stream;
	OSSharedPtr<IOUserAudioStream>		m_input_stream;
	OSSharedPtr<IOMemoryMap>		m_output_memory_map;
	OSSharedPtr<IOMemoryMap>		m_input_memory_map;

	OSSharedPtr<IOBufferMemoryDescriptor> 	output_io_ring_buffer;
	OSSharedPtr<IOBufferMemoryDescriptor> 	input_io_ring_buffer;
	
	uint8_t*				CoreAudioOutputBufferAddr;
	uint8_t*				CoreAudioInputBufferAddr;
	uint8_t*				PloytecOutputBufferAddr;
	uint8_t*				PloytecInputBufferAddr;
	
	uint64_t				HWSampleTimeIn;
	uint64_t				HWSampleTimeOut;
	//uint64_t				out_hw_sample_time_usb;
	uint16_t				out_current_buffer_pos_usb;
	uint64_t				in_sample_time;
	uint64_t				out_sample_time;
	
	bool					PCMinActive;
	bool					PCMoutActive;
	
	uint64_t				xruns;
};

bool PloytecDevice::init(IOUserAudioDriver* in_driver, bool in_supports_prewarming, OSString* in_device_uid, OSString* in_model_uid, OSString* in_manufacturer_uid, uint32_t in_zero_timestamp_period, IOBufferMemoryDescriptor* receiveBuffer, IOBufferMemoryDescriptor* transmitBuffer, TransferMode transferMode)
{
	auto success = super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period);
	if (!success) {
		return false;
	}
	ivars = IONewZero(PloytecDevice_IVars, 1);
	if (ivars == nullptr) {
		return false;
	}
	
	int i = 0;
	IOReturn ret = kIOReturnSuccess;
	IOAddressSegment range;
	IOOperationHandler ioOperationBulk = nullptr;
	IOOperationHandler ioOperationInterrupt = nullptr;
	char outputName[128];
	char inputName[128];
	snprintf(outputName, sizeof(outputName), "%s PCM OUT", in_device_uid->getCStringNoCopy());
	snprintf(inputName, sizeof(inputName), "%s PCM IN", in_device_uid->getCStringNoCopy());
	OSSharedPtr<OSString> output_stream_name = OSSharedPtr(OSString::withCString(outputName), OSNoRetain);
	OSSharedPtr<OSString> input_stream_name = OSSharedPtr(OSString::withCString(inputName), OSNoRetain);
	ivars->m_driver = OSSharedPtr(in_driver, OSRetain);
	//ivars->m_work_queue = GetWorkQueue();
	double sample_rates[] = {96000};
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

	// allocate the USB ring buffers
	ret = transmitBuffer->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to get address of output ring buffer");
	ivars->PloytecOutputBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	ret = receiveBuffer->GetAddressRange(&range);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to get address of input ring buffer");
	ivars->PloytecInputBufferAddr = reinterpret_cast<uint8_t*>(range.address);

	// set the available sample rates in CoreAudio
	ret = SetAvailableSampleRates(sample_rates, 1);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set the available samplerates in CoreAudio");
	
	SetOutputSafetyOffset(100);

	// set the current sample rate in CoreAudio
	ret = SetSampleRate(96000);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set samplerate in CoreAudio");

	// set the transport type in CoreAudio
	ret = SetTransportType(IOUserAudioTransportType::USB);
	FailIf(ret != kIOReturnSuccess, , Failure, "Failed to set transport type in CoreAudio");

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
	ioOperationBulk = ^kern_return_t(IOUserAudioObjectID in_device, IOUserAudioIOOperation in_io_operation, uint32_t in_io_buffer_frame_size, uint64_t in_sample_time, uint64_t in_host_time)
	{
		if (in_io_operation == IOUserAudioIOOperationWriteEnd) {
			ivars->PCMoutActive = true;
			ivars->out_sample_time = in_sample_time;

			int sampleoffset;
			int byteoffset;

			for (int i = 0; i < in_io_buffer_frame_size; i++)
			{
				sampleoffset = ((in_sample_time + i) % GetZeroTimestampPeriod());

				if (sampleoffset >= 10) {
					byteoffset = ((sampleoffset - 10) / 10) * 32 + 32;
				} else {
					byteoffset = 0;
				}

				EncodePloytecPCM(ivars->PloytecOutputBufferAddr + byteoffset + (sampleoffset * XDB4_PCM_OUT_FRAME_SIZE), ivars->CoreAudioOutputBufferAddr + (sampleoffset * COREAUDIO_PLAYBACK_BYTES_PER_FRAME));
			}
		} else if (in_io_operation == IOUserAudioIOOperationBeginRead) {
			ivars->PCMinActive = true;
			ivars->in_sample_time = in_sample_time;

			for (int i = 0; i < in_io_buffer_frame_size; i++)
			{
				DecodePloytecPCM(ivars->CoreAudioInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * COREAUDIO_CAPTURE_BYTES_PER_FRAME), ivars->PloytecInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * XDB4_PCM_IN_FRAME_SIZE));
			}
		}

		return kIOReturnSuccess;
	};

	ioOperationInterrupt = ^kern_return_t(IOUserAudioObjectID in_device, IOUserAudioIOOperation in_io_operation, uint32_t in_io_buffer_frame_size, uint64_t in_sample_time, uint64_t in_host_time)
	{
		if (in_io_operation == IOUserAudioIOOperationWriteEnd) {
			ivars->PCMoutActive = true;
			ivars->out_sample_time = in_sample_time;

			int sampleoffset;
			int byteoffset;

			for (int i = 0; i < in_io_buffer_frame_size; i++)
			{
				sampleoffset = ((in_sample_time + i) % GetZeroTimestampPeriod());

				if (sampleoffset >= 9) {
					byteoffset = ((sampleoffset - 9) / 10) * 2 + 2;
				} else {
					byteoffset = 0;
				}

				EncodePloytecPCM(ivars->PloytecOutputBufferAddr + byteoffset + (sampleoffset * XDB4_PCM_OUT_FRAME_SIZE), ivars->CoreAudioOutputBufferAddr + (sampleoffset * COREAUDIO_PLAYBACK_BYTES_PER_FRAME));
			}
		} else if (in_io_operation == IOUserAudioIOOperationBeginRead) {
			ivars->PCMinActive = true;
			ivars->in_sample_time = in_sample_time;

			for (int i = 0; i < in_io_buffer_frame_size; i++)
			{
				DecodePloytecPCM(ivars->CoreAudioInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * COREAUDIO_CAPTURE_BYTES_PER_FRAME), ivars->PloytecInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * XDB4_PCM_IN_FRAME_SIZE));
			}
		}
		
		return kIOReturnSuccess;
	};

	if (transferMode == INTERRUPT)
		this->SetIOOperationHandler(ioOperationInterrupt);
	else if (transferMode == BULK)
		this->SetIOOperationHandler(ioOperationBulk);
	
	return true;

Failure:
	ivars->m_driver.reset();
	ivars->m_output_stream.reset();
	ivars->m_input_stream.reset();
	ivars->m_output_memory_map.reset();
	ivars->m_input_memory_map.reset();
	return false;
}

kern_return_t PloytecDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
	__block int i = 0;
	__block kern_return_t ret = kIOReturnSuccess;
	__block OSSharedPtr<IOMemoryDescriptor> output_iomd;
	__block OSSharedPtr<IOMemoryDescriptor> input_iomd;
	
	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StartIO(in_flags);
		FailIfError(ret, , Failure, "Failed to start I/O");

		output_iomd = ivars->m_output_stream->GetIOMemoryDescriptor();
		FailIfNULL(output_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get output stream");
		ret = output_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_output_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from output stream");
		ivars->CoreAudioOutputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_output_memory_map->GetAddress() + ivars->m_output_memory_map->GetOffset());

		input_iomd = ivars->m_input_stream->GetIOMemoryDescriptor();
		FailIfNULL(input_iomd.get(), ret = kIOReturnNoMemory, Failure, "Failed to get input stream");
		ret = input_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_input_memory_map.attach());
		FailIf(ret != kIOReturnSuccess, , Failure, "Failed to create memory map from input stream");
		ivars->CoreAudioInputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_input_memory_map->GetAddress() + ivars->m_input_memory_map->GetOffset());

		ivars->HWSampleTimeOut = 0;
		ivars->HWSampleTimeIn = 0;
		ivars->PCMinActive = false;
		ivars->PCMoutActive = false;

		ivars->m_output_stream->SetStreamIsActive(true);
		ivars->m_input_stream->SetStreamIsActive(true);

		return;

	Failure:
		super::StopIO(in_flags);
		ivars->m_output_memory_map.reset();
		ivars->m_input_memory_map.reset();
		return;
	});

	return ret;
}

kern_return_t PloytecDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret;

	ivars->m_work_queue->DispatchSync(^(){
		ret = super::StopIO(in_flags);
		
		ivars->PCMinActive = false;
		ivars->PCMoutActive = false;
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
		ivars->m_input_stream.reset();
		ivars->m_input_memory_map.reset();
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

	stats->playing = ivars->PCMoutActive;
	stats->recording = ivars->PCMinActive;
	stats->out_sample_time = out_sample_time;
	stats->in_sample_time = in_sample_time;
	stats->xruns = ivars->xruns;
	
	return kIOReturnSuccess;
}

bool PloytecDevice::Playback(uint16_t &currentpos, uint16_t frameCount, uint64_t completionTimestamp)
{
	uint32_t period = GetZeroTimestampPeriod();
	uint64_t sampleTime = ivars->HWSampleTimeOut;

	currentpos = (sampleTime % period) / frameCount;

	uint64_t expectedZeroTimestamp = (sampleTime / period) * period;

	uint64_t currentZeroTimestamp;
	GetCurrentZeroTimestamp(&currentZeroTimestamp, nullptr);

	if (sampleTime + (period / 2) < currentZeroTimestamp) {
		ivars->xruns++;
		os_log(OS_LOG_DEFAULT, "XRUN detected: sampleTime=%llu zeroTime=%llu", sampleTime, currentZeroTimestamp);
	}

	if (currentZeroTimestamp != expectedZeroTimestamp) {
		UpdateCurrentZeroTimestamp(expectedZeroTimestamp, completionTimestamp);
	}

	ivars->HWSampleTimeOut += frameCount;

	return true;
}

bool PloytecDevice::Capture(uint16_t &currentpos, uint16_t frameCount, uint64_t completionTimestamp)
{
	currentpos = (ivars->HWSampleTimeIn % GetZeroTimestampPeriod()) / frameCount;
	ivars->HWSampleTimeIn += frameCount;
	return true;
}

void PloytecDevice::SetDispatchQueue(IODispatchQueue* queue)
{
	if (queue)
		ivars->m_work_queue = OSSharedPtr<IODispatchQueue>(queue, OSRetain);
}

/* Takes 24 bytes, outputs 48 bytes */
void PloytecDevice::EncodePloytecPCM(uint8_t *dst, uint8_t *src)
{
    //          ========= channel 1 ========   ========= channel 3 ========   ========= channel 5 ========   ========= channel 7 ========
    dst[0x00] = ((src[0x02] & 0x80) >> 0x07) | ((src[0x08] & 0x80) >> 0x06) | ((src[0x0E] & 0x80) >> 0x05) | ((src[0x14] & 0x80) >> 0x04);
    dst[0x01] = ((src[0x02] & 0x40) >> 0x06) | ((src[0x08] & 0x40) >> 0x05) | ((src[0x0E] & 0x40) >> 0x04) | ((src[0x14] & 0x40) >> 0x03);
    dst[0x02] = ((src[0x02] & 0x20) >> 0x05) | ((src[0x08] & 0x20) >> 0x04) | ((src[0x0E] & 0x20) >> 0x03) | ((src[0x14] & 0x20) >> 0x02);
    dst[0x03] = ((src[0x02] & 0x10) >> 0x04) | ((src[0x08] & 0x10) >> 0x03) | ((src[0x0E] & 0x10) >> 0x02) | ((src[0x14] & 0x10) >> 0x01);
    dst[0x04] = ((src[0x02] & 0x08) >> 0x03) | ((src[0x08] & 0x08) >> 0x02) | ((src[0x0E] & 0x08) >> 0x01) | ((src[0x14] & 0x08) >> 0x00);
    dst[0x05] = ((src[0x02] & 0x04) >> 0x02) | ((src[0x08] & 0x04) >> 0x01) | ((src[0x0E] & 0x04) >> 0x00) | ((src[0x14] & 0x04) << 0x01);
    dst[0x06] = ((src[0x02] & 0x02) >> 0x01) | ((src[0x08] & 0x02) >> 0x00) | ((src[0x0E] & 0x02) << 0x01) | ((src[0x14] & 0x02) << 0x02);
    dst[0x07] = ((src[0x02] & 0x01) >> 0x00) | ((src[0x08] & 0x01) << 0x01) | ((src[0x0E] & 0x01) << 0x02) | ((src[0x14] & 0x01) << 0x03);
    dst[0x08] = ((src[0x01] & 0x80) >> 0x07) | ((src[0x07] & 0x80) >> 0x06) | ((src[0x0D] & 0x80) >> 0x05) | ((src[0x13] & 0x80) >> 0x04);
    dst[0x09] = ((src[0x01] & 0x40) >> 0x06) | ((src[0x07] & 0x40) >> 0x05) | ((src[0x0D] & 0x40) >> 0x04) | ((src[0x13] & 0x40) >> 0x03);
    dst[0x0A] = ((src[0x01] & 0x20) >> 0x05) | ((src[0x07] & 0x20) >> 0x04) | ((src[0x0D] & 0x20) >> 0x03) | ((src[0x13] & 0x20) >> 0x02);
    dst[0x0B] = ((src[0x01] & 0x10) >> 0x04) | ((src[0x07] & 0x10) >> 0x03) | ((src[0x0D] & 0x10) >> 0x02) | ((src[0x13] & 0x10) >> 0x01);
    dst[0x0C] = ((src[0x01] & 0x08) >> 0x03) | ((src[0x07] & 0x08) >> 0x02) | ((src[0x0D] & 0x08) >> 0x01) | ((src[0x13] & 0x08) >> 0x00);
    dst[0x0D] = ((src[0x01] & 0x04) >> 0x02) | ((src[0x07] & 0x04) >> 0x01) | ((src[0x0D] & 0x04) >> 0x00) | ((src[0x13] & 0x04) << 0x01);
    dst[0x0E] = ((src[0x01] & 0x02) >> 0x01) | ((src[0x07] & 0x02) >> 0x00) | ((src[0x0D] & 0x02) << 0x01) | ((src[0x13] & 0x02) << 0x02);
    dst[0x0F] = ((src[0x01] & 0x01) >> 0x00) | ((src[0x07] & 0x01) << 0x01) | ((src[0x0D] & 0x01) << 0x02) | ((src[0x13] & 0x01) << 0x03);
    dst[0x10] = ((src[0x00] & 0x80) >> 0x07) | ((src[0x06] & 0x80) >> 0x06) | ((src[0x0C] & 0x80) >> 0x05) | ((src[0x12] & 0x80) >> 0x04);
    dst[0x11] = ((src[0x00] & 0x40) >> 0x06) | ((src[0x06] & 0x40) >> 0x05) | ((src[0x0C] & 0x40) >> 0x04) | ((src[0x12] & 0x40) >> 0x03);
    dst[0x12] = ((src[0x00] & 0x20) >> 0x05) | ((src[0x06] & 0x20) >> 0x04) | ((src[0x0C] & 0x20) >> 0x03) | ((src[0x12] & 0x20) >> 0x02);
    dst[0x13] = ((src[0x00] & 0x10) >> 0x04) | ((src[0x06] & 0x10) >> 0x03) | ((src[0x0C] & 0x10) >> 0x02) | ((src[0x12] & 0x10) >> 0x01);
    dst[0x14] = ((src[0x00] & 0x08) >> 0x03) | ((src[0x06] & 0x08) >> 0x02) | ((src[0x0C] & 0x08) >> 0x01) | ((src[0x12] & 0x08) >> 0x00);
    dst[0x15] = ((src[0x00] & 0x04) >> 0x02) | ((src[0x06] & 0x04) >> 0x01) | ((src[0x0C] & 0x04) >> 0x00) | ((src[0x12] & 0x04) << 0x01);
    dst[0x16] = ((src[0x00] & 0x02) >> 0x01) | ((src[0x06] & 0x02) >> 0x00) | ((src[0x0C] & 0x02) << 0x01) | ((src[0x12] & 0x02) << 0x02);
    dst[0x17] = ((src[0x00] & 0x01) >> 0x00) | ((src[0x06] & 0x01) << 0x01) | ((src[0x0C] & 0x01) << 0x02) | ((src[0x12] & 0x01) << 0x03);
    //          ========= channel 2 ========   ========= channel 4 ========   ========= channel 6 ========   ========= channel 8 ========
    dst[0x18] = ((src[0x05] & 0x80) >> 0x07) | ((src[0x0B] & 0x80) >> 0x06) | ((src[0x11] & 0x80) >> 0x05) | ((src[0x17] & 0x80) >> 0x04);
    dst[0x19] = ((src[0x05] & 0x40) >> 0x06) | ((src[0x0B] & 0x40) >> 0x05) | ((src[0x11] & 0x40) >> 0x04) | ((src[0x17] & 0x40) >> 0x03);
    dst[0x1A] = ((src[0x05] & 0x20) >> 0x05) | ((src[0x0B] & 0x20) >> 0x04) | ((src[0x11] & 0x20) >> 0x03) | ((src[0x17] & 0x20) >> 0x02);
    dst[0x1B] = ((src[0x05] & 0x10) >> 0x04) | ((src[0x0B] & 0x10) >> 0x03) | ((src[0x11] & 0x10) >> 0x02) | ((src[0x17] & 0x10) >> 0x01);
    dst[0x1C] = ((src[0x05] & 0x08) >> 0x03) | ((src[0x0B] & 0x08) >> 0x02) | ((src[0x11] & 0x08) >> 0x01) | ((src[0x17] & 0x08) >> 0x00);
    dst[0x1D] = ((src[0x05] & 0x04) >> 0x02) | ((src[0x0B] & 0x04) >> 0x01) | ((src[0x11] & 0x04) >> 0x00) | ((src[0x17] & 0x04) << 0x01);
    dst[0x1E] = ((src[0x05] & 0x02) >> 0x01) | ((src[0x0B] & 0x02) >> 0x00) | ((src[0x11] & 0x02) << 0x01) | ((src[0x17] & 0x02) << 0x02);
    dst[0x1F] = ((src[0x05] & 0x01) >> 0x00) | ((src[0x0B] & 0x01) << 0x01) | ((src[0x11] & 0x01) << 0x02) | ((src[0x17] & 0x01) << 0x03);
    dst[0x20] = ((src[0x04] & 0x80) >> 0x07) | ((src[0x0A] & 0x80) >> 0x06) | ((src[0x10] & 0x80) >> 0x05) | ((src[0x16] & 0x80) >> 0x04);
    dst[0x21] = ((src[0x04] & 0x40) >> 0x06) | ((src[0x0A] & 0x40) >> 0x05) | ((src[0x10] & 0x40) >> 0x04) | ((src[0x16] & 0x40) >> 0x03);
    dst[0x22] = ((src[0x04] & 0x20) >> 0x05) | ((src[0x0A] & 0x20) >> 0x04) | ((src[0x10] & 0x20) >> 0x03) | ((src[0x16] & 0x20) >> 0x02);
    dst[0x23] = ((src[0x04] & 0x10) >> 0x04) | ((src[0x0A] & 0x10) >> 0x03) | ((src[0x10] & 0x10) >> 0x02) | ((src[0x16] & 0x10) >> 0x01);
    dst[0x24] = ((src[0x04] & 0x08) >> 0x03) | ((src[0x0A] & 0x08) >> 0x02) | ((src[0x10] & 0x08) >> 0x01) | ((src[0x16] & 0x08) >> 0x00);
    dst[0x25] = ((src[0x04] & 0x04) >> 0x02) | ((src[0x0A] & 0x04) >> 0x01) | ((src[0x10] & 0x04) >> 0x00) | ((src[0x16] & 0x04) << 0x01);
    dst[0x26] = ((src[0x04] & 0x02) >> 0x01) | ((src[0x0A] & 0x02) >> 0x00) | ((src[0x10] & 0x02) << 0x01) | ((src[0x16] & 0x02) << 0x02);
    dst[0x27] = ((src[0x04] & 0x01) >> 0x00) | ((src[0x0A] & 0x01) << 0x01) | ((src[0x10] & 0x01) << 0x02) | ((src[0x16] & 0x01) << 0x03);
    dst[0x28] = ((src[0x03] & 0x80) >> 0x07) | ((src[0x09] & 0x80) >> 0x06) | ((src[0x0F] & 0x80) >> 0x05) | ((src[0x15] & 0x80) >> 0x04);
    dst[0x29] = ((src[0x03] & 0x40) >> 0x06) | ((src[0x09] & 0x40) >> 0x05) | ((src[0x0F] & 0x40) >> 0x04) | ((src[0x15] & 0x40) >> 0x03);
    dst[0x2A] = ((src[0x03] & 0x20) >> 0x05) | ((src[0x09] & 0x20) >> 0x04) | ((src[0x0F] & 0x20) >> 0x03) | ((src[0x15] & 0x20) >> 0x02);
    dst[0x2B] = ((src[0x03] & 0x10) >> 0x04) | ((src[0x09] & 0x10) >> 0x03) | ((src[0x0F] & 0x10) >> 0x02) | ((src[0x15] & 0x10) >> 0x01);
    dst[0x2C] = ((src[0x03] & 0x08) >> 0x03) | ((src[0x09] & 0x08) >> 0x02) | ((src[0x0F] & 0x08) >> 0x01) | ((src[0x15] & 0x08) >> 0x00);
    dst[0x2D] = ((src[0x03] & 0x04) >> 0x02) | ((src[0x09] & 0x04) >> 0x01) | ((src[0x0F] & 0x04) >> 0x00) | ((src[0x15] & 0x04) << 0x01);
    dst[0x2E] = ((src[0x03] & 0x02) >> 0x01) | ((src[0x09] & 0x02) >> 0x00) | ((src[0x0F] & 0x02) << 0x01) | ((src[0x15] & 0x02) << 0x02);
    dst[0x2F] = ((src[0x03] & 0x01) >> 0x00) | ((src[0x09] & 0x01) << 0x01) | ((src[0x0F] & 0x01) << 0x02) | ((src[0x15] & 0x01) << 0x03);
}

/* Takes 64 bytes, outputs 24 bytes */
void PloytecDevice::DecodePloytecPCM(uint8_t *dst, uint8_t *src)
{
	// channel 1
	dst[0x00] = ((src[0x17] & 0x01) << 0x00) | ((src[0x16] & 0x01) << 0x01) | ((src[0x15] & 0x01) << 0x02) | ((src[0x14] & 0x01) << 0x03) | ((src[0x13] & 0x01) << 0x04) | ((src[0x12] & 0x01) << 0x05) | ((src[0x11] & 0x01) << 0x06) | ((src[0x10] & 0x01) << 0x07);
	dst[0x01] = ((src[0x0F] & 0x01) << 0x00) | ((src[0x0E] & 0x01) << 0x01) | ((src[0x0D] & 0x01) << 0x02) | ((src[0x0C] & 0x01) << 0x03) | ((src[0x0B] & 0x01) << 0x04) | ((src[0x0A] & 0x01) << 0x05) | ((src[0x09] & 0x01) << 0x06) | ((src[0x08] & 0x01) << 0x07);
	dst[0x02] = ((src[0x07] & 0x01) << 0x00) | ((src[0x06] & 0x01) << 0x01) | ((src[0x05] & 0x01) << 0x02) | ((src[0x04] & 0x01) << 0x03) | ((src[0x03] & 0x01) << 0x04) | ((src[0x02] & 0x01) << 0x05) | ((src[0x01] & 0x01) << 0x06) | ((src[0x00] & 0x01) << 0x07);
	// channel 2
	dst[0x03] = ((src[0x37] & 0x01) << 0x00) | ((src[0x36] & 0x01) << 0x01) | ((src[0x35] & 0x01) << 0x02) | ((src[0x34] & 0x01) << 0x03) | ((src[0x33] & 0x01) << 0x04) | ((src[0x32] & 0x01) << 0x05) | ((src[0x31] & 0x01) << 0x06) | ((src[0x30] & 0x01) << 0x07);
	dst[0x04] = ((src[0x2F] & 0x01) << 0x00) | ((src[0x2E] & 0x01) << 0x01) | ((src[0x2D] & 0x01) << 0x02) | ((src[0x2C] & 0x01) << 0x03) | ((src[0x2B] & 0x01) << 0x04) | ((src[0x2A] & 0x01) << 0x05) | ((src[0x29] & 0x01) << 0x06) | ((src[0x28] & 0x01) << 0x07);
	dst[0x05] = ((src[0x27] & 0x01) << 0x00) | ((src[0x26] & 0x01) << 0x01) | ((src[0x25] & 0x01) << 0x02) | ((src[0x24] & 0x01) << 0x03) | ((src[0x23] & 0x01) << 0x04) | ((src[0x22] & 0x01) << 0x05) | ((src[0x21] & 0x01) << 0x06) | ((src[0x20] & 0x01) << 0x07);
	// channel 3
	dst[0x06] = ((src[0x17] & 0x02) >> 0x01) | ((src[0x16] & 0x02) << 0x00) | ((src[0x15] & 0x02) << 0x01) | ((src[0x14] & 0x02) << 0x02) | ((src[0x13] & 0x02) << 0x03) | ((src[0x12] & 0x02) << 0x04) | ((src[0x11] & 0x02) << 0x05) | ((src[0x10] & 0x02) << 0x06);
	dst[0x07] = ((src[0x0F] & 0x02) >> 0x01) | ((src[0x0E] & 0x02) << 0x00) | ((src[0x0D] & 0x02) << 0x01) | ((src[0x0C] & 0x02) << 0x02) | ((src[0x0B] & 0x02) << 0x03) | ((src[0x0A] & 0x02) << 0x04) | ((src[0x09] & 0x02) << 0x05) | ((src[0x08] & 0x02) << 0x06);
	dst[0x08] = ((src[0x07] & 0x02) >> 0x01) | ((src[0x06] & 0x02) << 0x00) | ((src[0x05] & 0x02) << 0x01) | ((src[0x04] & 0x02) << 0x02) | ((src[0x03] & 0x02) << 0x03) | ((src[0x02] & 0x02) << 0x04) | ((src[0x01] & 0x02) << 0x05) | ((src[0x00] & 0x02) << 0x06);
	// channel 4
	dst[0x09] = ((src[0x37] & 0x02) >> 0x01) | ((src[0x36] & 0x02) << 0x00) | ((src[0x35] & 0x02) << 0x01) | ((src[0x34] & 0x02) << 0x02) | ((src[0x33] & 0x02) << 0x03) | ((src[0x32] & 0x02) << 0x04) | ((src[0x31] & 0x02) << 0x05) | ((src[0x30] & 0x02) << 0x06);
	dst[0x0A] = ((src[0x2F] & 0x02) >> 0x01) | ((src[0x2E] & 0x02) << 0x00) | ((src[0x2D] & 0x02) << 0x01) | ((src[0x2C] & 0x02) << 0x02) | ((src[0x2B] & 0x02) << 0x03) | ((src[0x2A] & 0x02) << 0x04) | ((src[0x29] & 0x02) << 0x05) | ((src[0x28] & 0x02) << 0x06);
	dst[0x0B] = ((src[0x27] & 0x02) >> 0x01) | ((src[0x26] & 0x02) << 0x00) | ((src[0x25] & 0x02) << 0x01) | ((src[0x24] & 0x02) << 0x02) | ((src[0x23] & 0x02) << 0x03) | ((src[0x22] & 0x02) << 0x04) | ((src[0x21] & 0x02) << 0x05) | ((src[0x20] & 0x02) << 0x06);
	// channel 5
	dst[0x0C] = ((src[0x17] & 0x04) >> 0x02) | ((src[0x16] & 0x04) >> 0x01) | ((src[0x15] & 0x04) << 0x00) | ((src[0x14] & 0x04) << 0x01) | ((src[0x13] & 0x04) << 0x02) | ((src[0x12] & 0x04) << 0x03) | ((src[0x11] & 0x04) << 0x04) | ((src[0x10] & 0x04) << 0x05);
	dst[0x0D] = ((src[0x0F] & 0x04) >> 0x02) | ((src[0x0E] & 0x04) >> 0x01) | ((src[0x0D] & 0x04) << 0x00) | ((src[0x0C] & 0x04) << 0x01) | ((src[0x0B] & 0x04) << 0x02) | ((src[0x0A] & 0x04) << 0x03) | ((src[0x09] & 0x04) << 0x04) | ((src[0x08] & 0x04) << 0x05);
	dst[0x0E] = ((src[0x07] & 0x04) >> 0x02) | ((src[0x06] & 0x04) >> 0x01) | ((src[0x05] & 0x04) << 0x00) | ((src[0x04] & 0x04) << 0x01) | ((src[0x03] & 0x04) << 0x02) | ((src[0x02] & 0x04) << 0x03) | ((src[0x01] & 0x04) << 0x04) | ((src[0x00] & 0x04) << 0x05);
	// channel 6
	dst[0x0F] = ((src[0x37] & 0x04) >> 0x02) | ((src[0x36] & 0x04) >> 0x01) | ((src[0x35] & 0x04) << 0x00) | ((src[0x34] & 0x04) << 0x01) | ((src[0x33] & 0x04) << 0x02) | ((src[0x32] & 0x04) << 0x03) | ((src[0x31] & 0x04) << 0x04) | ((src[0x30] & 0x04) << 0x05);
	dst[0x10] = ((src[0x2F] & 0x04) >> 0x02) | ((src[0x2E] & 0x04) >> 0x01) | ((src[0x2D] & 0x04) << 0x00) | ((src[0x2C] & 0x04) << 0x01) | ((src[0x2B] & 0x04) << 0x02) | ((src[0x2A] & 0x04) << 0x03) | ((src[0x29] & 0x04) << 0x04) | ((src[0x28] & 0x04) << 0x05);
	dst[0x11] = ((src[0x27] & 0x04) >> 0x02) | ((src[0x26] & 0x04) >> 0x01) | ((src[0x25] & 0x04) << 0x00) | ((src[0x24] & 0x04) << 0x01) | ((src[0x23] & 0x04) << 0x02) | ((src[0x22] & 0x04) << 0x03) | ((src[0x21] & 0x04) << 0x04) | ((src[0x20] & 0x04) << 0x05);
	// channel 7
	dst[0x12] = ((src[0x17] & 0x08) >> 0x03) | ((src[0x16] & 0x08) >> 0x02) | ((src[0x15] & 0x08) >> 0x01) | ((src[0x14] & 0x08) << 0x00) | ((src[0x13] & 0x08) << 0x01) | ((src[0x12] & 0x08) << 0x02) | ((src[0x11] & 0x08) << 0x03) | ((src[0x10] & 0x08) << 0x04);
	dst[0x13] = ((src[0x0F] & 0x08) >> 0x03) | ((src[0x0E] & 0x08) >> 0x02) | ((src[0x0D] & 0x08) >> 0x01) | ((src[0x0C] & 0x08) << 0x00) | ((src[0x0B] & 0x08) << 0x01) | ((src[0x0A] & 0x08) << 0x02) | ((src[0x09] & 0x08) << 0x03) | ((src[0x08] & 0x08) << 0x04);
	dst[0x14] = ((src[0x07] & 0x08) >> 0x03) | ((src[0x06] & 0x08) >> 0x02) | ((src[0x05] & 0x08) >> 0x01) | ((src[0x04] & 0x08) << 0x00) | ((src[0x03] & 0x08) << 0x01) | ((src[0x02] & 0x08) << 0x02) | ((src[0x01] & 0x08) << 0x03) | ((src[0x00] & 0x08) << 0x04);
	// channel 8
	dst[0x15] = ((src[0x37] & 0x08) >> 0x03) | ((src[0x36] & 0x08) >> 0x02) | ((src[0x35] & 0x08) >> 0x01) | ((src[0x34] & 0x08) << 0x00) | ((src[0x33] & 0x08) << 0x01) | ((src[0x32] & 0x08) << 0x02) | ((src[0x31] & 0x08) << 0x03) | ((src[0x30] & 0x08) << 0x04);
	dst[0x16] = ((src[0x2F] & 0x08) >> 0x03) | ((src[0x2E] & 0x08) >> 0x02) | ((src[0x2D] & 0x08) >> 0x01) | ((src[0x2C] & 0x08) << 0x00) | ((src[0x2B] & 0x08) << 0x01) | ((src[0x2A] & 0x08) << 0x02) | ((src[0x29] & 0x08) << 0x03) | ((src[0x28] & 0x08) << 0x04);
	dst[0x17] = ((src[0x27] & 0x08) >> 0x03) | ((src[0x26] & 0x08) >> 0x02) | ((src[0x25] & 0x08) >> 0x01) | ((src[0x24] & 0x08) << 0x00) | ((src[0x23] & 0x08) << 0x01) | ((src[0x22] & 0x08) << 0x02) | ((src[0x21] & 0x08) << 0x03) | ((src[0x20] & 0x08) << 0x04);
}
