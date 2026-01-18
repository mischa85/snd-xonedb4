#include "PloytecUSB.h"
#include <IOKit/IOLib.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/mach_time.h>

#define Log(fmt, ...) IOLog("[PloytecUSB] " fmt "\n", ##__VA_ARGS__)
#define MEMORY_BARRIER() __sync_synchronize()

OSDefineMetaClassAndStructors(PloytecUSB, OzzyKext)

// --- CONTROL REQUEST HELPER (Internal) ---
// We pass 'self' to access mHostDevice and use 'self' as the client.
static IOReturn SendControlRequest(PloytecUSB* self, IOUSBHostDevice* dev, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, void* data) {
    StandardUSB::DeviceRequest req;
    req.bmRequestType = bmRequestType;
    req.bRequest = bRequest;
    req.wValue = wValue;
    req.wIndex = wIndex;
    req.wLength = wLength;
    
    IOBufferMemoryDescriptor* mem = nullptr;
    if (wLength > 0 && data) {
        mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIOMemoryPhysicallyContiguous, wLength, 0);
        if (!mem) return kIOReturnNoMemory;
        
        // Host to Device: Copy Data In
        if ((bmRequestType & 0x80) == 0) { 
            mem->writeBytes(0, data, wLength);
        }
        mem->prepare();
    }
    
    uint32_t bytesTransferred = 0;
    // Correct Signature: deviceRequest(IOService* client, DeviceRequest& req, IOMemoryDescriptor* data, uint32_t& bytes, uint32_t timeout)
    IOReturn ret = dev->deviceRequest(self, req, mem, bytesTransferred, 5000);
    
    if (mem) {
        // Device to Host: Copy Data Out
        if (ret == kIOReturnSuccess && (bmRequestType & 0x80)) { 
            mem->readBytes(0, data, wLength);
        }
        mem->complete();
        mem->release();
    }
    return ret;
}

// --- CLASS MEMBER IMPLEMENTATIONS ---

bool PloytecUSB::ReadFirmwareVersion() {
    uint8_t buf[16] = {0};
    if (SendControlRequest(this, mHostDevice, 0xC0, 'V', 0, 0, 0x0F, buf) != kIOReturnSuccess) return false;
    Log("🔹 Firmware: v1.%u.%u (ID:0x%02X)", buf[2]/10, buf[2]%10, buf[0]);
    return true;
}

bool PloytecUSB::ReadHardwareStatus() {
    uint8_t buf[1] = {0};
    if (SendControlRequest(this, mHostDevice, 0xC0, 'I', 0, 0, 1, buf) != kIOReturnSuccess) return false;
    uint8_t s = buf[0];
    Log("🔹 HW Status: [0x%02X] %s %s", s, (s & 0x10) ? "[Armed]" : "[Disarmed]", (s & 0x01) ? "[Stable]" : "[Syncing]");
    return true;
}

bool PloytecUSB::GetHardwareFrameRate() {
    uint8_t buf[3] = {0};
    // 0xA2 = Get Current Sampling Freq
    if (SendControlRequest(this, mHostDevice, 0xA2, 0x81, 0x0100, 0, 3, buf) != kIOReturnSuccess) return false;
    uint32_t rate = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16);
    Log("🔹 Current Rate: %u Hz", rate);
    return true;
}

bool PloytecUSB::SetHardwareFrameRate(uint32_t r) {
    Log("⚡ Setting Rate to %u Hz...", r);
    uint8_t buf[3];
    buf[0]=(uint8_t)r; buf[1]=(uint8_t)(r>>8); buf[2]=(uint8_t)(r>>16);
    
    // 0x22 = Set Current Sampling Freq (Endpoint 0x86)
    SendControlRequest(this, mHostDevice, 0x22, 0x01, 0x0100, 0x0086, 3, buf);
    
    // Dummy sequence from userspace
    SendControlRequest(this, mHostDevice, 0x22, 0x01, 0x0100, 0x0005, 3, buf);
    SendControlRequest(this, mHostDevice, 0x22, 0x01, 0x0100, 0x0086, 3, buf);
    SendControlRequest(this, mHostDevice, 0x22, 0x01, 0x0100, 0x0005, 3, buf);
    
    // Final verify
    return (SendControlRequest(this, mHostDevice, 0x22, 0x01, 0x0100, 0x0086, 3, buf) == kIOReturnSuccess);
}

bool PloytecUSB::WriteHardwareStatus(uint16_t v) {
    Log("⚡ Writing Status: 0x%04X", v);
    // 0x40 = Vendor Request Out
    return (SendControlRequest(this, mHostDevice, 0x40, 'I', v, 0, 0, nullptr) == kIOReturnSuccess);
}

// --- MAIN SETUP ---

