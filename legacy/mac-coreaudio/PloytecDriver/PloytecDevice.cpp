#include <AudioDriverKit/AudioDriverKit.h>
#include "PloytecDevice.h"

static constexpr uint32_t INITIAL_BUFFERSIZE = 2560;
static constexpr uint32_t MAX_BUFFERSIZE = 800000;
static constexpr uint8_t PLOYTEC_PCM_OUT_FRAME_SIZE = 48;
static constexpr uint8_t PLOYTEC_PCM_IN_FRAME_SIZE = 64;
static constexpr uint8_t COREAUDIO_BYTES_PER_SAMPLE = 3; // S24_3LE

struct PloytecDevice_IVars
{
	OSSharedPtr<IODispatchQueue>		m_work_queue;
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
	uint64_t				in_sample_time;
	uint64_t				out_sample_time;
	
	uint32_t				CoreAudioPlaybackBytesPerFrame;
	uint32_t				CoreAudioCaptureBytesPerFrame;

	bool					IOStarted;
	bool					PCMinActive;
	bool					PCMoutActive;

	uint64_t				xruns;

	uint64_t				lastZeroReported;
};

bool PloytecDevice::init(IOUserAudioDriver* in_driver, bool in_supports_prewarming, OSString* in_device_uid, OSString* in_model_uid, OSString* in_manufacturer_uid, uint32_t in_zero_timestamp_period, uint32_t inChannelCount, uint32_t outChannelCount, IOBufferMemoryDescriptor* receiveBuffer, IOBufferMemoryDescriptor* transmitBuffer, TransferMode transferMode)
{
	bool ok = super::init(in_driver, in_supports_prewarming, in_device_uid, in_model_uid, in_manufacturer_uid, in_zero_timestamp_period);
	if (!ok) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: super::init failed"); return false; }

	ivars = IONewZero(PloytecDevice_IVars, 1);
	if (ivars == nullptr) {	os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to alloc ivars"); return false; }

	auto cleanup = [&]() {
		ivars->m_output_stream.reset();
		ivars->m_input_stream.reset();
		ivars->m_output_memory_map.reset();
		ivars->m_input_memory_map.reset();
	};

	ivars->CoreAudioPlaybackBytesPerFrame = outChannelCount * COREAUDIO_BYTES_PER_SAMPLE;
	ivars->CoreAudioCaptureBytesPerFrame = inChannelCount * COREAUDIO_BYTES_PER_SAMPLE;

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
	double sample_rates[] = { 96000 };
	IOUserAudioStreamBasicDescription outputStreamFormats[] =
	{
		{
			// S24_3LE
			96000,
			IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			ivars->CoreAudioPlaybackBytesPerFrame,
			1,
			ivars->CoreAudioPlaybackBytesPerFrame,
			outChannelCount,
			24
		},
	};
	IOUserAudioStreamBasicDescription inputStreamFormats[] =
	{
		{
			// S24_3LE
			96000,
			IOUserAudioFormatID::LinearPCM,
			static_cast<IOUserAudioFormatFlags>(IOUserAudioFormatFlags::FormatFlagIsSignedInteger | IOUserAudioFormatFlags::FormatFlagsNativeEndian),
			ivars->CoreAudioCaptureBytesPerFrame,
			1,
			ivars->CoreAudioCaptureBytesPerFrame,
			inChannelCount,
			24
		},
	};

	// allocate the USB ring buffers
	ret = transmitBuffer->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to get address of output ring buffer: %{public}s", strerror(ret)); cleanup(); return false; }
	ivars->PloytecOutputBufferAddr = reinterpret_cast<uint8_t*>(range.address);
	ret = receiveBuffer->GetAddressRange(&range);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to get address of input ring buffer: %{public}s", strerror(ret)); cleanup(); return false; }
	ivars->PloytecInputBufferAddr = reinterpret_cast<uint8_t*>(range.address);

	// set the available sample rates in CoreAudio
	ret = SetAvailableSampleRates(sample_rates, 1);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set available samplerates: %{public}s", strerror(ret)); cleanup(); return false; }
	
	ret = SetOutputSafetyOffset(100);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set output safety offset: %{public}s", strerror(ret)); cleanup(); return false; }

	// set the current sample rate in CoreAudio
	ret = SetSampleRate(96000);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set samplerate: %{public}s", strerror(ret)); cleanup(); return false; }

	// set the transport type in CoreAudio
	ret = SetTransportType(IOUserAudioTransportType::USB);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set transport type: %{public}s", strerror(ret)); cleanup(); return false; }

	// allocate the CoreAudio ring buffers
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, MAX_BUFFERSIZE * ivars->CoreAudioPlaybackBytesPerFrame, 0, ivars->output_io_ring_buffer.attach());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to create output ring buffer: %{public}s", strerror(ret)); cleanup(); return false; }
	ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, MAX_BUFFERSIZE * ivars->CoreAudioPlaybackBytesPerFrame, 0, ivars->input_io_ring_buffer.attach());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to create input ring buffer: %{public}s", strerror(ret)); cleanup(); return false; }

	// create the streams
	ivars->m_output_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Output, ivars->output_io_ring_buffer.get());
	if (!ivars->m_output_stream.get()) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to create output stream"); cleanup(); return false; }
	ivars->m_input_stream = IOUserAudioStream::Create(in_driver, IOUserAudioStreamDirection::Input, ivars->input_io_ring_buffer.get());
	if (!ivars->m_input_stream.get()) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to create input stream"); cleanup(); return false; }

	// set names on input/output streams
	ret = ivars->m_output_stream->SetName(output_stream_name.get());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set name on output stream: %{public}s", strerror(ret)); cleanup(); return false; }
	ret = ivars->m_input_stream->SetName(input_stream_name.get());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set name on input stream: %{public}s", strerror(ret)); cleanup(); return false; }

	// set the available sample formats on the streams
	ret = ivars->m_output_stream->SetAvailableStreamFormats(outputStreamFormats, 1);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set available formats on output stream: %{public}s", strerror(ret)); cleanup(); return false; }
	ret = ivars->m_input_stream->SetAvailableStreamFormats(inputStreamFormats, 1);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set available formats on input stream: %{public}s", strerror(ret)); cleanup(); return false; }

	// set the current stream format on the stream
	ret = ivars->m_output_stream->SetCurrentStreamFormat(&outputStreamFormats[0]);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set current format on output stream: %{public}s", strerror(ret)); cleanup(); return false; }
	ret = ivars->m_input_stream->SetCurrentStreamFormat(&inputStreamFormats[0]);
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to set current format on input stream: %{public}s", strerror(ret)); cleanup(); return false; }

	// add the streams to CoreAudio
	ret = AddStream(ivars->m_output_stream.get());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to add output stream: %{public}s", strerror(ret)); cleanup(); return false; }
	ret = AddStream(ivars->m_input_stream.get());
	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::init: failed to add input stream: %{public}s", strerror(ret)); cleanup(); return false; }

	// this block converts the audio from CoreAudioOutputBuffer to the format the USB device wants in PloytecOutputBuffer
	ioOperationBulk = ^kern_return_t(IOUserAudioObjectID in_device, IOUserAudioIOOperation in_io_operation, uint32_t in_io_buffer_frame_size, uint64_t in_sample_time, uint64_t in_host_time)
	{
		// playback
		if (in_io_operation == IOUserAudioIOOperationWriteEnd)
		{
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

				EncodePloytecPCM(ivars->PloytecOutputBufferAddr + byteoffset + (sampleoffset * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->CoreAudioOutputBufferAddr + (sampleoffset * ivars->CoreAudioPlaybackBytesPerFrame));
			}
		}
		// capture
		else if (in_io_operation == IOUserAudioIOOperationBeginRead)
		{
			ivars->PCMinActive = true;
			ivars->in_sample_time = in_sample_time;

			for (int i = 0; i < in_io_buffer_frame_size; i++)
			{
				DecodePloytecPCM(ivars->CoreAudioInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * ivars->CoreAudioCaptureBytesPerFrame), ivars->PloytecInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * PLOYTEC_PCM_IN_FRAME_SIZE));
			}
		}

		return kIOReturnSuccess;
	};

	ioOperationInterrupt = ^kern_return_t(IOUserAudioObjectID in_device, IOUserAudioIOOperation in_io_operation, uint32_t in_io_buffer_frame_size, uint64_t in_sample_time, uint64_t in_host_time)
	{
		// playback
		if (in_io_operation == IOUserAudioIOOperationWriteEnd)
		{
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

				EncodePloytecPCM(ivars->PloytecOutputBufferAddr + byteoffset + (sampleoffset * PLOYTEC_PCM_OUT_FRAME_SIZE), ivars->CoreAudioOutputBufferAddr + (sampleoffset * ivars->CoreAudioPlaybackBytesPerFrame));
			}
		}
		// capture
		else if (in_io_operation == IOUserAudioIOOperationBeginRead)
		{
			ivars->PCMinActive = true;
			ivars->in_sample_time = in_sample_time;

			for (int i = 0; i < in_io_buffer_frame_size; i++)
			{
				DecodePloytecPCM(ivars->CoreAudioInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * ivars->CoreAudioCaptureBytesPerFrame), ivars->PloytecInputBufferAddr + (((in_sample_time + i) % GetZeroTimestampPeriod()) * PLOYTEC_PCM_IN_FRAME_SIZE));
			}
		}
		
		return kIOReturnSuccess;
	};

	if (transferMode == INTERRUPT)
	{
		ret = this->SetIOOperationHandler(ioOperationInterrupt);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: Cannot set IOOperationHandler: %{public}s", strerror(ret)); return false; }
	}
	else if (transferMode == BULK)
	{
		ret = this->SetIOOperationHandler(ioOperationBulk);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: Cannot set IOOperationHandler: %{public}s", strerror(ret)); return false; }
	}
	
	return true;
}

