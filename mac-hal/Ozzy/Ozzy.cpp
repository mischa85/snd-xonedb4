#include "Ozzy.h"

os_log_t gOzzyLog = nullptr;

int main(int argc, const char * argv[]) {
	Ozzy::Get().Run();
	return 0;
}

Ozzy& Ozzy::Get() {
	static Ozzy instance;
	return instance;
}

Ozzy::Ozzy() {
	mRunLoop = CFRunLoopGetCurrent();
	mNotifyPort = IONotificationPortCreate(kIOMainPortDefault);
	CFRunLoopAddSource(mRunLoop, IONotificationPortGetRunLoopSource(mNotifyPort), kCFRunLoopDefaultMode);
}

Ozzy::~Ozzy() {
	if (mNotifyPort) IONotificationPortDestroy(mNotifyPort);
	if (mHeartbeatTimer) {
		CFRunLoopTimerInvalidate(mHeartbeatTimer);
		CFRelease(mHeartbeatTimer);
	}
}

void Ozzy::Run() {
	os_log(OzzyLog(), "[Ozzy] 🐈 Service Starting...");
	SetupDeviceMatching();
	StartGlobalHeartbeat();
	CFRunLoopRun();
}

void Ozzy::StartGlobalHeartbeat() {
	mHeartbeatTimer = CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.1, 0.1, 0, 0, ^(CFRunLoopTimerRef timer) {
		for (auto& driver : Ozzy::Get().mDrivers) {
			if (driver) {
				driver->PulseHeartbeat();
			}
		}
	});
	
	if (mHeartbeatTimer) {
		CFRunLoopAddTimer(mRunLoop, mHeartbeatTimer, kCFRunLoopCommonModes);
		os_log(OzzyLog(), "[Ozzy] 💓 Heartbeat Timer Started");
	} else {
		os_log_error(OzzyLog(), "[Ozzy] 💥 Failed to create Heartbeat Timer!");
	}
}

void Ozzy::SetupDeviceMatching() {
	const uint16_t pids[] = { kPloytecPID_DB4, kPloytecPID_DB2, kPloytecPID_DX, kPloytecPID_4D };
	SInt32 vid = kPloytecVendorID;
	CFNumberRef nVid = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);

	for (uint16_t pidVal : pids) {
		SInt32 pid = pidVal;
		CFNumberRef nPid = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
		CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
		CFDictionarySetValue(match, CFSTR(kUSBVendorID), nVid);
		CFDictionarySetValue(match, CFSTR(kUSBProductID), nPid);
		
		CFRetain(match);
		
		io_iterator_t addIter;
		IOServiceAddMatchingNotification(mNotifyPort, kIOMatchedNotification, match, DeviceAdded, this, &addIter);
		DeviceAdded(this, addIter);
		
		io_iterator_t remIter;
		IOServiceAddMatchingNotification(mNotifyPort, kIOTerminatedNotification, match, DeviceRemoved, this, &remIter);
		DeviceRemoved(this, remIter);
		
		CFRelease(nPid);
	}
	CFRelease(nVid);
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

static uint32_t GetRegistryInt(io_service_t service, CFStringRef key) {
	uint32_t value = 0;
	CFTypeRef ref = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	if (ref) {
		if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
		}
		CFRelease(ref);
	}
	return value;
}

void Ozzy::DeviceAdded(void* refCon, io_iterator_t iterator) {
	auto* self = static_cast<Ozzy*>(refCon);
	io_service_t service;
	while ((service = IOIteratorNext(iterator))) {
		os_log(OzzyLog(), "[Ozzy] Spawning PloytecUSB for New Device [0x%04X:0x%04X]", GetRegistryInt(service, CFSTR(kUSBVendorID)), GetRegistryInt(service, CFSTR(kUSBProductID))); 
		auto driver = std::make_shared<PloytecUSB>(service);
		self->mDrivers.push_back(driver);
		IOObjectRelease(service);
	}
}

void Ozzy::DeviceRemoved(void* refCon, io_iterator_t iterator) {
	auto* self = static_cast<Ozzy*>(refCon);
	io_service_t service;
	while ((service = IOIteratorNext(iterator))) {
		os_log(OzzyLog(), "[Ozzy] Device Removed");
		self->mDrivers.erase(std::remove_if(self->mDrivers.begin(), self->mDrivers.end(),
			[service](std::shared_ptr<PloytecUSB>& d) { 
				return d->MatchesService(service); 
			}), self->mDrivers.end());
		IOObjectRelease(service);
	}
}