//
//  Use this file to import your target's public headers that you would like to expose to Swift.
//

#import <IOKit/IOKitLib.h>
#import <Foundation/Foundation.h>

typedef struct {
	bool playing;
	bool recording;
	uint64_t out_sample_time;
	uint64_t out_sample_time_usb;
	int64_t out_sample_time_diff;
	uint64_t in_sample_time;
	uint64_t in_sample_time_usb;
	int64_t in_sample_time_diff;
	uint64_t xruns;
} playbackstats;

@interface PloytecAppUserClient : NSObject

- (NSString*)openConnection;
- (NSString*)getFirmwareVersion;
- (NSString*)getDeviceName;
- (NSString*)getDeviceManufacturer;
- (playbackstats)getPlaybackStats;
- (BOOL)getNextMIDIPacket;

@end
