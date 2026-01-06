#include "PloytecUSB.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/USBSpec.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

os_log_t gUSBEngineLog = nullptr;

int main(int argc, const char * argv[]) {
	PloytecUSB::GetInstance().Run();
	return 0;
}

static void PromoteToRealTime() {
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	const uint64_t period_ns = 3333332ull;
	const uint64_t computation_ns = 400000ull;
	const uint64_t constraint_ns = 3333332ull;

	uint64_t period_abs64 = (period_ns * (uint64_t)tb.denom) / (uint64_t)tb.numer;
	uint64_t computation_abs64 = (computation_ns * (uint64_t)tb.denom) / (uint64_t)tb.numer;
	uint64_t constraint_abs64 = (constraint_ns * (uint64_t)tb.denom) / (uint64_t)tb.numer;

	if (period_abs64 > UINT32_MAX) period_abs64 = UINT32_MAX;
	if (computation_abs64 > UINT32_MAX) computation_abs64 = UINT32_MAX;
	if (constraint_abs64 > UINT32_MAX) constraint_abs64 = UINT32_MAX;

	thread_time_constraint_policy_data_t policy;
	policy.period = (uint32_t)period_abs64;
	policy.computation = (uint32_t)computation_abs64;
	policy.constraint = (uint32_t)constraint_abs64;
	policy.preemptible = 1;

	thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

PloytecUSB::PloytecUSB() { 
	mach_timebase_info(&mTimebase);
}

PloytecUSB::~PloytecUSB() { 
	DestroySharedMemory();
	CloseUSBDevice();
	if (mRxBufferControl) free(mRxBufferControl);
	if (mTxBufferControl) free(mTxBufferControl);
}

PloytecUSB& PloytecUSB::GetInstance() {
	static PloytecUSB instance;
	return instance;
}

IOUSBInterfaceInterface** PloytecUSB::GetInterface(uint8_t index) {
	if (index == 0) return mUsbInterface0;
	if (index == 1) return mUsbInterface1;
	return nullptr;
}

void PloytecUSB::Run() {
	GetLog();
	os_log(GetLog(), "[PloytecUSB] Engine Starting...");
	
	shm_unlink(kPloytecSharedMemName);

	pthread_t thread;
	pthread_create(&thread, NULL, USBWorkLoop, this);
	CFRunLoopRun(); 
}

void PloytecUSB::SetupSharedMemory() {
	if (mSHM) return;

	mShmFd = shm_open(kPloytecSharedMemName, O_CREAT | O_RDWR, 0666);
	if (mShmFd == -1) {
		os_log_error(GetLog(), "[PloytecUSB] Critical: Failed to create SHM (%d)", errno);
		exit(1);
	}

	if (ftruncate(mShmFd, sizeof(PloytecSharedMemory)) == -1) {
		os_log_error(GetLog(), "[PloytecUSB] Critical: SHM Resize failed");
		exit(1);
	}

	void* ptr = mmap(NULL, sizeof(PloytecSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, mShmFd, 0);
	if (ptr == MAP_FAILED) {
		os_log_error(GetLog(), "[PloytecUSB] Critical: MMAP failed");
		exit(1);
	}

	mSHM = (PloytecSharedMemory*)ptr;
	memset(mSHM, 0, sizeof(PloytecSharedMemory));

	mSHM->magic = 0x504C4F59; 
	mSHM->version = 1;
	srand((unsigned int)time(NULL));
	mSHM->sessionID = rand();
	mSHM->audio.hardwarePresent.store(true);
	
	if (mVendorName) CFStringGetCString(mVendorName, mSHM->manufacturerName, 64, kCFStringEncodingUTF8);
	if (mProductName) CFStringGetCString(mProductName, mSHM->productName, 64, kCFStringEncodingUTF8);
	if (mSerialNumber) CFStringGetCString(mSerialNumber, mSHM->serialNumber, 64, kCFStringEncodingUTF8);

	os_log(GetLog(), "[PloytecUSB] Shared Memory Created. Session: 0x%08X", mSHM->sessionID);
}

void PloytecUSB::DestroySharedMemory() {
	if (mSHM) {
		munmap(mSHM, sizeof(PloytecSharedMemory));
		mSHM = nullptr;
	}
	if (mShmFd != -1) {
		close(mShmFd);
		mShmFd = -1;
	}
	shm_unlink(kPloytecSharedMemName);
}

void* PloytecUSB::USBWorkLoop(void* arg) {
	PromoteToRealTime();
	auto* self = static_cast<PloytecUSB*>(arg);
	self->mRunLoop = CFRunLoopGetCurrent(); CFRetain(self->mRunLoop);
	self->mNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
	CFRunLoopAddSource(self->mRunLoop, IONotificationPortGetRunLoopSource(self->mNotifyPort), kCFRunLoopCommonModes);

	const uint16_t supportedPIDs[] = { kPloytecPID_DB4, kPloytecPID_DB2, kPloytecPID_DX, kPloytecPID_4D };
	SInt32 vid = kPloytecVendorID;
	CFNumberRef nVid = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);

	for (uint16_t rawPid : supportedPIDs) {
		SInt32 pid = rawPid;
		CFNumberRef nPid = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
		CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
		CFDictionarySetValue(match, CFSTR(kUSBVendorID), nVid);
		CFDictionarySetValue(match, CFSTR(kUSBProductID), nPid);
		io_iterator_t addedIter = IO_OBJECT_NULL;
		if (IOServiceAddMatchingNotification(self->mNotifyPort, kIOMatchedNotification, match, DeviceAdded, self, &addedIter) == kIOReturnSuccess) DeviceAdded(self, addedIter);
		
		CFMutableDictionaryRef matchR = IOServiceMatching(kIOUSBDeviceClassName);
		CFDictionarySetValue(matchR, CFSTR(kUSBVendorID), nVid);
		CFDictionarySetValue(matchR, CFSTR(kUSBProductID), nPid);
		io_iterator_t removedIter = IO_OBJECT_NULL;
		if (IOServiceAddMatchingNotification(self->mNotifyPort, kIOTerminatedNotification, matchR, DeviceRemoved, self, &removedIter) == kIOReturnSuccess) DeviceRemoved(self, removedIter);
		CFRelease(nPid);
	}
	CFRelease(nVid);
	os_log(GetLog(), "[PloytecUSB] USB WorkLoop Started. Waiting for device...");
	CFRunLoopRun();
	return nullptr;
}

void PloytecUSB::DeviceAdded(void* refCon, io_iterator_t iterator) {
	auto* self = static_cast<PloytecUSB*>(refCon);
	io_service_t obj;
	while ((obj = IOIteratorNext(iterator))) {
		if (self->OpenUSBDevice(obj)) {
			os_log(GetLog(), "[PloytecUSB] >>> DEVICE FOUND (PID:0x%04X) <<<", self->mProductID);
			
			if (!self->mRxBufferControl) self->mRxBufferControl = (uint8_t*)calloc(1, 64);
			if (!self->mTxBufferControl) self->mTxBufferControl = (uint8_t*)calloc(1, 64);

			self->SetupSharedMemory();

			if (self->mSHM) {
				if (self->mIsResetting.load()) {
					os_log(GetLog(), "[PloytecUSB] ♻️ Restoring session.");
					self->mIsResetting.store(false);
				}
				self->mSHM->audio.hardwarePresent.store(true);
			}

			self->ReadFirmwareVersion();
			self->SetHardwareFrameRate(96000); 
			usleep(50000);
			self->WriteHardwareStatus(0xFFB2);
			
			if (self->CreateUSBPipes() && self->DetectTransferMode()) {
				if (self->mSHM) self->mSHM->audio.isBulkMode.store((self->mTransferMode == kTransferModeBulk), std::memory_order_release);

				if (!self->StartStreaming(kDefaultUrbs)) {
					os_log_error(GetLog(), "[PloytecUSB] Failed to start streaming!");
				}
			} else {
				os_log_error(GetLog(), "[PloytecUSB] Pipe/Mode error");
				self->CloseUSBDevice();
			}
		}
		IOObjectRelease(obj);
	}
}

bool PloytecUSB::OpenUSBDevice(io_service_t service) {
	CloseUSBDevice();
	mUSBService = service; IOObjectRetain(mUSBService);
	mVendorName = (CFStringRef)IORegistryEntryCreateCFProperty(service, CFSTR("USB Vendor Name"), kCFAllocatorDefault, 0);
	mProductName = (CFStringRef)IORegistryEntryCreateCFProperty(service, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
	mSerialNumber = (CFStringRef)IORegistryEntryCreateCFProperty(service, CFSTR("USB Serial Number"), kCFAllocatorDefault, 0);
	
	IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
	IOCreatePlugInInterfaceForService(mUSBService, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
	if (!plug) return false;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID*)&mUsbDevice);
	(*plug)->Release(plug);
	if (!mUsbDevice) return false;
	
	IOReturn openRet = kIOReturnError;
	for (int i=0; i<5; i++) {
		openRet = (*mUsbDevice)->USBDeviceOpenSeize(mUsbDevice);
		if (openRet == kIOReturnSuccess) break;
		if (openRet == kIOReturnExclusiveAccess) usleep(100000); else break;
	}
	if (openRet != kIOReturnSuccess) return false;
	
	(*mUsbDevice)->GetDeviceVendor(mUsbDevice, &mVendorID);
	(*mUsbDevice)->GetDeviceProduct(mUsbDevice, &mProductID);
	return true;
}

void PloytecUSB::CloseUSBDevice() {
	StopStreaming();
	if (mUsbInterface0 && mInterface0Src) { CFRunLoopRemoveSource(mRunLoop, mInterface0Src, kCFRunLoopDefaultMode); CFRelease(mInterface0Src); mInterface0Src=nullptr; }
	if (mUsbInterface1 && mInterface1Src) { CFRunLoopRemoveSource(mRunLoop, mInterface1Src, kCFRunLoopDefaultMode); CFRelease(mInterface1Src); mInterface1Src=nullptr; }
	if (mUsbInterface0) { (*mUsbInterface0)->USBInterfaceClose(mUsbInterface0); (*mUsbInterface0)->Release(mUsbInterface0); mUsbInterface0=nullptr; }
	if (mUsbInterface1) { (*mUsbInterface1)->USBInterfaceClose(mUsbInterface1); (*mUsbInterface1)->Release(mUsbInterface1); mUsbInterface1=nullptr; }
	if (mUsbDevice) { (*mUsbDevice)->USBDeviceClose(mUsbDevice); (*mUsbDevice)->Release(mUsbDevice); mUsbDevice=nullptr; }
	if (mUSBService) { IOObjectRelease(mUSBService); mUSBService = IO_OBJECT_NULL; }
}

static IOUSBInterfaceInterface** OpenInterface(io_service_t ifService) {
	IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
	if (IOCreatePlugInInterfaceForService(ifService, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) != kIOReturnSuccess) return nullptr;
	IOUSBInterfaceInterface** intf = nullptr;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID*)&intf);
	(*plug)->Release(plug);
	if (intf && (*intf)->USBInterfaceOpen(intf) != kIOReturnSuccess) { (*intf)->Release(intf); return nullptr; }
	return intf;
}

