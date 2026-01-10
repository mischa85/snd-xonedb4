#include "PloytecUSB.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <mach/thread_policy.h>

os_log_t gUSBEngineLog = nullptr;

static constexpr uint32_t kXone4DFixedRate = 96000;

static void PromoteToRealTime() {
	mach_timebase_info_data_t tb; 
	mach_timebase_info(&tb);
	uint64_t p = (3333332ull * tb.denom) / tb.numer;
	thread_time_constraint_policy_data_t pol = { 
		(uint32_t)p, 
		(uint32_t)((400000ull * tb.denom)/tb.numer), 
		(uint32_t)p, 
		1 
	};
	kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (kr != KERN_SUCCESS) {
		os_log_error(GetLog(), "[PloytecUSB] 🛑 Failed to set real-time policy: 0x%08X", kr);
	} else {
		os_log_info(GetLog(), "[PloytecUSB] ✅ Real-time policy set");
	}
}

static void GetRegistryString(io_service_t service, CFStringRef key, char* dest, size_t maxLen) {
	CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	if (ref) {
		if (CFGetTypeID(ref) == CFStringGetTypeID()) {
			CFStringGetCString((CFStringRef)ref, dest, maxLen, kCFStringEncodingUTF8);
		}
		CFRelease(ref);
	}
}

PloytecUSB::PloytecUSB(io_service_t service) : mService(service) {
	IOObjectRetain(mService);
	mRxBufferControl = (uint8_t*)calloc(1, 64);
	mTxBufferControl = (uint8_t*)calloc(1, 64);
	
	InitSharedMemory();
	
	if (OpenDevice()) {
		os_log(GetLog(), "[PloytecUSB] Device Opened. Starting Engine.");
		mRunning = true;
		mThread = std::thread(&PloytecUSB::EngineThread, this);
	}
}

PloytecUSB::~PloytecUSB() {
	mRunning = false;
	if (mThreadRunLoop) CFRunLoopStop(mThreadRunLoop);
	if (mThread.joinable()) mThread.join();
	
	CloseDevice();
	DestroySharedMemory();
	
	if (mRxBufferControl) free(mRxBufferControl);
	if (mTxBufferControl) free(mTxBufferControl);
	IOObjectRelease(mService);
}

void PloytecUSB::PulseHeartbeat() {
	if (mSHM) mSHM->heartbeat.fetch_add(1, std::memory_order_relaxed);
}

bool PloytecUSB::MatchesService(io_service_t service) {
	return IOObjectIsEqualTo(mService, service);
}