bool PloytecUSB::ConfigureHardware() {
	if (mHostDevice->setConfiguration(1) != kIOReturnSuccess) Log("⚠️ setConfig failed");
    IOSleep(200);

	IOUSBHostInterface* iface = nullptr;
	int attempts = 0;
	bool found0 = false, found1 = false;
	const char* planes[] = { "IOService", "IOUSB" };

	while (attempts < 50) { 
		for (int p = 0; p < 2; p++) {
			const IORegistryPlane* plane = getPlane(planes[p]);
			if (!plane) continue;
			OSIterator* it = mHostDevice->getChildIterator(plane);
			if (it) {
				while ((iface = OSDynamicCast(IOUSBHostInterface, it->getNextObject()))) {
					const StandardUSB::InterfaceDescriptor* d = iface->getInterfaceDescriptor();
					if (!d) continue;
					if (d->bInterfaceNumber == 0 && !mIntf0) { 
                        mIntf0 = iface; mIntf0->retain();
                        if (mIntf0->open(this)) {
                            mIntf0->selectAlternateSetting(1); IOSleep(10); found0 = true;
                        } else { mIntf0->release(); mIntf0 = nullptr; }
                    }
                    if (d->bInterfaceNumber == 1 && !mIntf1) { 
                        mIntf1 = iface; mIntf1->retain();
                        if (mIntf1->open(this)) {
                            mIntf1->selectAlternateSetting(1); IOSleep(10); found1 = true;
                        } else { mIntf1->release(); mIntf1 = nullptr; }
                    }
				}
				it->release();
			}
			if (found0 && found1) break;
		}
		if (found0 && found1) break;
		IOSleep(100); attempts++;
	}

	if (!found0 && !found1) return false;

    if (mIntf0) { mPipePcmOut = mIntf0->copyPipe(kPcmOutEp); mPipeMidiIn = mIntf0->copyPipe(kMidiInEp); }
    if (mIntf1) { mPipePcmIn = mIntf1->copyPipe(kPcmInEp); }

	if (mPipePcmOut && mPipePcmIn) {
		mIsBulk = false; 
        mPacketSizeOut = kInterruptPacketSizeOut; 
        mMidiOffset = 432; 
        
		if (mSHM) {
			strlcpy(mSHM->productName, "Xone:DB4", 64);
			mSHM->audio.isBulkMode = mIsBulk; 
            MEMORY_BARRIER();
		}
        
        // --- INITIALIZATION SEQUENCE ---
        ReadFirmwareVersion();
        GetHardwareFrameRate();
        SetHardwareFrameRate(96000);
        GetHardwareFrameRate();
        IOSleep(50);
        WriteHardwareStatus(0xFFB2);
        ReadHardwareStatus();
        // --------------------------------
        
		StartStreaming();
		return true;
	}
	return false;
}

void PloytecUSB::stop(IOService* p) {
	mRunning = false;
	if (mPipePcmOut) mPipePcmOut->abort();
	if (mPipePcmIn) mPipePcmIn->abort();
	if (mPipeMidiIn) mPipeMidiIn->abort();
    IOSleep(10);
	if (mPipePcmOut) { mPipePcmOut->release(); mPipePcmOut = nullptr; }
	if (mPipePcmIn) { mPipePcmIn->release(); mPipePcmIn = nullptr; }
	if (mPipeMidiIn) { mPipeMidiIn->release(); mPipeMidiIn = nullptr; }
    if (mIntf0) { mIntf0->close(this); mIntf0->release(); mIntf0 = nullptr; }
    if (mIntf1) { mIntf1->close(this); mIntf1->release(); mIntf1 = nullptr; }
	OzzyKext::stop(p);
}

void PloytecUSB::StartStreaming() {
	mRunning = true;
	if (!mSHM) return;

    mSHM->audio.hardwarePresent = true;
    mSHM->audio.driverReady = false;
    mHwSampleTime = 0;
    mSHM->audio.timestamp.sampleTime = 0;
    mSHM->audio.halWritePosition = 0;
    
    memset((void*)mSHM->audio.inputBuffer, 0, sizeof(mSHM->audio.inputBuffer));
    memset((void*)mSHM->audio.outputBuffer, 0, sizeof(mSHM->audio.outputBuffer));

    uint32_t stride = mIsBulk ? 512 : 482;
    uint32_t startOffset = mIsBulk ? 480 : 432;
    uint32_t limit = kNumPackets * mPacketSizeOut;

    for (uint32_t i = startOffset; i + 1 < limit; i += stride) {
        mSHM->audio.outputBuffer[i] = 0xFD;
        mSHM->audio.outputBuffer[i+1] = 0xFD;
    }

	if (mPipeMidiIn) SubmitMIDIin();
	for (int i=0; i<kDefaultUrbs; i++) {
		SubmitPCMout((uint32_t)i);
		SubmitPCMin((uint32_t)i);
	}
    
    MEMORY_BARRIER();
    mSHM->audio.driverReady = true; 
    Log("🚀 Streaming Active!");
}