static bool AttachAsyncSource(IOUSBInterfaceInterface** intf, CFRunLoopRef rl, CFRunLoopSourceRef* outSrc) {
	if (!intf || !rl) return false;
	if ((*intf)->CreateInterfaceAsyncEventSource(intf, outSrc) == kIOReturnSuccess && *outSrc) { CFRunLoopAddSource(rl, *outSrc, kCFRunLoopDefaultMode); return true; }
	return false;
}

static bool FindPipe(IOUSBInterfaceInterface** intf, UInt8 epAddr, UInt8* outPipe) {
	if (!intf || !outPipe) return false;
	UInt8 n = 0;
	if ((*intf)->GetNumEndpoints(intf, &n) != kIOReturnSuccess) return false;
	for (UInt8 pipe = 1; pipe <= n; ++pipe) {
		UInt8 dir=0, num=0, tt=0, interval=0; UInt16 mps=0;
		if ((*intf)->GetPipeProperties(intf, pipe, &dir, &num, &tt, &mps, &interval) != kIOReturnSuccess) continue;
		if ((UInt8)((dir ? 0x80 : 0x00) | (num & 0x0F)) == epAddr) { *outPipe = pipe; return true; }
	}
	return false;
}

bool PloytecUSB::CreateUSBPipes() {
	if ((*mUsbDevice)->SetConfiguration(mUsbDevice, 1) != kIOReturnSuccess) return false;
	IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
	io_iterator_t it;
	(*mUsbDevice)->CreateInterfaceIterator(mUsbDevice, &req, &it);
	io_service_t svc;
	while((svc = IOIteratorNext(it))) {
		IOUSBInterfaceInterface** intf = OpenInterface(svc);
		IOObjectRelease(svc);
		if(!intf) continue;
		(*intf)->SetAlternateInterface(intf, 1);
		UInt8 pM=0, pO=0, pI=0;
		FindPipe(intf, kMidiInEp, &pM); FindPipe(intf, kPcmOutEp, &pO); FindPipe(intf, kPcmInEp, &pI);
		if (!mUsbInterface0) mUsbInterface0 = intf; else mUsbInterface1 = intf;
		if(pM) { mMidiInPipe = pM; mMidiInIf = (mUsbInterface0 == intf) ? 0 : 1; }
		if(pO) { mPcmOutPipe = pO; mPcmOutIf = (mUsbInterface0 == intf) ? 0 : 1; }
		if(pI) { mPcmInPipe = pI;  mPcmInIf = (mUsbInterface0 == intf) ? 0 : 1; }
	}
	IOObjectRelease(it);
	if (mRunLoop) {
		if(mUsbInterface0) AttachAsyncSource(mUsbInterface0, mRunLoop, &mInterface0Src);
		if(mUsbInterface1) AttachAsyncSource(mUsbInterface1, mRunLoop, &mInterface1Src);
	}
	return (mMidiInPipe && mPcmOutPipe && mPcmInPipe);
}