void PloytecUSB::InitSharedMemory() {
	mShmName = kPloytecSharedMemName;
	shm_unlink(mShmName.c_str());
	mShmFd = shm_open(mShmName.c_str(), O_CREAT | O_RDWR, 0666);
	if (mShmFd == -1) { os_log_error(GetLog(), "SHM Fail"); return; }
	ftruncate(mShmFd, sizeof(PloytecSharedMemory));
	mSHM = (PloytecSharedMemory*)mmap(NULL, sizeof(PloytecSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, mShmFd, 0);
	memset(mSHM, 0, sizeof(PloytecSharedMemory));
	mSHM->magic = 0x504C4F59; mSHM->version = 1; mSHM->sessionID = (uint32_t)time(NULL);
	mSHM->audio.hardwarePresent.store(false);
}

void PloytecUSB::DestroySharedMemory() {
	if (mSHM) { mSHM->audio.hardwarePresent.store(false); munmap(mSHM, sizeof(PloytecSharedMemory)); }
	if (mShmFd != -1) close(mShmFd);
	shm_unlink(mShmName.c_str());
}

void PloytecUSB::EngineThread() {
	PromoteToRealTime();
	mThreadRunLoop = CFRunLoopGetCurrent(); CFRetain(mThreadRunLoop);
	
	if (CreatePipes() && DetectTransferMode()) {
		if (mSHM) mSHM->audio.isBulkMode.store((mTransferMode == kTransferModeBulk), std::memory_order_release);
		ReadFirmwareVersion();
		GetHardwareFrameRate();
		SetHardwareFrameRate(96000);
		GetHardwareFrameRate();
		usleep(50000);
		WriteHardwareStatus(0xFFB2);
		ReadHardwareStatus();
		
		if (StartStreaming(kDefaultUrbs)) {
			if (mSHM) mSHM->audio.hardwarePresent.store(true, std::memory_order_release);
			while (mRunning) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
		}
	}
	StopStreaming();
	if (mSHM) mSHM->audio.hardwarePresent.store(false, std::memory_order_release);
	
	if (mInterface0 && mIf0Src) { CFRunLoopRemoveSource(mThreadRunLoop, mIf0Src, kCFRunLoopDefaultMode); CFRelease(mIf0Src); mIf0Src=nullptr; }
	if (mInterface1 && mIf1Src) { CFRunLoopRemoveSource(mThreadRunLoop, mIf1Src, kCFRunLoopDefaultMode); CFRelease(mIf1Src); mIf1Src=nullptr; }
	CFRelease(mThreadRunLoop); mThreadRunLoop = nullptr;
}

bool PloytecUSB::OpenDevice() {
	IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
	IOCreatePlugInInterfaceForService(mService, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
	if (!plug) return false;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID*)&mDevice);
	(*plug)->Release(plug);
	if (!mDevice) return false;
	
	IOReturn ret = kIOReturnError;
	for (int i=0; i<5; i++) { ret = (*mDevice)->USBDeviceOpenSeize(mDevice); if (ret == kIOReturnSuccess) break; usleep(100000); }
	if (ret != kIOReturnSuccess) return false;
	
	(*mDevice)->GetDeviceVendor(mDevice, &mVendorID);
	(*mDevice)->GetDeviceProduct(mDevice, &mProductID);

	if (mSHM) {
		GetRegistryString(mService, CFSTR("USB Vendor Name"), mSHM->manufacturerName, 64);
		GetRegistryString(mService, CFSTR("USB Product Name"), mSHM->productName, 64);
		GetRegistryString(mService, CFSTR("USB Serial Number"), mSHM->serialNumber, 64);
		
		if (mSHM->manufacturerName[0] == 0) strlcpy(mSHM->manufacturerName, "Ploytec", 64);
		if (mSHM->productName[0] == 0) strlcpy(mSHM->productName, "USB Audio Device", 64);
		
		os_log(GetLog(), "[PloytecUSB] Info: %{public}s - %{public}s (SN: %{public}s)", 
			   mSHM->manufacturerName, mSHM->productName, mSHM->serialNumber);
	}
	return true; // 🟢 Ensures engine starts
}

void PloytecUSB::CloseDevice() {
	if (mInterface0) { (*mInterface0)->USBInterfaceClose(mInterface0); (*mInterface0)->Release(mInterface0); mInterface0=nullptr; }
	if (mInterface1) { (*mInterface1)->USBInterfaceClose(mInterface1); (*mInterface1)->Release(mInterface1); mInterface1=nullptr; }
	if (mDevice) { (*mDevice)->USBDeviceClose(mDevice); (*mDevice)->Release(mDevice); mDevice=nullptr; }
}

static IOUSBInterfaceInterface** OpenInterface(io_service_t svc) {
	IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
	if (IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) != 0) return nullptr;
	IOUSBInterfaceInterface** intf = nullptr;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID*)&intf);
	(*plug)->Release(plug);
	if (intf && (*intf)->USBInterfaceOpen(intf) != 0) { (*intf)->Release(intf); return nullptr; }
	return intf;
}

