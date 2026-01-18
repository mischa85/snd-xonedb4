#ifndef OzzyMIDI_h
#define OzzyMIDI_h

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIDriver.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <atomic>

#define kOzzyFactoryUUID_Str   "43A67C89-12F5-4076-92F7-8975306B8F82"
#define kOzzyMIDIDriverTypeID  CFUUIDGetConstantUUIDWithBytes(NULL, 0xEC, 0xDE, 0x95, 0x74, 0x0F, 0xE4, 0x11, 0xD4, 0xBB, 0x1A, 0x00, 0x50, 0xE4, 0xCE, 0xA5, 0x26)
#define kOzzyInterfaceUUID     CFUUIDGetConstantUUIDWithBytes(NULL, 0x49, 0xDF, 0xCA, 0x9E, 0x0F, 0xE5, 0x11, 0xD4, 0x95, 0x0D, 0x00, 0x50, 0xE4, 0xCE, 0xA5, 0x26)

typedef struct OzzyMIDIDriver {
	MIDIDriverInterface* _vtbl;
	UInt32 _refCount;
} OzzyMIDIDriver;

extern "C" void* OzzyMIDIFactory(CFAllocatorRef allocator, CFUUIDRef typeID);

#endif
