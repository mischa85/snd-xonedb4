//
//  Use this file to import your target's public headers that you would like to expose to Swift.
//

#import <IOKit/IOKitLib.h>
#import <Foundation/Foundation.h>

@interface OzzyUserClient : NSObject

- (NSString*)openConnection;
- (NSString*)getFirmwareVersion;
- (NSString*)getDeviceName;
- (NSString*)getDeviceManufacturer;

@end