static bool FindPipe(IOUSBInterfaceInterface** intf, UInt8 addr, UInt8* p) {
	UInt8 n=0; (*intf)->GetNumEndpoints(intf, &n);
	for (UInt8 i=1; i<=n; ++i) {
		UInt8 d=0, num=0, tt=0, intv=0; UInt16 mps=0;
		if ((*intf)->GetPipeProperties(intf, i, &d, &num, &tt, &mps, &intv) == 0) {
			// Construct actual endpoint address (0x80 | num) or (0x00 | num)
			UInt8 epAddr = (UInt8)((d ? 0x80 : 0) | (num & 0xF));
			if (epAddr == addr) { *p = i; return true; }
		}
	}
	return false;
}

bool PloytecUSB::CreatePipes() {
	os_log(GetLog(), "[PloytecUSB] Creating USB Pipes...");

	uint8_t c = 0;
	(*mDevice)->GetConfiguration(mDevice, &c);
	if (c != 1) {
		os_log(GetLog(), "[PloytecUSB] Device unconfigured. Setting Config 1...");
		(*mDevice)->SetConfiguration(mDevice, 1);
		usleep(200000);
	} else {
		os_log(GetLog(), "[PloytecUSB] Config is already 1. Checking registry...");
	}

	for (int i = 1; i <= 15; i++) {
		if (i == 2) {
			os_log(GetLog(), "[PloytecUSB] ⚠️ Interfaces missing (Zombie). Forcing Reset...");
			(*mDevice)->SetConfiguration(mDevice, 1);
			usleep(200000);
		}

		IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
		io_iterator_t it;
		if ((*mDevice)->CreateInterfaceIterator(mDevice, &req, &it) != 0) return false;

		io_service_t svc;
		bool found = false;
		while((svc = IOIteratorNext(it))) {
			found = true;
			IOUSBInterfaceInterface** intf = OpenInterface(svc); IOObjectRelease(svc);
			if(!intf) continue;

			(*intf)->SetAlternateInterface(intf, 1);
			UInt8 pM=0, pO=0, pI=0;
			FindPipe(intf, kMidiInEp, &pM); FindPipe(intf, kPcmOutEp, &pO); FindPipe(intf, kPcmInEp, &pI);

			if (pM || pO || pI) {
				if (!mInterface0) mInterface0 = intf; else mInterface1 = intf;
				if(pM) { mMidiInPipe=pM; mMidiInIf=(mInterface0==intf)?0:1; }
				if(pO) { mPcmOutPipe=pO; mPcmOutIf=(mInterface0==intf)?0:1; }
				if(pI) { mPcmInPipe=pI;  mPcmInIf=(mInterface0==intf)?0:1; }
			} else {
				(*intf)->USBInterfaceClose(intf); (*intf)->Release(intf);
			}
		}
		IOObjectRelease(it);

		if (mMidiInPipe && mPcmOutPipe && mPcmInPipe) {
			os_log(GetLog(), "[PloytecUSB] ✅ Pipes Created on Attempt %d", i);
			if (mInterface0) (*mInterface0)->CreateInterfaceAsyncEventSource(mInterface0, &mIf0Src);
			if (mInterface1) (*mInterface1)->CreateInterfaceAsyncEventSource(mInterface1, &mIf1Src);
			if (mIf0Src) CFRunLoopAddSource(mThreadRunLoop, mIf0Src, kCFRunLoopDefaultMode);
			if (mIf1Src) CFRunLoopAddSource(mThreadRunLoop, mIf1Src, kCFRunLoopDefaultMode);
			return true;
		}

		if (mInterface0) { (*mInterface0)->USBInterfaceClose(mInterface0); (*mInterface0)->Release(mInterface0); mInterface0=nullptr; }
		if (mInterface1) { (*mInterface1)->USBInterfaceClose(mInterface1); (*mInterface1)->Release(mInterface1); mInterface1=nullptr; }

		if (found) os_log(GetLog(), "[PloytecUSB] ⚠️ Attempt %d: Locked. Retrying...", i);
		else os_log(GetLog(), "[PloytecUSB] ⚠️ Attempt %d: Registry empty. Retrying...", i);
		usleep(100000);
	}

	os_log_error(GetLog(), "[PloytecUSB] ❌ Failed to create pipes after 15 attempts.");
	return false;
}