void PloytecUSB::DeviceRemoved(void* refCon, io_iterator_t iterator) {
	auto* self = static_cast<PloytecUSB*>(refCon);
	io_service_t obj;
	bool activeDeviceRemoved = false;
	while ((obj = IOIteratorNext(iterator))) {
		if (self->mUSBService != IO_OBJECT_NULL && IOObjectIsEqualTo(obj, self->mUSBService)) activeDeviceRemoved = true;
		IOObjectRelease(obj);
	}
	if (activeDeviceRemoved) {
		bool planningToReturn = self->mIsResetting.load();
		os_log(GetLog(), "[PloytecUSB] <<< DEVICE REMOVED %s<<<", planningToReturn ? "(TRANSPARENT) " : "");
		
		self->StopStreaming(); 
		self->CloseUSBDevice();
		
		if (!planningToReturn) {
			self->DestroySharedMemory();
		} else {
			if (self->mSHM) self->mSHM->audio.hardwarePresent.store(false);
		}
	}
}

static bool GetPipeInfo(IOUSBInterfaceInterface** intf, UInt8 pipeRef, UInt8* outTransferType, UInt16* outMaxPacketSize) {
	if (!intf || pipeRef == 0) return false;
	UInt8 dir=0, num=0, tt=0, interval=0; UInt16 mps=0;
	if ((*intf)->GetPipeProperties(intf, pipeRef, &dir, &num, &tt, &mps, &interval) != kIOReturnSuccess) return false;
	if (outTransferType) *outTransferType = tt;
	if (outMaxPacketSize) *outMaxPacketSize = mps;
	return true;
}

