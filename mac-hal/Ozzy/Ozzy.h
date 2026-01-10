#ifndef Ozzy_h
#define Ozzy_h

#include "PloytecUSB.h"
#include <vector>
#include <memory>
#include <IOKit/usb/IOUSBLib.h>
#include <CoreFoundation/CoreFoundation.h>

extern os_log_t gOzzyLog;
inline os_log_t OzzyLog() { 
	if (!gOzzyLog) gOzzyLog = os_log_create("Ozzy", "daemon"); 
	return gOzzyLog;
}

class Ozzy {
public:
	static Ozzy& Get();
	void Run();

private:
	Ozzy();
	~Ozzy();

	void SetupDeviceMatching();
	void StartGlobalHeartbeat();

	static void DeviceAdded(void* refCon, io_iterator_t iterator);
	static void DeviceRemoved(void* refCon, io_iterator_t iterator);

	IONotificationPortRef mNotifyPort = nullptr;
	CFRunLoopRef mRunLoop = nullptr;

	CFRunLoopTimerRef mHeartbeatTimer = nullptr;
	
	std::vector<std::shared_ptr<PloytecUSB>> mDrivers;
};

#endif