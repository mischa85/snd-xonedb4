#import "PloytecAppUserClient.h"
#import "PloytecDriverKeys.h"
#import <IOKit/IOKitLib.h>
#import <IOKit/IOMessage.h>

static void PloytecInterestCallback(void *refCon, io_service_t service, uint32_t type, void *arg)
{
	if (type == kIOMessageServiceIsTerminated)
	{
		[(__bridge PloytecAppUserClient *)refCon handleDisconnected];
	}
}

@interface PloytecAppUserClient()
@property io_object_t ioObject;
@property io_connect_t ioConnection;
@property (nonatomic, assign) CFRunLoopRef globalRunLoop;
@property (nonatomic, assign) IONotificationPortRef notePort;
@property mach_port_t globalMachNotificationPort;
@property io_object_t terminationNotifier;
@end

@implementation PloytecAppUserClient

- (instancetype)init
{
	NSLog(@"PloytecAppUserClient: Init");

	self = [super init];
	if (!self) return nil;

	if (!_notePort)
	{
		_notePort = IONotificationPortCreate(kIOMainPortDefault);
		if (_notePort)
			CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(_notePort), kCFRunLoopCommonModes);
		else
			NSLog(@"PloytecAppUserClient: IONotificationPortCreate failed");
	}

	return self;
}

- (void)handleDisconnected
{
	NSLog(@"PloytecAppUserClient: Disconnected");

	if (_ioConnection != IO_OBJECT_NULL)
	{
		IOServiceClose(_ioConnection);
		_ioConnection = IO_OBJECT_NULL;
	}

	if (_terminationNotifier)
	{
		IOObjectRelease(_terminationNotifier);
		_terminationNotifier = IO_OBJECT_NULL;
	}
	[[NSNotificationCenter defaultCenter] postNotificationName:@"UserClientConnectionClosed" object:nil];
}