bool PloytecUSB::DetectTransferMode() {
	IOUSBInterfaceInterface** i = (mPcmOutIf==0)?mInterface0:mInterface1;
	if (!i) return false;
	UInt8 d=0, n=0, tt=0, iv=0; UInt16 mps=0;
	(*i)->GetPipeProperties(i, mPcmOutPipe, &d, &n, &tt, &mps, &iv);
	mTransferMode = (tt == kUSBBulk) ? kTransferModeBulk : kTransferModeInterrupt;
	mMidiByteOffset = (mTransferMode == kTransferModeBulk) ? 480 : 432;
	os_log(GetLog(), "[PloytecUSB] Mode: %s", (mTransferMode == kTransferModeBulk) ? "BULK" : "INTERRUPT");
	return true;
}

bool PloytecUSB::ConfigureStreamingFormat() {
	mPacketSizeOut = (mTransferMode == kTransferModeBulk) ? kBulkPacketSizeOut : kInterruptPacketSizeOut;
	mPacketSizeIn = kPacketSizeIn;
	return true;
}

bool PloytecUSB::StartStreaming(uint8_t urbCount) {
	if (!mSHM) return false;
	mSHM->audio.driverReady.store(false); mUsbShutdownInProgress.store(false); ConfigureStreamingFormat();
	memset(mSHM->audio.inputBuffer, 0, sizeof(mSHM->audio.inputBuffer));
	memset(mSHM->audio.outputBuffer, 0, sizeof(mSHM->audio.outputBuffer));
	uint32_t s = (mTransferMode==kTransferModeBulk)?512:482, off=(mTransferMode==kTransferModeBulk)?480:432;
	for (uint32_t i=off; i+1 < kNumPackets*mPacketSizeOut; i+=s) { mSHM->audio.outputBuffer[i]=0xFD; mSHM->audio.outputBuffer[i+1]=0xFD; }
	mInputSequence=0; mOutputSequence=0; mHwSampleTime=0; mAnchorHostTime=0; mWasDriverReady=false;
	mSHM->audio.timestamp.sampleTime.store(0); mSHM->audio.halWritePosition.store(0);
	SubmitMIDIin(); for(uint8_t i=0; i<urbCount; ++i) { SubmitPCMin(i); SubmitPCMout(i); }
	mSHM->audio.driverReady.store(true, std::memory_order_release);
	return true;
}

bool PloytecUSB::StopStreaming() {
	mUsbShutdownInProgress.store(true);
	if (mInterface0) { (*mInterface0)->AbortPipe(mInterface0, mPcmInPipe); (*mInterface0)->AbortPipe(mInterface0, mPcmOutPipe); (*mInterface0)->AbortPipe(mInterface0, mMidiInPipe); }
	if (mInterface1) { (*mInterface1)->AbortPipe(mInterface1, mPcmInPipe); (*mInterface1)->AbortPipe(mInterface1, mPcmOutPipe); (*mInterface1)->AbortPipe(mInterface1, mMidiInPipe); }
	return true;
}

bool PloytecUSB::SubmitPCMin(uint32_t idx) {
	IOUSBInterfaceInterface** i = (mPcmInIf==0)?mInterface0:mInterface1;
	return ((*i)->ReadPipeAsync(i, mPcmInPipe, mSHM->audio.inputBuffer + ((idx&kPacketMask)*mPacketSizeIn), mPacketSizeIn, PCMinComplete, this) == kIOReturnSuccess);
}
void PloytecUSB::PCMinComplete(void* r, IOReturn res, void*) {
	auto* s = (PloytecUSB*)r; if (s->mUsbShutdownInProgress) return;
	if (res != kIOReturnAborted) s->SubmitPCMin((uint32_t)(s->mInputSequence.fetch_add(1) + kDefaultUrbs));
}