bool PloytecUSB::DetectTransferMode() {
	IOUSBInterfaceInterface** outIntf = GetInterface(mPcmOutIf);
	if (!outIntf || mPcmOutPipe == 0) return false;
	UInt8 tt = 0; UInt16 mps = 0;
	if (!GetPipeInfo(outIntf, mPcmOutPipe, &tt, &mps)) return false;
	mTransferMode = (tt == kUSBBulk) ? kTransferModeBulk : kTransferModeInterrupt;
	mMidiByteOffset = (mTransferMode == kTransferModeBulk) ? 480 : 432;
	return true;
}

bool PloytecUSB::ConfigureStreamingFormat() {
	mPacketSizeOut = (mTransferMode == kTransferModeBulk) ? kBulkPacketSizeOut : kInterruptPacketSizeOut;
	mPacketSizeIn = kPacketSizeIn; 
	return true;
}

bool PloytecUSB::StartStreaming(uint8_t urbCount) {
	if (!mUsbDevice || !mSHM) return false;
	
	mSHM->audio.driverReady.store(false, std::memory_order_release);
	mUsbShutdownInProgress.store(false);
	ConfigureStreamingFormat();

	memset(mSHM->audio.inputBuffer, 0, sizeof(mSHM->audio.inputBuffer));
	memset(mSHM->audio.outputBuffer, 0, sizeof(mSHM->audio.outputBuffer));

	const bool bulk = (mTransferMode == kTransferModeBulk);
	const uint32_t stride = bulk ? 512 : 482, offset = bulk ? 480 : 432;
	const uint32_t limit = kNumPackets * mPacketSizeOut;

	for (uint32_t i = offset; i + 1 < limit; i += stride) {
		mSHM->audio.outputBuffer[i] = 0xFD; 
		mSHM->audio.outputBuffer[i+1] = 0xFD;
	}
	
	mInputSequence.store(0);
	mOutputSequence.store(0);
	mHwSampleTime = 0;
	mLastZeroReported = 0;
	mWasDriverReady = false;
	
	mSHM->audio.timestamp.sampleTime.store(0);
	mSHM->audio.timestamp.hostTime.store(0);
	mSHM->audio.halWritePosition.store(0);

	SubmitMIDIin();
	for(uint8_t i=0; i<urbCount; ++i) { 
		SubmitPCMin(i); 
		SubmitPCMout(i); 
	}
	
	mSHM->audio.driverReady.store(true, std::memory_order_release);
	return true;
}