// Open a user client instance, which initiates communication with the driver.
- (NSString*)openConnection
{
	NSLog(@"PloytecAppUserClient:openConnection");

	kern_return_t ret;

	if (self.notePort == NULL)
	{
		self.notePort = IONotificationPortCreate(kIOMainPortDefault);
		if (self.notePort == NULL)
		{
			NSLog(@"PloytecAppUserClient:openConnection: Failed to initialize IONotificationPort");
			return @"Failed to initialize IONotificationPort.";
		}
		CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(self.notePort);
		if (src == NULL)
		{
			NSLog(@"PloytecAppUserClient:openConnection: Failed to get IONotification runloop source");
			return @"Failed to get IONotification runloop source.";
		}
		CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
	}

	if (self.ioObject != IO_OBJECT_NULL && self.ioConnection != IO_OBJECT_NULL)
	{
		io_async_ref64_t asyncRef = {};
		asyncRef[kIOAsyncCalloutFuncIndex] = (io_user_reference_t)MIDIAsyncCallback;
		asyncRef[kIOAsyncCalloutRefconIndex] = (io_user_reference_t)self;

		mach_port_t mp = IONotificationPortGetMachPort(self.notePort);
		ret = IOConnectCallAsyncMethod(self.ioConnection, PloytecDriverExternalMethod_RegisterForMIDINotification, mp, asyncRef, kIOAsyncCalloutCount, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
		if (ret != kIOReturnSuccess)
		{
			NSLog(@"PloytecAppUserClient:openConnection:IOConnectCallAsyncMethod: %s", mach_error_string(ret));
			return [NSString stringWithFormat:@"Failed to register async MIDI: %s", mach_error_string(ret)];
		}
		[[NSNotificationCenter defaultCenter] postNotificationName:@"UserClientConnectionOpened" object:nil];
		return @"Connection to user client succeeded";
	}

	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("PloytecDriver"));
	if (!service)
	{
		NSLog(@"PloytecAppUserClient:openConnection:IOServiceOpen: Driver Extension is not running");
		return @"Driver Extension is not running.";
	}

	io_connect_t conn = IO_OBJECT_NULL;
	ret = IOServiceOpen(service, mach_task_self(), 0, &conn);
	if (ret != kIOReturnSuccess) {
		NSLog(@"PloytecAppUserClient:openConnection:IOServiceOpen: %s", mach_error_string(ret));
		IOObjectRelease(service);
		return [NSString stringWithFormat:@"Failed to open user client: %s", mach_error_string(ret)];
	}

	self.ioObject = service;
	self.ioConnection = conn;

	ret = IOServiceAddInterestNotification(self.notePort, service, kIOGeneralInterest, PloytecInterestCallback, (__bridge void *)self, &_terminationNotifier);
	if (ret != kIOReturnSuccess) {
		NSLog(@"PloytecAppUserClient:openConnection:IOServiceAddInterestNotification: %s", mach_error_string(ret));
		IOObjectRelease(service);
		return @"Failed to set IOServiceAddInterestNotification.";
	}

	io_async_ref64_t asyncRef = {};
	asyncRef[kIOAsyncCalloutFuncIndex] = (io_user_reference_t)MIDIAsyncCallback;
	asyncRef[kIOAsyncCalloutRefconIndex] = (io_user_reference_t)self;

	mach_port_t mp = IONotificationPortGetMachPort(self.notePort);
	ret = IOConnectCallAsyncMethod(self.ioConnection, PloytecDriverExternalMethod_RegisterForMIDINotification, mp, asyncRef, kIOAsyncCalloutCount, nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
	if (ret != kIOReturnSuccess) {
		NSLog(@"PloytecAppUserClient:openConnection:IOConnectCallAsyncMethod: %s", mach_error_string(ret));
		return [NSString stringWithFormat:@"Failed to register async MIDI: %s", mach_error_string(ret)];
	}

	NSLog(@"PloytecAppUserClient:openConnection: Success");
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

	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetDeviceName, nullptr, 0, nullptr, 0, nullptr, nullptr, devicename, &devicenameSize);

	if (ret != kIOReturnSuccess)
		return [NSString stringWithFormat:@"Failed to get device name, error: %s.", mach_error_string(ret)];

	return [NSString stringWithUTF8String:devicename];
}

- (NSString*)getDeviceManufacturer
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:getDeviceManufacturer: No connection to driver");
		return;
	}

	char devicemanufacturer[128] = {0};
	size_t devicemanufacturerSize = sizeof(devicemanufacturer);

	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetDeviceManufacturer, nullptr, 0, nullptr, 0, nullptr, nullptr, devicemanufacturer, &devicemanufacturerSize);

	if (ret != kIOReturnSuccess)
		return [NSString stringWithFormat:@"Failed to get device manufacturer, error: %s.", mach_error_string(ret)];
	
	return [NSString stringWithUTF8String:devicemanufacturer];
}

- (NSString*)getFirmwareVersion
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:getFirmwareVersion: No connection to driver");
		return;
	}

	char firmwarever[15] = {0};
	size_t firmwareverSize = sizeof(firmwarever);

	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetFirmwareVer, nullptr, 0, nullptr, 0, nullptr, nullptr, firmwarever, &firmwareverSize);

	if (ret != kIOReturnSuccess)
		return [NSString stringWithFormat:@"Failed to get firmware, error: %s.", mach_error_string(ret)];

	return [NSString stringWithFormat:@"Firmware: %d.%d.%d", firmwarever[1], firmwarever[2], firmwarever[3]];
}

- (void)setCurrentUrbCount:(uint8_t)urbCount
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:setCurrentUrbCount: No connection to driver");
		return;
	}

	uint64_t value = urbCount;

	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_SetCurrentUrbCount, &value, 1, nullptr, 0, nullptr, nullptr, nullptr, 0);

	if (ret != kIOReturnSuccess)
		NSLog(@"PloytecAppUserClient:setCurrentUrbCount: %s", mach_error_string(ret));
}