bool PloytecUSB::SubmitPCMout(uint32_t idx) {
	IOUSBInterfaceInterface** i = (mPcmOutIf==0)?mInterface0:mInterface1;
	uint8_t* buf = mSHM->audio.outputBuffer + ((idx&kPacketMask)*mPacketSizeOut);
	if (mTransferMode == kTransferModeBulk) {
		uint32_t r = std::atomic_load_explicit(&mSHM->midiOut.readIndex, std::memory_order_relaxed);
		uint32_t w = std::atomic_load_explicit(&mSHM->midiOut.writeIndex, std::memory_order_acquire);
		if (r != w) { buf[mMidiByteOffset]=mSHM->midiOut.buffer[r]; std::atomic_store_explicit(&mSHM->midiOut.readIndex, (r+1)&kMidiRingMask, std::memory_order_release); }
		else { buf[mMidiByteOffset]=0xFD; } buf[mMidiByteOffset+1]=0xFD;
	}
	return ((*i)->WritePipeAsync(i, mPcmOutPipe, buf, mPacketSizeOut, PCMoutComplete, this) == kIOReturnSuccess);
}
void PloytecUSB::PCMoutComplete(void* r, IOReturn res, void*) {
	auto* s = (PloytecUSB*)r; if (s->mUsbShutdownInProgress) return;
	if (res != kIOReturnAborted) s->SubmitPCMout((uint32_t)(s->mOutputSequence.fetch_add(1) + kDefaultUrbs));
	if (res == kIOReturnSuccess) {
		bool rdy = s->mSHM->audio.driverReady.load(std::memory_order_relaxed);
		if (rdy) {
			s->mHwSampleTime += kFramesPerPacket;
			if ((s->mHwSampleTime % kZeroTimestampPeriod) == 0) {
				auto& ts = s->mSHM->audio.timestamp;
				uint32_t seq = ts.sequence.load(std::memory_order_relaxed);
				ts.sequence.store(seq+1, std::memory_order_release);
				ts.sampleTime.store(s->mHwSampleTime, std::memory_order_relaxed);
				ts.hostTime.store(mach_absolute_time(), std::memory_order_relaxed);
				ts.sequence.store(seq+2, std::memory_order_release);
			}
		}
	}
}

bool PloytecUSB::SubmitMIDIin() {
	IOUSBInterfaceInterface** i = (mMidiInIf==0)?mInterface0:mInterface1;
	uint32_t w = std::atomic_load_explicit(&mSHM->midiIn.writeIndex, std::memory_order_relaxed);
	return ((*i)->ReadPipeAsync(i, mMidiInPipe, &mSHM->midiIn.buffer[w], 1, MIDIinComplete, this) == kIOReturnSuccess);
}
void PloytecUSB::MIDIinComplete(void* r, IOReturn res, void* arg) {
	auto* s = (PloytecUSB*)r; if (s->mUsbShutdownInProgress) return;
	if (res == kIOReturnSuccess && (uintptr_t)arg == 1) {
		uint32_t w = std::atomic_load_explicit(&s->mSHM->midiIn.writeIndex, std::memory_order_relaxed);
		if (s->mSHM->midiIn.buffer[w] != 0xFD) {
			std::atomic_store_explicit(&s->mSHM->midiIn.writeIndex, (w+1)&kMidiRingMask, std::memory_order_release);
		}
	}
	if (res != kIOReturnAborted) s->SubmitMIDIin();
}