bool PloytecUSB::StopStreaming() {
	mUsbShutdownInProgress.store(true);
	IOUSBInterfaceInterface** inIntf = GetInterface(mPcmInIf);
	IOUSBInterfaceInterface** outIntf = GetInterface(mPcmOutIf);
	IOUSBInterfaceInterface** midiIntf= GetInterface(mMidiInIf);
	if (inIntf && mPcmInPipe) (*inIntf)->AbortPipe(inIntf, mPcmInPipe);
	if (outIntf && mPcmOutPipe) (*outIntf)->AbortPipe(outIntf, mPcmOutPipe);
	if (midiIntf && mMidiInPipe) (*midiIntf)->AbortPipe(midiIntf, mMidiInPipe);
	return true;
}

bool PloytecUSB::SubmitPCMin(uint32_t packetIndex) {
	IOUSBInterfaceInterface** intf = GetInterface(mPcmInIf);
	if(!intf) return false;
	
	uint32_t shmIndex = packetIndex & kPacketMask;
	uint8_t* targetBuffer = mSHM->audio.inputBuffer + (shmIndex * kPacketSizeIn);
	
	IOReturn ret = (*intf)->ReadPipeAsync(intf, mPcmInPipe, targetBuffer, mPacketSizeIn, PCMinComplete, this);
	if (ret != kIOReturnSuccess) {
		os_log_error(GetLog(), "[PloytecUSB] SubmitPCMin FAILED: 0x%08X", ret);
		return false;
	}
	return true;
}

