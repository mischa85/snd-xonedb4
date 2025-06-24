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
- (void)changeUrbCount:(uint8_t)urbCount;
- (void)changeInputFramesCount:(uint16_t)framesCount;
- (void)changeOutputFramesCount:(uint16_t)framesCount;
- (uint8_t)getCurrentUrbCount;
- (uint16_t)getCurrentInputFramesCount;
- (uint16_t)getCurrentOutputFramesCount;
- (playbackstats)getPlaybackStats;
- (void)sendMIDIMessageToDriver:(uint64_t)message;

@end