kern_return_t PloytecDevice::StartIO(IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret = kIOReturnSuccess;

	ivars->m_work_queue->DispatchSync(^{
		ret = super::StartIO(in_flags);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: super::StartIO failed: %{public}s", strerror(ret)); return; }

		auto cleanup = [&](){
		    ivars->IOStarted = false;
		    ivars->PCMinActive = false;
		    ivars->PCMoutActive = false;

		    if (ivars->m_output_memory_map) ivars->m_output_memory_map.reset();
		    if (ivars->m_input_memory_map) ivars->m_input_memory_map.reset();

		    ivars->CoreAudioOutputBufferAddr = nullptr;
		    ivars->CoreAudioInputBufferAddr = nullptr;

		    (void)super::StopIO(in_flags);
		};

		OSSharedPtr<IOMemoryDescriptor> output_iomd = ivars->m_output_stream->GetIOMemoryDescriptor();
		if (!output_iomd.get()) { ret = kIOReturnNoMemory; os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: no output IOMD"); cleanup(); return; }

		ret = output_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_output_memory_map.attach());
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: output mapping failed: %{public}s", strerror(ret)); cleanup(); return; }
		ivars->CoreAudioOutputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_output_memory_map->GetAddress() + ivars->m_output_memory_map->GetOffset());

		OSSharedPtr<IOMemoryDescriptor> input_iomd = ivars->m_input_stream->GetIOMemoryDescriptor();
		if (!input_iomd.get()) { ret = kIOReturnNoMemory; os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: no input IOMD"); cleanup(); return; }

		ret = input_iomd->CreateMapping(0, 0, 0, 0, 0, ivars->m_input_memory_map.attach());
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: input mapping failed: %{public}s", strerror(ret)); cleanup(); return; }
		ivars->CoreAudioInputBufferAddr = reinterpret_cast<uint8_t*>(ivars->m_input_memory_map->GetAddress() + ivars->m_input_memory_map->GetOffset());

		ivars->HWSampleTimeOut = 0;
		ivars->HWSampleTimeIn = 0;
		ivars->lastZeroReported = UINT64_MAX;
		ivars->IOStarted = true;
		ivars->PCMinActive = false;
		ivars->PCMoutActive = false;

		ret = ivars->m_output_stream->SetStreamIsActive(true);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: Set output stream active: %{public}s", strerror(ret)); cleanup(); return; }

		ret = ivars->m_input_stream->SetStreamIsActive(true);
		if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StartIO: Set input stream active: %{public}s", strerror(ret)); cleanup(); return; }
	});

	return ret;
}

