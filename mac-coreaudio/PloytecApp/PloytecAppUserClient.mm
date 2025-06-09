//
//  PloytecAppUserClient.mm
//  PloytecApp
//
//  Created by Marcel Bierling on 04/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#import "PloytecAppUserClient.h"
#import "PloytecDriverKeys.h"

@interface PloytecAppUserClient()
@property io_object_t ioObject;
@property io_connect_t ioConnection;
@end

@implementation PloytecAppUserClient

// Open a user client instance, which initiates communication with the driver.
- (NSString*)openConnection
{
	bool newlyConnected = false;

	if (_ioObject == IO_OBJECT_NULL && _ioConnection == IO_OBJECT_NULL)
	{
		// Get the IOKit main port.
		mach_port_t theMainPort = MACH_PORT_NULL;
		kern_return_t theKernelError = IOMainPort(bootstrap_port, &theMainPort);
		if (theKernelError != kIOReturnSuccess) {
			return @"Failed to get IOMainPort.";
		}

		CFDictionaryRef theMatchingDictionary = IOServiceNameMatching("PloytecDriver");
		io_service_t matchedService = IOServiceGetMatchingService(theMainPort, theMatchingDictionary);
		if (matchedService) {
			_ioObject = matchedService;
			theKernelError = IOServiceOpen(_ioObject, mach_task_self(), 0, &_ioConnection);
			if (theKernelError != kIOReturnSuccess) {
				_ioObject = IO_OBJECT_NULL;
				_ioConnection = IO_OBJECT_NULL;
				return [NSString stringWithFormat:@"Failed to open user client connection, error: %s.", mach_error_string(theKernelError)];
			}
			newlyConnected = true;
			[[NSNotificationCenter defaultCenter] postNotificationName:@"UserClientConnectionOpened" object:nil];
		} else {
			return @"Driver Extension is not running";
		}
	}

	mach_port_t notifyPort = MACH_PORT_NULL;
	kern_return_t kr = IOCreateReceivePort(kOSAsyncCompleteMessageID, &notifyPort);
	if (kr != KERN_SUCCESS) {
		return [NSString stringWithFormat:@"Failed to create Mach receive port: %s", mach_error_string(kr)];
	}

	kr = IOConnectSetNotificationPort(_ioConnection, 0, notifyPort, 0);
	if (kr != KERN_SUCCESS) {
		return [NSString stringWithFormat:@"Failed to set notification port: %s", mach_error_string(kr)];
	}

	uint64_t asyncRef[1] = { 0x1337 };
	kr = IOConnectCallAsyncScalarMethod(
		_ioConnection,
		PloytecDriverExternalMethod_RegisterForMIDINotification,
		notifyPort, asyncRef, 1,
		NULL, 0,
		NULL, NULL
	);
	if (kr != KERN_SUCCESS) {
		return [NSString stringWithFormat:@"Failed to register for async MIDI notifications: %s", mach_error_string(kr)];
	}

	dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, notifyPort, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(source, ^{
		mach_msg_header_t msg;
		kern_return_t r = mach_msg(&msg, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), notifyPort, 0, MACH_PORT_NULL);
		if (r == KERN_SUCCESS) {
			uint64_t asyncRef[1] = { 0x1337 }; // not used in this dummy call
			uint64_t inputScalar[1] = { 0 };   // optional
			uint32_t inputScalarCnt = 0;

			uint64_t outputScalar[1] = { 0 };
			uint32_t outputScalarCnt = 0;

			uint8_t outputStruct[1] = { 0 };
			size_t outputStructCnt = sizeof(outputStruct);

			IOConnectCallAsyncMethod(
				_ioConnection,
				0,                   // selector (use a valid one if needed)
				MACH_PORT_NULL,      // async reply port (not needed here)
				asyncRef, 1,         // async reference
				inputScalar, inputScalarCnt,
				NULL, 0,             // inputStruct, inputStructCnt
				outputScalar, &outputScalarCnt,
				outputStruct, &outputStructCnt
			);
		}
	});
	dispatch_resume(source);

	return newlyConnected ? @"Connection to user client succeeded" : @"User client was already connected";
}


- (NSString*)getDeviceName
{
	if (_ioConnection == IO_OBJECT_NULL) {
		return @"Can't toggle the data source because the user client isn't connected.";
	}

	char devicename[128] = {0};
	size_t devicenameSize = sizeof(devicename);

	kern_return_t error = IOConnectCallMethod(_ioConnection, static_cast<uint64_t>(PloytecDriverExternalMethod_GetDeviceName), nullptr, 0, nullptr, 0, nullptr, nullptr, devicename, &devicenameSize);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to get device name, error: %s.", mach_error_string(error)];
	}

	return [NSString stringWithFormat:@"Device: %s", devicename];
}

- (NSString*)getDeviceManufacturer
{
	if (_ioConnection == IO_OBJECT_NULL) {
		return @"Can't toggle the data source because the user client isn't connected.";
	}

	char devicemanufacturer[128] = {0};
	size_t devicemanufacturerSize = sizeof(devicemanufacturer);

	kern_return_t error = IOConnectCallMethod(_ioConnection, static_cast<uint64_t>(PloytecDriverExternalMethod_GetDeviceManufacturer), nullptr, 0, nullptr, 0, nullptr, nullptr, devicemanufacturer, &devicemanufacturerSize);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to get device manufacturer, error: %s.", mach_error_string(error)];
	}
	
	return [NSString stringWithFormat:@"Manufacturer: %s", devicemanufacturer];
}

- (NSString*)getFirmwareVersion
{
	if (_ioConnection == IO_OBJECT_NULL) {
		return @"Can't toggle the data source because the user client isn't connected.";
	}

	char firmwarever[15] = {0};
	size_t firmwareverSize = sizeof(firmwarever);
	
	kern_return_t error =
		IOConnectCallMethod(_ioConnection,
							static_cast<uint64_t>(PloytecDriverExternalMethod_GetFirmwareVer),
							nullptr, 0, nullptr, 0, nullptr, nullptr, firmwarever, &firmwareverSize);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to get firmware, error: %s.", mach_error_string(error)];
	}

	return [NSString stringWithFormat:@"Firmware: 1.%d.%d", (firmwarever[2]/10), (firmwarever[2]%10)];
}

- (playbackstats)getPlaybackStats
{
	if (_ioConnection == IO_OBJECT_NULL) {
		return;
	}

	playbackstats stats;
	size_t playbackstatsSize = sizeof(stats);
	
	kern_return_t error =
		IOConnectCallMethod(_ioConnection,
							static_cast<uint64_t>(PloytecDriverExternalMethod_GetPlaybackStats),
							nullptr, 0, nullptr, 0, nullptr, nullptr, &stats, &playbackstatsSize);
	
	//NSLog(@"PLAYBACKSTATS: %llu %llu %llu %llu", stats.out_sample_time, stats.out_sample_time_usb, stats.in_sample_time, stats.in_sample_time_usb);

	return stats;
}

static void MIDIAsyncCallback(void* refcon, IOReturn result, void** args, uint32_t numArgs) {
	if (result == kIOReturnSuccess && numArgs >= 1) {
		uint64_t midiMessage = (uint64_t)args[0];
		NSLog(@"Received async MIDI message: 0x%llx", midiMessage);
		// Optionally: forward this to the main thread or SwiftUI via NotificationCenter, Combine, or delegate
	}
}

@end
