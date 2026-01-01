#include "PloytecDriver.h"
#include "PloytecAudioDevice.h"
#include "../shared/PloytecSharedData.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/USBSpec.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

static constexpr uint8_t PCM_OUT_EP = 0x05, PCM_IN_EP = 0x86, MIDI_IN_EP = 0x83;
static constexpr uint32_t MIDI_RX_SIZE = 512, DEFAULT_URBS = 2;

struct PloytecDriver_IVars {
	IOUSBDeviceInterface** usbDevice = nullptr;
	IOUSBInterfaceInterface** usbInterface0 = nullptr;
	IOUSBInterfaceInterface** usbInterface1 = nullptr;
	CFStringRef usbVendorName = nullptr;
	CFStringRef usbProductName = nullptr;
	CFStringRef usbSerialNumber = nullptr;
	uint8_t usbMIDIinPipe = 0, usbPCMoutPipe = 0, usbPCMinPipe = 0;
	uint8_t midiInIf = 0xFF, pcmOutIf = 0xFF, pcmInIf = 0xFF;
	TransferMode transferMode = BULK;
	uint16_t usbMIDIbyteNo = 0;
	CFRunLoopSourceRef usbInterface0Src = nullptr;
	CFRunLoopSourceRef usbInterface1Src = nullptr;
	std::atomic<bool> usbShutdownInProgress{false};
	uint8_t* usbRXBufferCONTROLAddr = nullptr;
	uint8_t* usbTXBufferCONTROLAddr = nullptr;
	uint8_t* usbRXBufferPCM = nullptr;
	uint8_t* usbTXBufferPCMandUART = nullptr;
	uint8_t* usbRXBufferMIDI = nullptr;
	uint32_t usbInputPacketSize = 0, usbOutputPacketSize = 0;
	uint32_t rxSegCount = 0, txSegCount = 0;
	std::atomic<uint32_t> rxSeg { 0 }, txSeg { 0 };
	uint16_t usbVendorID = 0, usbProductID = 0;
	uint8_t currentStatus = 0;
	uint32_t currentHWFrameRate = 0;
};

os_log_t gPloytecLog = nullptr;
static PloytecSharedMemory* gSharedMem = nullptr;

static inline IOUSBInterfaceInterface** IfPtr(PloytecDriver_IVars* v, UInt8 idx) {
	return (idx == 0) ? v->usbInterface0 : (idx == 1) ? v->usbInterface1 : nullptr;
}

