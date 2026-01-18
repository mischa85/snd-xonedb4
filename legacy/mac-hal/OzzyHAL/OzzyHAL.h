#ifndef OzzyHAL_h
#define OzzyHAL_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudioTypes/CoreAudioBaseTypes.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h> // Required for Kext Communication
#include <atomic>
#include <cstdint>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include "../Shared/PloytecSharedData.h"

#define kPloytecDeviceID 100
#define kPloytecInputStreamID 150
#define kPloytecOutputStreamID 200

class OzzyHAL;
typedef OSStatus (OzzyHAL::*IOHandler)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo*, void*, void*);

class OzzyHAL {
public:
    static OzzyHAL& Get();
    
    void Init(AudioServerPlugInHostRef host);
    void Cleanup();

    bool IsConnected();
    bool IsRunning() const { return mIOStarted; }

    kern_return_t StartIO();
    kern_return_t StopIO();

    OSStatus ioOperationHandler(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer) {
        return (this->*mActiveIOHandler)(inDriver, inDeviceObjectID, inStreamObjectID, inClientID, inOperationID, inIOBufferFrameSize, inIOCycleInfo, ioMainBuffer, ioSecondaryBuffer);
    }

    HRESULT GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
    
    Float64 GetNominalSampleRate() const;
    AudioValueRange GetAvailableSampleRates() const;
    AudioBufferList* GetInputStreamConfiguration() const;
    AudioBufferList* GetOutputStreamConfiguration() const;
    CFStringRef GetDeviceName() const;
    CFStringRef GetManufacturer() const;
    CFStringRef GetDeviceUID() const;
    CFStringRef GetModelUID() const;
    UInt32 GetTransportType() const;
    UInt32 GetClockDomain() const;
    AudioObjectID GetInputStreamID() const;
    AudioObjectID GetOutputStreamID() const;
    AudioStreamBasicDescription GetStreamFormat() const;
    AudioStreamRangedDescription GetStreamRangedDescription() const;
    
    UInt32 GetLatency() const { return 0; }
    UInt32 GetSafetyOffset() const { return kDefaultUrbs * kFramesPerPacket; }
    UInt32 GetZeroTimestampPeriod() const;
    
    AudioChannelLayout* GetPreferredChannelLayout(AudioObjectPropertyScope inScope);
    UInt32 GetPreferredChannelLayoutSize(AudioObjectPropertyScope inScope) const;

private:
    OzzyHAL() = default;
    ~OzzyHAL() = default;

    CFStringRef mDeviceUID = nullptr;
    CFStringRef mModelUID = nullptr;
    CFStringRef mManufacturerUID = nullptr;
    
    uint32_t mZeroTimestampPeriod = kZeroTimestampPeriod;
    uint8_t* mInputBufferAddr = nullptr;
    uint8_t* mOutputBufferAddr = nullptr;
    
    AudioStreamBasicDescription mCurrentStreamFormat;
    AudioBufferList mInputConfig;
    AudioBufferList mOutputConfig;
    AudioValueRange mAvailableSampleRates;
    
    bool mIOStarted = false;
    uint32_t mTimestampSeed = 1;

    AudioServerPlugInHostRef mHost = nullptr;
    PloytecSharedMemory* mSHM = nullptr;
    
    // --- Kext Support ---
    io_connect_t mKextConnect = IO_OBJECT_NULL;
    mach_vm_address_t mKextMapAddress = 0;
    mach_vm_size_t mKextMapSize = 0;
    bool mIsKextMapping = false;

    // --- Legacy Daemon Support ---
    int mSHMFd = -1;
    ino_t mSHMInode = 0;

    uint32_t mLastSessionID = 0;

    pthread_t mMonitorThread;
    std::atomic<bool> mMonitorRunning { false };
    bool mLastConnectedState = false;
    alignas(16) uint8_t mChannelLayoutBuffer[0x50C];

    void MapSharedMemory();
    void UnmapSharedMemory();
    static void* MonitorEntry(void* arg);
    void MonitorLoop();
    void RebuildStreamConfigs();

    void ClearOutputBuffer();

    IOHandler mActiveIOHandler = &OzzyHAL::ioOperationBulk;
    
    OSStatus ioOperationBulk(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
    OSStatus ioOperationInterrupt(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);

    void EncodePloytecPCM(uint8_t *dst, const float *src);
    void DecodePloytecPCM(float *dst, const uint8_t *src);
};

#endif