bool PloytecUSB::ReadFirmwareVersion() {
	IOUSBDevRequest req = { 0xC0, 'V', 0, 0, 0x0F, mRxBufferControl, 0 };
	if ((*mDevice)->DeviceRequest(mDevice, &req) != kIOReturnSuccess) return false;
	mFW = { mRxBufferControl[0], 1, (uint8_t)(mRxBufferControl[2]/10), (uint8_t)(mRxBufferControl[2]%10) };
	os_log(GetLog(), "[PloytecUSB] Firmware: v%u.%u.%u (ID:0x%02X)", mFW.major, mFW.minor, mFW.patch, mFW.ID);
	return true;
}

bool PloytecUSB::ReadHardwareStatus() {
	IOUSBDevRequest req = { 0xC0, 'I', 0, 0, 1, mRxBufferControl, 0 };
	if ((*mDevice)->DeviceRequest(mDevice, &req) != 0) return false;
	uint8_t s = mRxBufferControl[0];
	os_log(GetLog(), "[PloytecUSB] Hardware Status: [0x%{public}02X] %{public}s %{public}s %{public}s %{public}s %{public}s %{public}s",
	   s,
	   (s & kStatusHighSpeedActive) ? "[HighSpeed]"   : "[FullSpeed]",
	   (s & kStatusLegacyActive)    ? "[Legacy/BCD1]" : "[Modern/BCD3]",
	   (s & kStatusStreamingArmed)  ? "[Armed]"       : "[Disarmed]",
	   (s & kStatusDigitalLock)     ? "[Clock-Lock]"  : "[No-Lock]",
	   (s & kStatusUsbInterfaceOn)  ? "[Streaming]"   : "[Idle]",
	   (s & kStatusSyncReady)       ? "[Stable]"      : "[Syncing]");
	return true;
}

bool PloytecUSB::GetHardwareFrameRate() {
	if (!mDevice || !mSHM) return false;
	uint32_t rate = 0;

	// Special case for Xone:4D which is bugged and does not respond to the standard request
	if (mVendorID == 0x0A4A && mProductID == 0xFF4D) { 
		rate = kXone4DFixedRate;
	} else {
		IOUSBDevRequest req = { 0xA2, 0x81, 0x0100, 0, 0x03, mRxBufferControl, 0 };
		if ((*mDevice)->DeviceRequest(mDevice, &req) != kIOReturnSuccess || req.wLenDone < 3) return false;
		rate = ((uint32_t)mRxBufferControl[0]) | ((uint32_t)mRxBufferControl[1] << 8) | ((uint32_t)mRxBufferControl[2] << 16);
	}

	mSHM->audio.sampleRate.store(rate, std::memory_order_relaxed);
	os_log(GetLog(), "[PloytecUSB] Current Hardware Frame Rate: %{public}u Hz", rate);
	return true;
}

bool PloytecUSB::SetHardwareFrameRate(uint32_t r) {
	os_log(GetLog(), "[PloytecUSB] Setting Hardware Frame Rate to %{public}u Hz...", r);
	mTxBufferControl[0]=(uint8_t)r; mTxBufferControl[1]=(uint8_t)(r>>8); mTxBufferControl[2]=(uint8_t)(r>>16);
	IOUSBDevRequest q1 = { 0x22, 1, 0x0100, 0x0086, 3, mTxBufferControl, 0 }; (*mDevice)->DeviceRequest(mDevice, &q1);
	q1.wIndex = 0x0005; (*mDevice)->DeviceRequest(mDevice, &q1); q1.wIndex = 0x0086; (*mDevice)->DeviceRequest(mDevice, &q1);
	q1.wIndex = 0x0005; (*mDevice)->DeviceRequest(mDevice, &q1); q1.wIndex = 0x0086; return ((*mDevice)->DeviceRequest(mDevice, &q1)==0);
}

bool PloytecUSB::WriteHardwareStatus(uint16_t v) {
	IOUSBDevRequest req = { 0x40, 'I', v, 0, 0, mTxBufferControl, 0 };
	return ((*mDevice)->DeviceRequest(mDevice, &req) == 0);
}