//
//  XoneDB4AppUserClient.mm
//  XoneDB4App
//
//  Created by Marcel Bierling on 04/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#import "XoneDB4AppUserClient.h"
#import "XoneDB4DriverKeys.h"

@interface XoneDB4AppUserClient()
@property io_object_t ioObject;
@property io_connect_t ioConnection;
@end

@implementation XoneDB4AppUserClient

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
		CFDictionaryRef theMatchingDictionary = IOServiceNameMatching("XoneDB4Driver");
		io_service_t matchedService = IOServiceGetMatchingService(theMainPort, theMatchingDictionary);
		if (matchedService) {
			_ioObject = matchedService;
			theKernelError = IOServiceOpen(_ioObject, mach_task_self(), 0, &_ioConnection);
			if (theKernelError == kIOReturnSuccess) {
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

- (NSString*)getFirmwareVersion
{
	if (_ioConnection == IO_OBJECT_NULL) {
		return @"Can't toggle the data source because the user client isn't connected.";
	}
	
	char firmwarever[15] = {0};
	size_t firmwareverSize = sizeof(firmwarever);
	
	kern_return_t error =
		IOConnectCallMethod(_ioConnection,
							static_cast<uint64_t>(XoneDB4DriverExternalMethod_GetFirmwareVer),
							nullptr, 0, nullptr, 0, nullptr, nullptr, firmwarever, &firmwareverSize);

	if (error != kIOReturnSuccess) {
		return [NSString stringWithFormat:@"Failed to get firmware, error: %s.", mach_error_string(error)];
	}

	return [NSString stringWithFormat:@"Successfully got firmware %02x %02x %02x", firmwarever[0], firmwarever[1], firmwarever[2]];
}

@end
