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
	if (_ioObject == IO_OBJECT_NULL && _ioConnection == IO_OBJECT_NULL)
	{
		// Get the IOKit main port.
		mach_port_t theMainPort = MACH_PORT_NULL;
		kern_return_t theKernelError = IOMainPort(bootstrap_port, &theMainPort);
		if (theKernelError != kIOReturnSuccess) {
			return @"Failed to get IOMainPort.";
		}

		// Create a matching dictionary for the driver class.
		// Note that classes you publish by a dext need to match by class name
		// (for example, use `IOServiceNameMatching` to construct the
		// matching dictionary, not `IOServiceMatching`).
		CFDictionaryRef theMatchingDictionary = IOServiceNameMatching("PloytecDriver");
		io_service_t matchedService = IOServiceGetMatchingService(theMainPort, theMatchingDictionary);
		if (matchedService) {
			_ioObject = matchedService;
			theKernelError = IOServiceOpen(_ioObject, mach_task_self(), 0, &_ioConnection);
			if (theKernelError == kIOReturnSuccess) {
				// Post notification for successful connection
				[[NSNotificationCenter defaultCenter] postNotificationName:@"UserClientConnectionOpened" object:nil];
				return @"Connection to user client succeeded";
			}
			else {
				_ioObject = IO_OBJECT_NULL;
				_ioConnection = IO_OBJECT_NULL;
				return [NSString stringWithFormat:@"Failed to open user client connection, error: %s.", mach_error_string(theKernelError)];
			}
		}
		return @"Driver Extension is not running";
	}
	return @"User client is already connected";
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

- (NSString*)changeBufferSize:(uint32_t)buffersize
{
	if (_ioConnection == IO_OBJECT_NULL) {
		return @"Can't toggle the data source because the user client isn't connected.";
	}

	kern_return_t error =
		IOConnectCallMethod(_ioConnection,
							static_cast<uint64_t>(PloytecDriverExternalMethod_ChangeBufferSize),
							reinterpret_cast<const uint64_t*>(&buffersize), sizeof(buffersize), nullptr, 0, nullptr, nullptr, nullptr, 0);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to change buffersize, error: %s.", mach_error_string(error)];
	}

	return @"Successfully changed buffersize.";
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

@end