static void PromoteToRealTime() {
	mach_timebase_info_data_t timebase; mach_timebase_info(&timebase);
	thread_time_constraint_policy_data_t policy;
	policy.period = (uint32_t)(1.0 * 1000000.0 * (timebase.denom / (double)timebase.numer));
	policy.computation = (uint32_t)(0.5 * 1000000.0 * (timebase.denom / (double)timebase.numer));
	policy.constraint = (uint32_t)(1.0 * 1000000.0 * (timebase.denom / (double)timebase.numer));
	policy.preemptible = 1;
	thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

PloytecDriver::PloytecDriver() : mHost(nullptr), mIsConnected(false) { ivars = new PloytecDriver_IVars(); }
PloytecDriver::~PloytecDriver() {
	if (mAddedIter) IOObjectRelease(mAddedIter);
	if (mRemovedIter) IOObjectRelease(mRemovedIter);
	CloseUSBDevice(); DestroyBuffers();
	if (mNotifyPort) IONotificationPortDestroy(mNotifyPort);
	if (mRunLoop) CFRelease(mRunLoop);
	delete ivars;
}

void PloytecDriver::InitSharedMemory() {
	if (gSharedMem) return;
	
	mode_t oldMask = umask(0);
	int fd = shm_open(kPloytecSharedMemName, O_CREAT | O_RDWR, 0666);
	umask(oldMask);
	
	if (fd == -1) { os_log_error(GetLog(), "[PloytecHAL] Failed to open shm! %{public}d", errno); return; }
	
	struct stat sb; if (fstat(fd, &sb) == -1) { close(fd); return; }
	if (sb.st_size != sizeof(PloytecSharedMemory)) {
		if (ftruncate(fd, sizeof(PloytecSharedMemory)) == -1) {
			os_log_error(GetLog(), "[PloytecHAL] SHM mismatch. Recreating...");
			close(fd); shm_unlink(kPloytecSharedMemName);
			oldMask = umask(0);
			fd = shm_open(kPloytecSharedMemName, O_CREAT | O_RDWR, 0666);
			umask(oldMask);
			if (fd == -1) return;
			if (ftruncate(fd, sizeof(PloytecSharedMemory)) == -1) { close(fd); return; }
		}
	}
	void* ptr = mmap(NULL, sizeof(PloytecSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (ptr == MAP_FAILED) return;
	gSharedMem = (PloytecSharedMemory*)ptr;
	memset(gSharedMem, 0, sizeof(PloytecSharedMemory));
	
	srand((unsigned int)time(NULL));
	gSharedMem->sessionID = rand(); 
	gSharedMem->magic = 0x504C4F59; gSharedMem->version = 1; gSharedMem->audioDriverReady.store(true);
	
	if (ivars->usbProductName) {
		CFStringGetCString(ivars->usbProductName, gSharedMem->productName, 64, kCFStringEncodingUTF8);
	} else {
		strlcpy(gSharedMem->productName, "USB Audio Device", 64);
	}

	os_log_info(GetLog(), "[PloytecHAL] Shared Memory Ready (SessionID: 0x%{public}08X).", gSharedMem->sessionID);
}

void PloytecDriver::CleanupSharedMemory() {
	if (gSharedMem) { 
		gSharedMem->magic = 0xDEADBEEF; // Poison pill for MIDI driver
		gSharedMem->sessionID = 0;
		gSharedMem->audioDriverReady.store(false); 
		munmap(gSharedMem, sizeof(PloytecSharedMemory)); 
		gSharedMem = nullptr; 
		shm_unlink(kPloytecSharedMemName);
	}
}

PloytecDriver& PloytecDriver::GetInstance() { static PloytecDriver instance; return instance; }
PloytecAudioDevice* PloytecDriver::GetAudioDevice() const { return &PloytecAudioDevice::Get(); }

void PloytecDriver::Initialize() {
	GetLog();
	pthread_t thread;
	pthread_create(&thread, NULL, USBWorkLoop, this);
	pthread_detach(thread);
}

void PloytecDriver::NotifyDeviceListChanged() {
	if (mHost) {
		AudioObjectPropertyAddress addr = { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
		mHost->PropertiesChanged(mHost, kAudioObjectPlugInObject, 1, &addr);
	}
}

void PloytecDriver::SetConnected(bool connected) { if (mIsConnected.exchange(connected) != connected) NotifyDeviceListChanged(); }

void PloytecDriver::DeviceAdded(void* refCon, io_iterator_t iterator) {
	auto* self = static_cast<PloytecDriver*>(refCon);
	io_service_t obj;
	bool openedAny = false;
	
	while ((obj = IOIteratorNext(iterator))) {
		if (self->OpenUSBDevice(obj)) {
			uint16_t pid = self->ivars->usbProductID;
			bool supported = (pid == kPloytecPID_DB4 || pid == kPloytecPID_DB2 || pid == kPloytecPID_DX || pid == kPloytecPID_4D);
			if (supported) {
				os_log(GetLog(), "[PloytecHAL] >>> DEVICE FOUND (PID:0x%{public}04X) <<<", pid);
				if (self->CreateBuffers()) {
					self->ReadFirmwareVersion(); self->ReadHardwareStatus();
					self->GetHardwareFrameRate(); self->SetHardwareFrameRate(96000); usleep(50000);
					self->GetHardwareFrameRate(); self->ReadHardwareStatus(); self->WriteHardwareStatus(0xFFB2);
					if (self->CreateUSBPipes() && self->DetectTransferMode()) {
						CFStringRef vendor = self->GetManufacturerName();
						CFStringRef product = self->GetProductName();
						CFStringRef serial = self->GetSerialNumber();
						CFStringRef uid = serial ? CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("hackerman.ploytechal.%@"), serial) : CFSTR("hackerman.ploytechal.device.1");
						PloytecAudioDevice::Get().init(self, true, product, uid, vendor, 2560, 8, 8, self->GetInputBuffer(), BUFFER_SIZE_IN, self->GetOutputBuffer(), BUFFER_SIZE_OUT, self->GetTransferMode());
						if (serial && uid) CFRelease(uid);
						if (!self->StartStreaming(DEFAULT_URBS)) os_log_error(GetLog(), "[PloytecHAL] Failed to start streaming!"); else openedAny = true;
					} else os_log_error(GetLog(), "[PloytecHAL] Pipe/Mode error");
				} else { self->CloseUSBDevice(); os_log_error(GetLog(), "[PloytecHAL] Buffer alloc failed"); }
			} else {
				os_log(GetLog(), "[PloytecHAL] Unsupported PID 0x%{public}04X", pid);
				self->CloseUSBDevice();
			}
		} else os_log_error(GetLog(), "[PloytecHAL] Failed to open USB device");
		IOObjectRelease(obj);
	}
	if (openedAny) self->SetConnected(true);
}

static bool GetPipeInfo(IOUSBInterfaceInterface** intf, UInt8 pipeRef, UInt8* outTransferType, UInt16* outMaxPacketSize) {
	if (!intf || pipeRef == 0) return false;
	UInt8 dir=0, num=0, tt=0, interval=0; UInt16 mps=0;
	if ((*intf)->GetPipeProperties(intf, pipeRef, &dir, &num, &tt, &mps, &interval) != kIOReturnSuccess) return false;
	if (outTransferType) *outTransferType = tt;
	if (outMaxPacketSize) *outMaxPacketSize = mps;
	return true;
}

bool PloytecDriver::DetectTransferMode() {
	IOUSBInterfaceInterface** outIntf = IfPtr(ivars, ivars->pcmOutIf);
	if (!outIntf || ivars->usbPCMoutPipe == 0) return false;
	UInt8 tt = 0; UInt16 mps = 0;
	if (!GetPipeInfo(outIntf, ivars->usbPCMoutPipe, &tt, &mps)) return false;
	if (tt == kUSBBulk) ivars->transferMode = BULK;
	else if (tt == kUSBInterrupt) ivars->transferMode = INTERRUPT;
	else return false;
	ivars->usbMIDIbyteNo = (ivars->transferMode == BULK) ? 480 : 432;
	os_log(GetLog(), "[PloytecHAL] Transfer Mode: %{public}s", (ivars->transferMode == BULK ? "BULK" : "INTERRUPT"));
	return true;
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

static IOUSBInterfaceInterface** OpenInterface(io_service_t ifService) {
	IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
	if (IOCreatePlugInInterfaceForService(ifService, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score) != kIOReturnSuccess) return nullptr;
	IOUSBInterfaceInterface** intf = nullptr;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID), (LPVOID*)&intf);
	(*plug)->Release(plug);
	if (intf && (*intf)->USBInterfaceOpen(intf) != kIOReturnSuccess) { (*intf)->Release(intf); return nullptr; }
	return intf;
}

void PloytecDriver::DeviceRemoved(void* refCon, io_iterator_t iterator) {
	auto* self = static_cast<PloytecDriver*>(refCon);
	io_service_t obj;
	bool activeDeviceRemoved = false;
	while ((obj = IOIteratorNext(iterator))) {
		if (self->mUSBService != IO_OBJECT_NULL && IOObjectIsEqualTo(obj, self->mUSBService)) activeDeviceRemoved = true;
		IOObjectRelease(obj);
	}
	if (activeDeviceRemoved) {
		os_log(GetLog(), "[PloytecHAL] <<< DEVICE REMOVED <<<");
		self->StopStreaming(); 
		self->CloseUSBDevice(); 
		self->DestroyBuffers();
		self->SetConnected(false);
	}
}

void* PloytecDriver::USBWorkLoop(void* arg) {
	auto* self = static_cast<PloytecDriver*>(arg);
	self->mRunLoop = CFRunLoopGetCurrent(); CFRetain(self->mRunLoop);
	self->mNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
	CFRunLoopAddSource(self->mRunLoop, IONotificationPortGetRunLoopSource(self->mNotifyPort), kCFRunLoopCommonModes);

	const uint16_t supportedPIDs[] = { kPloytecPID_DB4, kPloytecPID_DB2, kPloytecPID_DX, kPloytecPID_4D };
	int vid = kPloytecVendorID;
	CFNumberRef nVid = CFNumberCreate(NULL, kCFNumberIntType, &vid);

	for (uint16_t pid : supportedPIDs) {
		CFNumberRef nPid = CFNumberCreate(NULL, kCFNumberIntType, &pid);
		CFMutableDictionaryRef matchAdded = IOServiceMatching(kIOUSBDeviceClassName);
		CFDictionarySetValue(matchAdded, CFSTR(kUSBVendorID), nVid);
		CFDictionarySetValue(matchAdded, CFSTR(kUSBProductID), nPid);
		if (IOServiceAddMatchingNotification(self->mNotifyPort, kIOMatchedNotification, matchAdded, DeviceAdded, self, &self->mAddedIter) == kIOReturnSuccess) DeviceAdded(self, self->mAddedIter);
		
		CFMutableDictionaryRef matchRemoved = IOServiceMatching(kIOUSBDeviceClassName);
		CFDictionarySetValue(matchRemoved, CFSTR(kUSBVendorID), nVid);
		CFDictionarySetValue(matchRemoved, CFSTR(kUSBProductID), nPid);
		if (IOServiceAddMatchingNotification(self->mNotifyPort, kIOTerminatedNotification, matchRemoved, DeviceRemoved, self, &self->mRemovedIter) == kIOReturnSuccess) DeviceRemoved(self, self->mRemovedIter);
		CFRelease(nPid);
	}
	CFRelease(nVid);
	PromoteToRealTime();
	os_log(GetLog(), "[PloytecHAL] USB WorkLoop Started. Waiting for devices...");
	CFRunLoopRun();
	return nullptr;
}

bool PloytecDriver::OpenUSBDevice(io_service_t service) {
	CloseUSBDevice();
	mUSBService = service; IOObjectRetain(mUSBService);
	ivars->usbVendorName = (CFStringRef)IORegistryEntryCreateCFProperty(service, CFSTR("USB Vendor Name"), kCFAllocatorDefault, 0);
	ivars->usbProductName = (CFStringRef)IORegistryEntryCreateCFProperty(service, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
	ivars->usbSerialNumber = (CFStringRef)IORegistryEntryCreateCFProperty(service, CFSTR("USB Serial Number"), kCFAllocatorDefault, 0);
	IOCFPlugInInterface** plug = nullptr; SInt32 score = 0;
	IOCreatePlugInInterfaceForService(mUSBService, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
	if (!plug) return false;
	(*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID*)&ivars->usbDevice);
	(*plug)->Release(plug);
	if (!ivars->usbDevice) return false;
	
	IOReturn openRet = kIOReturnError;
	for (int i=0; i<5; i++) {
		openRet = (*ivars->usbDevice)->USBDeviceOpenSeize(ivars->usbDevice);
		if (openRet == kIOReturnSuccess) break;
		if (openRet == kIOReturnExclusiveAccess) usleep(100000); else break;
	}
	if (openRet != kIOReturnSuccess) { os_log_error(GetLog(), "[PloytecHAL] Seize Failed 0x%{public}08X", openRet); return false; }
	(*ivars->usbDevice)->GetDeviceVendor(ivars->usbDevice, &ivars->usbVendorID);
	(*ivars->usbDevice)->GetDeviceProduct(ivars->usbDevice, &ivars->usbProductID);
	return true;
}

static bool AttachAsyncSource(IOUSBInterfaceInterface** intf, CFRunLoopRef rl, CFRunLoopSourceRef* outSrc) {
	if (!intf || !rl) return false;
	if ((*intf)->CreateInterfaceAsyncEventSource(intf, outSrc) == kIOReturnSuccess && *outSrc) { CFRunLoopAddSource(rl, *outSrc, kCFRunLoopDefaultMode); return true; }
	return false;
}

void PloytecDriver::CloseUSBDevice() {
	StopStreaming();
	if (ivars->usbInterface0 && ivars->usbInterface0Src) { CFRunLoopRemoveSource(mRunLoop, ivars->usbInterface0Src, kCFRunLoopDefaultMode); CFRelease(ivars->usbInterface0Src); ivars->usbInterface0Src=nullptr; }
	if (ivars->usbInterface1 && ivars->usbInterface1Src) { CFRunLoopRemoveSource(mRunLoop, ivars->usbInterface1Src, kCFRunLoopDefaultMode); CFRelease(ivars->usbInterface1Src); ivars->usbInterface1Src=nullptr; }
	if (ivars->usbInterface0) { (*ivars->usbInterface0)->USBInterfaceClose(ivars->usbInterface0); (*ivars->usbInterface0)->Release(ivars->usbInterface0); ivars->usbInterface0=nullptr; }
	if (ivars->usbInterface1) { (*ivars->usbInterface1)->USBInterfaceClose(ivars->usbInterface1); (*ivars->usbInterface1)->Release(ivars->usbInterface1); ivars->usbInterface1=nullptr; }
	if (ivars->usbDevice) { (*ivars->usbDevice)->USBDeviceClose(ivars->usbDevice); (*ivars->usbDevice)->Release(ivars->usbDevice); ivars->usbDevice=nullptr; }
	if (mUSBService) { IOObjectRelease(mUSBService); mUSBService = IO_OBJECT_NULL; }
}

bool PloytecDriver::CreateUSBPipes() {
	if ((*ivars->usbDevice)->SetConfiguration(ivars->usbDevice, 1) != kIOReturnSuccess) return false;
	IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
	io_iterator_t it;
	(*ivars->usbDevice)->CreateInterfaceIterator(ivars->usbDevice, &req, &it);
	io_service_t svc;
	while((svc = IOIteratorNext(it))) {
		IOUSBInterfaceInterface** intf = OpenInterface(svc);
		IOObjectRelease(svc);
		if(!intf) continue;
		(*intf)->SetAlternateInterface(intf, 1);
		UInt8 pM=0, pO=0, pI=0;
		FindPipe(intf, MIDI_IN_EP, &pM); FindPipe(intf, PCM_OUT_EP, &pO); FindPipe(intf, PCM_IN_EP, &pI);
		if (!ivars->usbInterface0) ivars->usbInterface0 = intf; else ivars->usbInterface1 = intf;
		if(pM) { ivars->usbMIDIinPipe = pM; ivars->midiInIf = (ivars->usbInterface0 == intf) ? 0 : 1; }
		if(pO) { ivars->usbPCMoutPipe = pO; ivars->pcmOutIf = (ivars->usbInterface0 == intf) ? 0 : 1; }
		if(pI) { ivars->usbPCMinPipe = pI;  ivars->pcmInIf = (ivars->usbInterface0 == intf) ? 0 : 1; }
	}
	IOObjectRelease(it);
	if (mRunLoop) {
		if(ivars->usbInterface0) AttachAsyncSource(ivars->usbInterface0, mRunLoop, &ivars->usbInterface0Src);
		if(ivars->usbInterface1) AttachAsyncSource(ivars->usbInterface1, mRunLoop, &ivars->usbInterface1Src);
	}
	return (ivars->usbMIDIinPipe && ivars->usbPCMoutPipe && ivars->usbPCMinPipe);
}

bool PloytecDriver::CreateBuffers() {
	DestroyBuffers();
	ivars->usbRXBufferCONTROLAddr = (uint8_t*)calloc(1, 16); ivars->usbTXBufferCONTROLAddr = (uint8_t*)calloc(1, 16);
	ivars->usbRXBufferPCM = (uint8_t*)aligned_alloc(64, BUFFER_SIZE_IN); ivars->usbTXBufferPCMandUART = (uint8_t*)aligned_alloc(64, BUFFER_SIZE_OUT);
	ivars->usbRXBufferMIDI = (uint8_t*)aligned_alloc(64, MIDI_RX_SIZE);
	if (!ivars->usbRXBufferPCM || !ivars->usbTXBufferPCMandUART) return false;
	InitSharedMemory(); return true;
}

void PloytecDriver::DestroyBuffers() {
	CleanupSharedMemory();
	free(ivars->usbRXBufferCONTROLAddr); ivars->usbRXBufferCONTROLAddr = nullptr;
	free(ivars->usbTXBufferCONTROLAddr); ivars->usbTXBufferCONTROLAddr = nullptr;
	free(ivars->usbRXBufferPCM); ivars->usbRXBufferPCM = nullptr;
	free(ivars->usbTXBufferPCMandUART); ivars->usbTXBufferPCMandUART = nullptr;
	free(ivars->usbRXBufferMIDI); ivars->usbRXBufferMIDI = nullptr;
}

uint8_t* PloytecDriver::GetInputBuffer() { return ivars->usbRXBufferPCM; }
uint8_t* PloytecDriver::GetOutputBuffer() { return ivars->usbTXBufferPCMandUART; }
TransferMode PloytecDriver::GetTransferMode() { return ivars->transferMode; }
CFStringRef PloytecDriver::GetManufacturerName() const { return ivars->usbVendorName; }
CFStringRef PloytecDriver::GetProductName() const { return ivars->usbProductName; }
CFStringRef PloytecDriver::GetSerialNumber() const { return ivars->usbSerialNumber; }

void PloytecDriver::ClearOutputBuffer() {
	if (!ivars->usbTXBufferPCMandUART) return;
	if (ivars->transferMode == BULK) {
		const uint32_t stride = 512, pcmSize = 480, numBlocks = BUFFER_SIZE_OUT / stride;
		for (uint32_t i = 0; i < numBlocks; ++i) memset(ivars->usbTXBufferPCMandUART + (i * stride), 0, pcmSize);
	} else {
		const uint32_t stride = 482, pcm1 = 432, gap = 2, pcm2 = 48, numBlocks = BUFFER_SIZE_OUT / stride;
		for (uint32_t i = 0; i < numBlocks; ++i) {
			uint8_t* ptr = ivars->usbTXBufferPCMandUART + (i * stride);
			memset(ptr, 0, pcm1); memset(ptr + pcm1 + gap, 0, pcm2);
		}
	}
}

bool PloytecDriver::StartStreaming(uint8_t urbCount) {
	if (!ivars->usbDevice) return false;
	ivars->usbShutdownInProgress.store(false);
	ivars->rxSeg.store(0); ivars->txSeg.store(0);
	if(!ConfigureStreamingFormat(80, 80)) return false;
	
	memset(ivars->usbRXBufferPCM, 0, BUFFER_SIZE_IN);
	memset(ivars->usbTXBufferPCMandUART, 0, BUFFER_SIZE_OUT);
	if (ivars->transferMode == BULK) {
		for (size_t i = 480; i + 1 < BUFFER_SIZE_OUT; i += 512) { ivars->usbTXBufferPCMandUART[i] = 0xFD; ivars->usbTXBufferPCMandUART[i + 1] = 0xFD; }
	} else {
		for (size_t i = 432; i + 1 < BUFFER_SIZE_OUT; i += 482) { ivars->usbTXBufferPCMandUART[i] = 0xFD; ivars->usbTXBufferPCMandUART[i + 1] = 0xFD; }
	}
	
	SubmitMIDIin();
	for(uint8_t i=0; i<urbCount; ++i) { SubmitPCMin(i); SubmitPCMout(i); }
	return true;
}

bool PloytecDriver::StopStreaming() {
	ivars->usbShutdownInProgress.store(true);
	IOUSBInterfaceInterface** inIntf = IfPtr(ivars, ivars->pcmInIf);
	IOUSBInterfaceInterface** outIntf = IfPtr(ivars, ivars->pcmOutIf);
	IOUSBInterfaceInterface** midiIntf= IfPtr(ivars, ivars->midiInIf);
	if (inIntf && ivars->usbPCMinPipe) (*inIntf)->AbortPipe(inIntf, ivars->usbPCMinPipe);
	if (outIntf && ivars->usbPCMoutPipe) (*outIntf)->AbortPipe(outIntf, ivars->usbPCMoutPipe);
	if (midiIntf && ivars->usbMIDIinPipe) (*midiIntf)->AbortPipe(midiIntf, ivars->usbMIDIinPipe);
	return true;
}

bool PloytecDriver::SubmitPCMin(uint32_t seg) {
	IOUSBInterfaceInterface** intf = IfPtr(ivars, ivars->pcmInIf);
	if(!intf) return false;
	(*intf)->ReadPipeAsync(intf, ivars->usbPCMinPipe, ivars->usbRXBufferPCM + (seg * ivars->usbInputPacketSize), ivars->usbInputPacketSize, PCMinComplete, this);
	return true;
}

void PloytecDriver::PCMinComplete(void* refCon, IOReturn result, void* arg0) {
	auto* self = (PloytecDriver*)refCon;
	if (self->ivars->usbShutdownInProgress.load() || result != kIOReturnSuccess) return;
	uint16_t currentpos; self->GetAudioDevice()->Capture(currentpos, 80, mach_absolute_time());
	uint32_t nextSeg = self->ivars->rxSeg.fetch_add(1) % self->ivars->rxSegCount;
	self->SubmitPCMin(nextSeg);
}

bool PloytecDriver::SubmitPCMout(uint32_t seg) {
	IOUSBInterfaceInterface** intf = IfPtr(ivars, ivars->pcmOutIf);
	const uint32_t off = seg * ivars->usbOutputPacketSize;
	uint8_t* txBuf = ivars->usbTXBufferPCMandUART + off;
	if (gSharedMem) {
		uint32_t r = std::atomic_load_explicit(&gSharedMem->midiOut.readIndex, std::memory_order_relaxed);
		uint32_t w = std::atomic_load_explicit(&gSharedMem->midiOut.writeIndex, std::memory_order_acquire);
		if (r != w) {
			uint8_t byte = gSharedMem->midiOut.buffer[r];
			std::atomic_store_explicit(&gSharedMem->midiOut.readIndex, (r + 1) & kMidiRingMask, std::memory_order_release);
			txBuf[ivars->usbMIDIbyteNo] = byte;
		} else { txBuf[ivars->usbMIDIbyteNo] = 0xFD; }
		txBuf[ivars->usbMIDIbyteNo + 1] = 0xFD; 
	}
	(*intf)->WritePipeAsync(intf, ivars->usbPCMoutPipe, txBuf, ivars->usbOutputPacketSize, PCMoutComplete, this);
	return true;
}

void PloytecDriver::PCMoutComplete(void* refCon, IOReturn result, void* arg0) {
	auto* self = (PloytecDriver*)refCon;
	if (self->ivars->usbShutdownInProgress.load() || result != kIOReturnSuccess) return;
	uint16_t currentpos; self->GetAudioDevice()->Playback(currentpos, 80, mach_absolute_time());
	uint32_t nextSeg = self->ivars->txSeg.fetch_add(1) % self->ivars->txSegCount;
	self->SubmitPCMout(nextSeg);
}

bool PloytecDriver::SubmitMIDIin() {
	IOUSBInterfaceInterface** intf = IfPtr(ivars, ivars->midiInIf);
	if(!intf) return false;
	IOReturn ret = (*intf)->ReadPipeAsync(intf, ivars->usbMIDIinPipe, ivars->usbRXBufferMIDI, MIDI_RX_SIZE, MIDIinComplete, this);
	if (ret != kIOReturnSuccess) { os_log_error(GetLog(), "[PloytecHAL] MIDIin Submit Failed 0x%{public}08X", ret); return false; }
	return true;
}

void PloytecDriver::MIDIinComplete(void* refCon, IOReturn result, void* arg0) {
	auto* self = (PloytecDriver*)refCon;
	if (self->ivars->usbShutdownInProgress.load()) return;
	if (result != kIOReturnSuccess) {
		os_log_error(GetLog(), "[PloytecHAL] MIDIin Fail 0x%{public}08X", result);
		self->SubmitMIDIin(); return;
	}
	uint32_t len = (uint32_t)(uintptr_t)arg0;
	if (len > 0) {
		if (gSharedMem) {
			uint32_t w = std::atomic_load_explicit(&gSharedMem->midiIn.writeIndex, std::memory_order_relaxed);
			uint32_t r = std::atomic_load_explicit(&gSharedMem->midiIn.readIndex, std::memory_order_acquire);
			for(uint32_t i=0; i<len; ++i) {
				uint8_t b = self->ivars->usbRXBufferMIDI[i];
				if (b == 0xFD) continue; 
				uint32_t nextW = (w + 1) & kMidiRingMask;
				if (nextW != r) { gSharedMem->midiIn.buffer[w] = b; w = nextW; }
			}
			std::atomic_store_explicit(&gSharedMem->midiIn.writeIndex, w, std::memory_order_release);
		}
	}
	self->SubmitMIDIin();
}

bool PloytecDriver::ConfigureStreamingFormat(uint16_t inputFrames, uint16_t outputFrames) {
	const uint32_t outStride = (ivars->transferMode == BULK) ? 512u : 482u;
	ivars->usbOutputPacketSize = (outputFrames / 10u) * outStride;
	if (inputFrames > 0) ivars->usbInputPacketSize = inputFrames * 64; else ivars->usbInputPacketSize = 0;
	if (ivars->usbOutputPacketSize > 0) ivars->txSegCount = BUFFER_SIZE_OUT / ivars->usbOutputPacketSize; else ivars->txSegCount = 0;
	if (ivars->usbInputPacketSize > 0) ivars->rxSegCount = BUFFER_SIZE_IN / ivars->usbInputPacketSize; else ivars->rxSegCount = 0;
	if (ivars->txSegCount < 2) return false;
	if (ivars->usbInputPacketSize > 0 && ivars->rxSegCount < 2) return false;
	return true;
}

bool PloytecDriver::ReadFirmwareVersion() {
	if (!ivars->usbDevice) return false;
	IOUSBDevRequest req = {}; req.bmRequestType = 0xC0; req.bRequest = 'V'; req.wLength = 0x0F; req.pData = ivars->usbRXBufferCONTROLAddr;
	if ((*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req) != kIOReturnSuccess) return false;
	mFW.ID = ivars->usbRXBufferCONTROLAddr[0]; mFW.major = 1; mFW.minor = (uint8_t)(ivars->usbRXBufferCONTROLAddr[2] / 10); mFW.patch = (uint8_t)(ivars->usbRXBufferCONTROLAddr[2] % 10);
	os_log(GetLog(), "[PloytecHAL] Firmware ID=0x%{public}02x Version=%u.%u.%u", mFW.ID, mFW.major, mFW.minor, mFW.patch);
	return true;
}

bool PloytecDriver::ReadHardwareStatus() {
	if (!ivars->usbDevice) return false;
	IOUSBDevRequest req = {}; req.bmRequestType = 0xC0; req.bRequest = 'I'; req.wLength = 0x01; req.pData = ivars->usbRXBufferCONTROLAddr;
	if ((*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req) != kIOReturnSuccess) return false;
	ivars->currentStatus = ivars->usbRXBufferCONTROLAddr[0];
	return true;
}

bool PloytecDriver::WriteHardwareStatus(uint16_t value) {
	if (!ivars->usbDevice) return false;
	IOUSBDevRequest req = {}; req.bmRequestType = 0x40; req.bRequest = 'I'; req.wValue = value; req.pData = ivars->usbTXBufferCONTROLAddr;
	return ((*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req) == kIOReturnSuccess);
}

bool PloytecDriver::GetHardwareFrameRate() {
	if (!ivars->usbDevice) return false;
	if (ivars->usbVendorID == 0x0A4A && ivars->usbProductID == 0xFF4D) { ivars->currentHWFrameRate = 96000; return true; }
	IOUSBDevRequest req = {}; req.bmRequestType = 0xA2; req.bRequest = 0x81; req.wValue = 0x0100; req.wLength = 0x03; req.pData = ivars->usbRXBufferCONTROLAddr;
	if ((*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req) != kIOReturnSuccess || req.wLenDone < 3) return false;
	ivars->currentHWFrameRate = ((uint32_t)ivars->usbRXBufferCONTROLAddr[0]) | ((uint32_t)ivars->usbRXBufferCONTROLAddr[1] << 8) | ((uint32_t)ivars->usbRXBufferCONTROLAddr[2] << 16);
	return true;
}

bool PloytecDriver::SetHardwareFrameRate(uint32_t framerate) {
	if (!ivars->usbDevice) return false;
	ivars->usbTXBufferCONTROLAddr[0] = (uint8_t)(framerate & 0xFF);
	ivars->usbTXBufferCONTROLAddr[1] = (uint8_t)((framerate >> 8) & 0xFF);
	ivars->usbTXBufferCONTROLAddr[2] = (uint8_t)((framerate >> 16) & 0xFF);
	IOUSBDevRequest req = {}; req.bmRequestType = 0x22; req.bRequest = 0x01; req.wValue = 0x0100; req.wLength = 0x03; req.pData = ivars->usbTXBufferCONTROLAddr;
	req.wIndex = 0x0086; (*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req);
	req.wIndex = 0x0005; (*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req);
	req.wIndex = 0x0086; (*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req);
	req.wIndex = 0x0005; (*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req);
	req.wIndex = 0x0086; if ((*ivars->usbDevice)->DeviceRequest(ivars->usbDevice, &req) != kIOReturnSuccess) return false;
	return true;
}