void PloytecUSB::PCMinComplete(void* refCon, IOReturn result, void* arg0) {
	auto* self = (PloytecUSB*)refCon;
	self->mLastInputTime.store(mach_absolute_time(), std::memory_order_relaxed);
	if (self->mUsbShutdownInProgress.load()) return;
	
	if (result != kIOReturnAborted) {
		if (result != kIOReturnSuccess) {
			os_log_error(GetLog(), "[PloytecUSB] PCMin Error 0x%08X (Retrying)", result);
		}
		uint64_t next = self->mInputSequence.fetch_add(1) + kDefaultUrbs;
		self->SubmitPCMin((uint32_t)next);
	}
}

bool PloytecUSB::SubmitPCMout(uint32_t packetIndex) {
	IOUSBInterfaceInterface** intf = GetInterface(mPcmOutIf);
	if(!intf) return false;

	uint32_t shmIndex = packetIndex & kPacketMask;
	uint8_t* sourceBuffer = mSHM->audio.outputBuffer + (shmIndex * kBulkPacketSizeOut);
	
	if (mTransferMode == kTransferModeBulk) {
		uint32_t r = std::atomic_load_explicit(&mSHM->midiOut.readIndex, std::memory_order_relaxed);
		uint32_t w = std::atomic_load_explicit(&mSHM->midiOut.writeIndex, std::memory_order_acquire);
		if (r != w) {
			sourceBuffer[mMidiByteOffset] = mSHM->midiOut.buffer[r];
			std::atomic_store_explicit(&mSHM->midiOut.readIndex, (r + 1) & kMidiRingMask, std::memory_order_release);
		} else {
			sourceBuffer[mMidiByteOffset] = 0xFD;
		}
		sourceBuffer[mMidiByteOffset + 1] = 0xFD;
	}

	IOReturn ret = (*intf)->WritePipeAsync(intf, mPcmOutPipe, sourceBuffer, mPacketSizeOut, PCMoutComplete, this);
	return (ret == kIOReturnSuccess);
}

void PloytecUSB::PCMoutComplete(void* refCon, IOReturn result, void* arg0) {
	auto* self = (PloytecUSB*)refCon;
	if (self->mUsbShutdownInProgress.load()) return;
	
	if (result != kIOReturnAborted) {
		uint64_t nextSeq = self->mOutputSequence.fetch_add(1) + kDefaultUrbs;
		self->SubmitPCMout((uint32_t)nextSeq);
	}

	if (result == kIOReturnSuccess) {
		bool ready = self->mSHM->audio.driverReady.load(std::memory_order_relaxed);
		
		if (ready && !self->mWasDriverReady) {
			if (self->mHwSampleTime == 0) {
				self->mAnchorHostTime = mach_absolute_time();
				os_log(GetLog(), "[PloytecUSB] 🟢 Driver Started");
			}
		}
		self->mWasDriverReady = ready;

		if (ready) {
			self->mHwSampleTime += kFramesPerPacket;
			
			uint64_t expectedZero = (self->mHwSampleTime / kZeroTimestampPeriod) * kZeroTimestampPeriod;
			
			if ((self->mHwSampleTime % kZeroTimestampPeriod) == 0) {
				uint64_t now = mach_absolute_time();
				auto& ts = self->mSHM->audio.timestamp;
				uint32_t seq = ts.sequence.load(std::memory_order_relaxed);
				ts.sequence.store(seq + 1, std::memory_order_release); 
				ts.sampleTime.store(self->mHwSampleTime, std::memory_order_relaxed);
				ts.hostTime.store(now, std::memory_order_relaxed);
				ts.sequence.store(seq + 2, std::memory_order_release);
				self->mLastZeroReported = expectedZero;
			}

			/*
			uint64_t halPos = self->mSHM->audio.halWritePosition.load(std::memory_order_relaxed);
			if (halPos > 0) { 
				int64_t lead = (int64_t)halPos - (int64_t)self->mHwSampleTime;
				const int64_t kBufferSize = kNumPackets * kFramesPerPacket; 
				const int64_t kSafetyMargin = 320; 

				if (lead >= kBufferSize) {
					static int f=0; if(f++<10) os_log_fault(GetLog(), "🛑 BUFFER LAPPED! Lead: %lld", lead);
				} else if (lead < kSafetyMargin) {
					static int f=0; if(f++<10) os_log_fault(GetLog(), "🛑 UNDERRUN! Lead: %lld", lead);
				}
				
				static int logCounter = 0;
				if (++logCounter > 4000) {
					logCounter = 0;
					os_log_info(GetLog(), "[Stats] Lead: %lld (Speed Spot)", lead);
				}
			}
			*/
		}
	}
}