void PloytecUSB::SubmitPCMout(uint32_t idx) {
	if (!mRunning) return;
	uint32_t pIdx = idx & kPacketMask;
	
	size_t offset = offsetof(PloytecSharedMemory, audio) + offsetof(AudioRingBuffer, outputBuffer) + (pIdx * mPacketSizeOut);
	
    if (mIsBulk) {
        uint8_t* rawBuf = (uint8_t*)mSHM + offset;
        uint32_t r = mSHM->midiOut.readIndex;
        uint32_t w = mSHM->midiOut.writeIndex;
        
        if (r != w) {
            rawBuf[mMidiOffset] = mSHM->midiOut.buffer[r];
            mSHM->midiOut.readIndex = (r+1) & kMidiRingMask;
        } else {
            rawBuf[mMidiOffset] = 0xFD; 
        }
        rawBuf[mMidiOffset+1] = 0xFD; 
    }

	IOMemoryDescriptor* d = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)mSHM + offset, mPacketSizeOut, kIODirectionOut, kernel_task);
	if (d) {
		d->prepare();
		IOUSBHostCompletion completion;
		completion.owner = this;
		completion.action = (IOUSBHostCompletionAction)&PloytecUSB::PCMoutComplete;
		completion.parameter = (void*)(uintptr_t)idx;
		
        IOReturn kr = mPipePcmOut->io(d, mPacketSizeOut, &completion);
        if (kr != kIOReturnSuccess) Log("❌ PcmOut IO Error: 0x%08x", kr);
        
		d->release();
	}
}

void PloytecUSB::PCMoutComplete(OSObject* target, void* parameter, IOReturn status, uint32_t bytesTransferred) {
	PloytecUSB* self = (PloytecUSB*)target;
    
    if (status != kIOReturnSuccess && status != kIOReturnAborted) {
        Log("⚠️ PcmOut Completion Error: 0x%08x", status);
    }

	if (self->mRunning) {
		self->SubmitPCMout((uint32_t)(uintptr_t)parameter + kDefaultUrbs);
	}

	if (self->mRunning && status == kIOReturnSuccess) {
        if (self->mSHM->audio.driverReady) {
            self->mHwSampleTime += kFramesPerPacket;
            
            if ((self->mHwSampleTime % kZeroTimestampPeriod) == 0) {
                volatile AudioRingBuffer* audio = &self->mSHM->audio;
                
                uint32_t seq = audio->timestamp.sequence;
                audio->timestamp.sequence = seq + 1;
                MEMORY_BARRIER(); 
                
                audio->timestamp.sampleTime = self->mHwSampleTime;
                audio->timestamp.hostTime = mach_absolute_time();
                
                MEMORY_BARRIER(); 
                audio->timestamp.sequence = seq + 2;
            }
        }
	}
}

void PloytecUSB::SubmitPCMin(uint32_t idx) {
	if (!mRunning) return;
	uint32_t pIdx = idx & kPacketMask;
	size_t offset = offsetof(PloytecSharedMemory, audio) + offsetof(AudioRingBuffer, inputBuffer) + (pIdx * kPacketSizeIn);
	IOMemoryDescriptor* d = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)mSHM + offset, kPacketSizeIn, kIODirectionIn, kernel_task);
	if (d) {
		d->prepare();
		IOUSBHostCompletion completion;
		completion.owner = this;
		completion.action = (IOUSBHostCompletionAction)&PloytecUSB::PCMinComplete;
		completion.parameter = (void*)(uintptr_t)idx;
		
        IOReturn kr = mPipePcmIn->io(d, kPacketSizeIn, &completion);
        if (kr != kIOReturnSuccess) Log("❌ PcmIn IO Error: 0x%08x", kr);
        
		d->release();
	}
}

void PloytecUSB::PCMinComplete(OSObject* target, void* parameter, IOReturn status, uint32_t bytesTransferred) {
	PloytecUSB* self = (PloytecUSB*)target;
	if (self->mRunning && status == kIOReturnSuccess) {
		self->SubmitPCMin((uint32_t)(uintptr_t)parameter + kDefaultUrbs);
	}
}

void PloytecUSB::SubmitMIDIin() {
	if (!mRunning || !mPipeMidiIn) return;
	uint32_t w = mSHM->midiIn.writeIndex;
	size_t offset = offsetof(PloytecSharedMemory, midiIn) + offsetof(MidiRingBuffer, buffer) + w;
	IOMemoryDescriptor* d = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)mSHM + offset, 1, kIODirectionIn, kernel_task);
	if (d) {
		d->prepare();
		IOUSBHostCompletion completion;
		completion.owner = this;
		completion.action = (IOUSBHostCompletionAction)&PloytecUSB::MIDIinComplete;
		completion.parameter = (void*)(uintptr_t)w;
		
        mPipeMidiIn->io(d, 1, &completion);
		d->release();
	}
}

void PloytecUSB::MIDIinComplete(OSObject* target, void* parameter, IOReturn status, uint32_t bytesTransferred) {
	PloytecUSB* self = (PloytecUSB*)target;
	uint32_t wSlot = (uint32_t)(uintptr_t)parameter;
	if (self->mRunning && status == kIOReturnSuccess) {
		if (self->mSHM->midiIn.buffer[wSlot] != 0xFD) {
			self->mSHM->midiIn.writeIndex = (wSlot + 1) & kMidiRingMask;
		}
		self->SubmitMIDIin();
	} else if (self->mRunning) {
		self->SubmitMIDIin(); 
	}
}