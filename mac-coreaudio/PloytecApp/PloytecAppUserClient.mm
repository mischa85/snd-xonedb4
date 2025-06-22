#import "PloytecAppUserClient.h"
#import "PloytecDriverKeys.h"

@interface PloytecAppUserClient()
@property io_object_t ioObject;
@property io_connect_t ioConnection;
@property (nonatomic, assign) CFRunLoopRef globalRunLoop;
@property mach_port_t globalMachNotificationPort;
@end

@implementation PloytecAppUserClient

// Open a user client instance, which initiates communication with the driver.
- (NSString*)openConnection
{
	kern_return_t ret;

	_globalRunLoop = CFRunLoopGetCurrent();
	if (_globalRunLoop == NULL)
	{
		return @"Failed to initialize globalRunLoop.";
	}
	CFRetain(_globalRunLoop);

	IONotificationPortRef globalNotificationPort = IONotificationPortCreate(kIOMainPortDefault);
	if (globalNotificationPort == NULL)
	{
		return @"Failed to initialize globalNotificationPort.";
	}

	_globalMachNotificationPort = IONotificationPortGetMachPort(globalNotificationPort);
	if (_globalMachNotificationPort == 0)
	{
		return @"Failed to initialize globalMachNotificationPort.";
	}

	CFRunLoopSourceRef globalRunLoopSource = IONotificationPortGetRunLoopSource(globalNotificationPort);
	if (globalRunLoopSource == NULL)
	{
		return @"Failed to initialize globalRunLoopSource.";
	}

	CFRunLoopAddSource(_globalRunLoop, globalRunLoopSource, kCFRunLoopDefaultMode);

	if (_ioObject == IO_OBJECT_NULL && _ioConnection == IO_OBJECT_NULL)
	{
		mach_port_t theMainPort = MACH_PORT_NULL;
		ret = IOMainPort(bootstrap_port, &theMainPort);
		if (ret != KERN_SUCCESS) {
			return @"Failed to get IOMainPort.";
		}

		CFDictionaryRef matchDict = IOServiceNameMatching("PloytecDriver");
		if (!matchDict) {
			return @"Failed to create matching dictionary.";
		}

		io_service_t service = IOServiceGetMatchingService(theMainPort, matchDict);
		if (!service) {
			return @"Driver Extension is not running.";
		}

		_ioObject = service;
		ret = IOServiceOpen(_ioObject, mach_task_self(), 0, &_ioConnection);
		if (ret != KERN_SUCCESS) {
			_ioObject = IO_OBJECT_NULL;
			_ioConnection = IO_OBJECT_NULL;
			return [NSString stringWithFormat:@"Failed to open user client: %s", mach_error_string(ret)];
		}
	}

	io_async_ref64_t asyncRef = {};
	asyncRef[kIOAsyncCalloutFuncIndex] = (io_user_reference_t)MIDIAsyncCallback;
	asyncRef[kIOAsyncCalloutRefconIndex] = (io_user_reference_t)self;

	ret = IOConnectCallAsyncMethod(_ioConnection, PloytecDriverExternalMethod_RegisterForMIDINotification, _globalMachNotificationPort, asyncRef, kIOAsyncCalloutCount, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
	if (ret != KERN_SUCCESS) {
		return [NSString stringWithFormat:@"Failed to register async MIDI: %s", mach_error_string(ret)];
	}

	[[NSNotificationCenter defaultCenter] postNotificationName:@"UserClientConnectionOpened" object:nil];
	return @"Connection to user client succeeded";
}

- (NSString*)getDeviceName
{
	if (_ioConnection == IO_OBJECT_NULL) {
		NSLog(@"%s: No connection to driver", __FUNCTION__);
		return;
	}

	char devicename[128] = {0};
	size_t devicenameSize = sizeof(devicename);

	kern_return_t error = IOConnectCallMethod(_ioConnection, static_cast<uint64_t>(PloytecDriverExternalMethod_GetDeviceName), nullptr, 0, nullptr, 0, nullptr, nullptr, devicename, &devicenameSize);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to get device name, error: %s.", mach_error_string(error)];
	}

	return [NSString stringWithUTF8String:devicename];
}