bool PloytecUSB::SubmitMIDIin() {
	IOUSBInterfaceInterface** intf = GetInterface(mMidiInIf);
	if (!intf || !mSHM) return false;
	uint32_t w = std::atomic_load_explicit(&mSHM->midiIn.writeIndex, std::memory_order_relaxed);
	IOReturn ret = (*intf)->ReadPipeAsync(intf, mMidiInPipe, &mSHM->midiIn.buffer[w], 1, MIDIinComplete, this);
	if (ret != kIOReturnSuccess && !mUsbShutdownInProgress.load()) {
		os_log_error(GetLog(), "[PloytecUSB] SubmitMIDIin Failed: 0x%08X", ret);
		return false;
	}
	return true;
}

void PloytecUSB::MIDIinComplete(void* refCon, IOReturn result, void* arg0) {
	auto* self = (PloytecUSB*)refCon;
	if (self->mUsbShutdownInProgress.load()) return;

	if (result == kIOReturnSuccess && (uint32_t)(uintptr_t)arg0 == 1) {
		uint32_t w = std::atomic_load_explicit(&self->mSHM->midiIn.writeIndex, std::memory_order_relaxed);
		if (self->mSHM->midiIn.buffer[w] != 0xFD) {
			uint32_t nextW = (w + 1) & kMidiRingMask;
			uint32_t r = std::atomic_load_explicit(&self->mSHM->midiIn.readIndex, std::memory_order_acquire);
			if (nextW != r) std::atomic_store_explicit(&self->mSHM->midiIn.writeIndex, nextW, std::memory_order_release);
		}
	}
	if (result != kIOReturnAborted) self->SubmitMIDIin();
}

bool PloytecUSB::ReadFirmwareVersion() {
	if (!mUsbDevice) return false;
	IOUSBDevRequest req = {}; req.bmRequestType = 0xC0; req.bRequest = 'V'; req.wLength = 0x0F; req.pData = mRxBufferControl;
	if ((*mUsbDevice)->DeviceRequest(mUsbDevice, &req) != kIOReturnSuccess) return false;
	mFW.ID = mRxBufferControl[0]; mFW.major = 1; mFW.minor = (uint8_t)(mRxBufferControl[2] / 10); mFW.patch = (uint8_t)(mRxBufferControl[2] % 10);
	os_log(GetLog(), "[PloytecUSB] Firmware ID=0x%{public}02x Version=%{public}u.%{public}u.%{public}u", mFW.ID, mFW.major, mFW.minor, mFW.patch);
	return true;
}

