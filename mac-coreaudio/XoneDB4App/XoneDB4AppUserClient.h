//
//  Use this file to import your target's public headers that you would like to expose to Swift.
//

#import <IOKit/IOKitLib.h>
#import <Foundation/Foundation.h>

typedef struct {
	uint64_t out_sample_time;
	uint64_t out_sample_time_usb;
	uint64_t in_sample_time;
	uint64_t in_sample_time_usb;
} playbackstats;

@interface XoneDB4AppUserClient : NSObject

- (NSString*)openConnection;
- (NSString*)getFirmwareVersion;
- (NSString*)changeBufferSize:(uint32_t)buffersize;
- (playbackstats)getPlaybackStats;

@end