- (NSString*)getDeviceManufacturer
{
	if (_ioConnection == IO_OBJECT_NULL) {
		NSLog(@"%s: No connection to driver", __FUNCTION__);
		return;
	}

	char devicemanufacturer[128] = {0};
	size_t devicemanufacturerSize = sizeof(devicemanufacturer);

	kern_return_t error = IOConnectCallMethod(_ioConnection, static_cast<uint64_t>(PloytecDriverExternalMethod_GetDeviceManufacturer), nullptr, 0, nullptr, 0, nullptr, nullptr, devicemanufacturer, &devicemanufacturerSize);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to get device manufacturer, error: %s.", mach_error_string(error)];
	}
	
	return [NSString stringWithUTF8String:devicemanufacturer];
}

- (NSString*)getFirmwareVersion
{
	if (_ioConnection == IO_OBJECT_NULL) {
		NSLog(@"%s: No connection to driver", __FUNCTION__);
		return;
	}

	char firmwarever[15] = {0};
	size_t firmwareverSize = sizeof(firmwarever);
	
	kern_return_t error =
		IOConnectCallMethod(_ioConnection, static_cast<uint64_t>(PloytecDriverExternalMethod_GetFirmwareVer), nullptr, 0, nullptr, 0, nullptr, nullptr, firmwarever, &firmwareverSize);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to get firmware, error: %s.", mach_error_string(error)];
	}

	return [NSString stringWithFormat:@"Firmware: 1.%d.%d", (firmwarever[2]/10), (firmwarever[2]%10)];
}

- (void)changeUrbCount:(uint8_t)urbCount
{
	if (_ioConnection == IO_OBJECT_NULL) {
		NSLog(@"%s: No connection to driver", __FUNCTION__);
		return;
	}

	IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_ChangeURBs, reinterpret_cast<const uint64_t*>(&urbCount), 1, nullptr, 0, nullptr, nullptr, nullptr, 0);
}

- (uint8_t)getCurrentUrbCount
{
	if (_ioConnection == IO_OBJECT_NULL) {
		NSLog(@"%s: No connection to driver", __FUNCTION__);
		return;
	}

	uint8_t num;
	uint32_t outputCount = 1;
	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetCurrentUrbCount, nullptr, 0, nullptr, 0, reinterpret_cast<uint64_t*>(&num), &outputCount, nullptr, 0);
	if (ret != KERN_SUCCESS) {
		NSLog(@"getCurrentUrbCount failed: %s", mach_error_string(ret));
		return 0;
	}
	return num;
}

- (playbackstats)getPlaybackStats
{
	if (_ioConnection == IO_OBJECT_NULL) {
		NSLog(@"%s: No connection to driver", __FUNCTION__);
		return;
	}

	playbackstats stats;
	size_t playbackstatsSize = sizeof(stats);
	
	kern_return_t error = IOConnectCallMethod(_ioConnection, static_cast<uint64_t>(PloytecDriverExternalMethod_GetPlaybackStats), nullptr, 0, nullptr, 0, nullptr, nullptr, &stats, &playbackstatsSize);

	return stats;
}

- (void)sendMIDIMessageToDriver:(uint64_t)message {
	if (_ioConnection == IO_OBJECT_NULL) {
		NSLog(@"%s: No connection to driver", __FUNCTION__);
		return;
	}

	kern_return_t result = IOConnectCallScalarMethod(_ioConnection, PloytecDriverExternalMethod_SendMIDI, &message, 1, NULL, NULL);
	if (result != KERN_SUCCESS) {
		NSLog(@"Failed to send MIDI to driver: %s", mach_error_string(result));
	}
}

static void MIDIAsyncCallback(void* refcon, IOReturn result, void** args, UInt32 numArgs)
{
	if (result != kIOReturnSuccess) {
		NSLog(@"Async MIDI callback failed: %s", mach_error_string(result));
		return;
	}

	uint64_t midiMsg = (uint64_t)(uintptr_t)args;
	uint8_t length = midiMsg & 0xFF;

	if (length == 0 || length > 3) {
		NSLog(@"Invalid MIDI message length: %u", length);
		return;
	}

	uint8_t midiBytes[3];
	if (length >= 1) midiBytes[0] = (midiMsg >> 8)  & 0xFF;
	if (length >= 2) midiBytes[1] = (midiMsg >> 16) & 0xFF;
	if (length == 3) midiBytes[2] = (midiMsg >> 24) & 0xFF;

	[[NSNotificationCenter defaultCenter]
		postNotificationName:@"PloytecMIDIMessageReceived"
		object:nil
		userInfo:@{
			@"length": @(length),
			@"bytes": [NSData dataWithBytes:midiBytes length:length]
		}];
}

@end