bool PloytecUSB::ReadHardwareStatus() {
	if (!mUsbDevice) return false;

	IOUSBDevRequest req = {}; 
	req.bmRequestType = 0xC0; 
	req.bRequest = 'I'; 
	req.wLength = 1; 
	req.pData = mRxBufferControl;
	
	if ((*mUsbDevice)->DeviceRequest(mUsbDevice, &req) != kIOReturnSuccess) return false;
	
	uint8_t status = mRxBufferControl[0];

	os_log(GetLog(), "[PloytecUSB] Hardware Status: [0x%{public}02X] %{public}s %{public}s %{public}s %{public}s %{public}s %{public}s",
		   status,
		   (status & kStatusHighSpeedActive) ? "[HighSpeed]"   : "[FullSpeed]",
		   (status & kStatusLegacyActive)    ? "[Legacy/BCD1]" : "[Modern/BCD3]",
		   (status & kStatusStreamingArmed)  ? "[Armed]"       : "[Disarmed]",
		   (status & kStatusDigitalLock)     ? "[Clock-Lock]"  : "[No-Lock]",
		   (status & kStatusUsbInterfaceOn)  ? "[Streaming]"   : "[Idle]",
		   (status & kStatusSyncReady)       ? "[Stable]"      : "[Syncing]");

	return true;
}

bool PloytecUSB::WriteHardwareStatus(uint16_t value) {
	if (!mUsbDevice) return false;
	IOUSBDevRequest req = {}; req.bmRequestType = 0x40; req.bRequest = 'I'; req.wValue = value; req.pData = mTxBufferControl;
	return ((*mUsbDevice)->DeviceRequest(mUsbDevice, &req) == kIOReturnSuccess);
}

bool PloytecUSB::GetHardwareFrameRate() {
	if (!mUsbDevice || !mSHM) return false;

	uint32_t rate = 0;

	// Special case for Xone:4D which is bugged and does not respond to the standard request
	if (mVendorID == 0x0A4A && mProductID == 0xFF4D) { 
		rate = 96000;
	} else {
		IOUSBDevRequest req = {}; 
		req.bmRequestType = 0xA2; 
		req.bRequest = 0x81; 
		req.wValue = 0x0100; 
		req.wLength = 0x03; 
		req.pData = mRxBufferControl;
		
		if ((*mUsbDevice)->DeviceRequest(mUsbDevice, &req) != kIOReturnSuccess || req.wLenDone < 3) return false;
		
		rate = ((uint32_t)mRxBufferControl[0]) | ((uint32_t)mRxBufferControl[1] << 8) | ((uint32_t)mRxBufferControl[2] << 16);
	}

	mSHM->audio.sampleRate.store(rate, std::memory_order_relaxed);

	os_log(GetLog(), "[PloytecUSB] Current Hardware Frame Rate: %{public}u Hz", rate);
	return true;
}

bool PloytecUSB::SetHardwareFrameRate(uint32_t framerate) {
	if (!mUsbDevice) return false;
	os_log(GetLog(), "[PloytecUSB] Setting Hardware Frame Rate to: %{public}u Hz", framerate);
	mTxBufferControl[0] = (uint8_t)(framerate & 0xFF);
	mTxBufferControl[1] = (uint8_t)((framerate >> 8) & 0xFF);
	mTxBufferControl[2] = (uint8_t)((framerate >> 16) & 0xFF);
	IOUSBDevRequest req = {}; req.bmRequestType = 0x22; req.bRequest = 0x01; req.wValue = 0x0100; req.wLength = 0x03; req.pData = mTxBufferControl;
	req.wIndex = 0x0086; (*mUsbDevice)->DeviceRequest(mUsbDevice, &req);
	req.wIndex = 0x0005; (*mUsbDevice)->DeviceRequest(mUsbDevice, &req);
	req.wIndex = 0x0086; (*mUsbDevice)->DeviceRequest(mUsbDevice, &req);
	req.wIndex = 0x0005; (*mUsbDevice)->DeviceRequest(mUsbDevice, &req);
	req.wIndex = 0x0086; if ((*mUsbDevice)->DeviceRequest(mUsbDevice, &req) != kIOReturnSuccess) return false;
	return true;
}