kern_return_t PloytecDevice::StopIO(IOUserAudioStartStopFlags in_flags)
{
	__block kern_return_t ret = kIOReturnSuccess;

	ivars->m_work_queue->DispatchSync(^{
		ivars->IOStarted = false;
		ivars->PCMinActive = false;
		ivars->PCMoutActive = false;

		if (ivars->m_output_memory_map) ivars->m_output_memory_map.reset();
		if (ivars->m_input_memory_map) ivars->m_input_memory_map.reset();
		ivars->CoreAudioOutputBufferAddr = nullptr;
		ivars->CoreAudioInputBufferAddr = nullptr;

		ret = super::StopIO(in_flags);
	});

	if (ret != kIOReturnSuccess) { os_log(OS_LOG_DEFAULT, "PloytecDevice::StopIO: super::StopIO failed: %{public}s", strerror(ret)); return ret; }
	return ret;
}

kern_return_t PloytecDevice::PerformDeviceConfigurationChange(uint64_t change_action, OSObject* in_change_info)
{
	kern_return_t ret = kIOReturnSuccess;
	switch (change_action)
	{
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

	if (ivars->IOStarted)
	{
		ivars->HWSampleTimeOut += frameCount;
		const uint64_t expectedZero = (ivars->HWSampleTimeOut / period) * period;
		if (expectedZero != ivars->lastZeroReported)
		{
			UpdateCurrentZeroTimestamp(expectedZero, completionTimestamp);
			ivars->lastZeroReported = expectedZero;
		}
	}

	return true;
}

bool PloytecDevice::Capture(uint16_t &currentpos, uint16_t frameCount, uint64_t completionTimestamp)
{
	currentpos = (ivars->HWSampleTimeIn % GetZeroTimestampPeriod()) / frameCount;
	ivars->HWSampleTimeIn += frameCount;
	return true;
}

void PloytecDevice::SetDispatchQueue(IODispatchQueue* q){ if (q) ivars->m_work_queue = OSSharedPtr<IODispatchQueue>(q, OSRetain); }

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