- (void)setFrameCount:(uint16_t)inputFrameCount output:(uint16_t)outputFrameCount
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:setFrameCount: No connection to driver");
		return;
	}

	uint64_t value = ((uint64_t)outputFrameCount << 32) | inputFrameCount;

	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_SetFrameCount, &value, 1, nullptr, 0, nullptr, nullptr, nullptr, 0);

	if (ret != kIOReturnSuccess)
		NSLog(@"setCurrentInputFramesCount failed: %s", mach_error_string(ret));
}

- (uint8_t)getCurrentUrbCount
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:getCurrentUrbCount: No connection to driver");
		return;
	}

	uint64_t num = 0;
	uint32_t outputCount = 1;
	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetCurrentUrbCount, nullptr, 0, nullptr, 0, &num, &outputCount, nullptr, 0);

	if (ret != kIOReturnSuccess)
	{
		NSLog(@"getCurrentUrbCount failed: %s", mach_error_string(ret));
		return 0;
	}

	return (uint8_t)num;
}

- (uint16_t)getCurrentInputFramesCount
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:getCurrentInputFramesCount: No connection to driver");
		return;
	}

	uint64_t num = 0;
	uint32_t outputCount = 1;
	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetCurrentInputFramesCount, nullptr, 0, nullptr, 0, &num, &outputCount, nullptr, 0);

	if (ret != kIOReturnSuccess)
	{
		NSLog(@"GetCurrentInputFramesCount failed: %s", mach_error_string(ret));
		return 0;
	}

	return (uint16_t)num;
}

- (uint16_t)getCurrentOutputFramesCount
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:getCurrentOutputFramesCount: No connection to driver");
		return;
	}

	uint64_t num = 0;
	uint32_t outputCount = 1;
	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetCurrentOutputFramesCount, nullptr, 0, nullptr, 0, &num, &outputCount, nullptr, 0);

	if (ret != kIOReturnSuccess)
	{
		NSLog(@"getCurrentOutputFramesCount failed: %s", mach_error_string(ret));
		return 0;
	}

	return (uint16_t)num;
}

- (playbackstats)getPlaybackStats
{
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:getPlaybackStats: No connection to driver");
		return;
	}

	playbackstats stats;
	size_t playbackstatsSize = sizeof(stats);
	
	kern_return_t ret = IOConnectCallMethod(_ioConnection, PloytecDriverExternalMethod_GetPlaybackStats, nullptr, 0, nullptr, 0, nullptr, nullptr, &stats, &playbackstatsSize);
	if (ret != kIOReturnSuccess)
	{
		NSLog(@"PloytecAppUserClient:getPlaybackStats: %s", mach_error_string(ret));
		return {0};
	}

	return stats;
}

- (void)sendMIDIMessageToDriver:(uint64_t)message {
	if (_ioConnection == IO_OBJECT_NULL)
	{
		NSLog(@"PloytecAppUserClient:sendMIDIMessageToDriver: No connection to driver");
		return;
	}

	kern_return_t ret = IOConnectCallScalarMethod(_ioConnection, PloytecDriverExternalMethod_SendMIDI, &message, 1, NULL, NULL);

	if (ret != kIOReturnSuccess)
		NSLog(@"PloytecAppUserClient:sendMIDIMessageToDriver: %s", mach_error_string(ret));
}

static void MIDIAsyncCallback(void* refcon, IOReturn ret, void** args, UInt32 numArgs)
{
	if (ret != kIOReturnSuccess)
	{
		NSLog(@"PloytecAppUserClient:MIDIAsyncCallback: %s", mach_error_string(ret));
		return;
	}

	uint64_t midiMsg = (uint64_t)(uintptr_t)args;
	uint8_t length = midiMsg & 0xFF;

	if (length == 0 || length > 3)
	{
		NSLog(@"PloytecAppUserClient:MIDIAsyncCallback: Invalid MIDI message length: %u", length);
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
