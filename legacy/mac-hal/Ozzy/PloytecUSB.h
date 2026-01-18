#ifndef PloytecUSB_h
#define PloytecUSB_h

#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <atomic>
#include <thread>
#include <string>
#include "../Shared/PloytecSharedData.h"

struct FirmwareVersion { uint8_t ID; uint8_t major; uint8_t minor; uint8_t patch; };
enum TransferMode { kTransferModeBulk = 0, kTransferModeInterrupt = 1 };
enum PloytecStatusMask : uint8_t {
	kStatusSyncReady       = 0x01,
	kStatusUsbInterfaceOn  = 0x02,
	kStatusDigitalLock     = 0x04,
	kStatusStreamingArmed  = 0x10,
	kStatusLegacyActive    = 0x20,
	kStatusHighSpeedActive = 0x80
};

extern os_log_t gUSBEngineLog;
inline os_log_t GetLog() { 
	if (!gUSBEngineLog) gUSBEngineLog = os_log_create("PloytecUSB", "engine"); 
	return gUSBEngineLog;
}

class PloytecUSB {
public:
	PloytecUSB(io_service_t service);
	~PloytecUSB();

	void PulseHeartbeat();
	bool MatchesService(io_service_t service);

private:
	void EngineThread();
	void InitSharedMemory();
	void DestroySharedMemory();

	bool OpenDevice();
	void CloseDevice();
	bool CreatePipes();
	bool DetectTransferMode();
	bool ConfigureStreamingFormat();
	bool StartStreaming(uint8_t urbCount);
	bool StopStreaming();
	
	bool SubmitPCMin(uint32_t packetIndex);
	bool SubmitPCMout(uint32_t packetIndex);
	bool SubmitMIDIin();
	
	static void PCMinComplete(void* refCon, IOReturn result, void* arg0);
	static void PCMoutComplete(void* refCon, IOReturn result, void* arg0);
	static void MIDIinComplete(void* refCon, IOReturn result, void* arg0);
	
	bool ReadFirmwareVersion();
	bool ReadHardwareStatus();
	bool GetHardwareFrameRate();
	bool SetHardwareFrameRate(uint32_t framerate);
	bool WriteHardwareStatus(uint16_t value);

	io_service_t mService;
	IOUSBDeviceInterface** mDevice = nullptr;
	IOUSBInterfaceInterface** mInterface0 = nullptr;
	IOUSBInterfaceInterface** mInterface1 = nullptr;
	
	PloytecSharedMemory* mSHM = nullptr;
	int mShmFd = -1;
	std::string mShmName;

	std::thread mThread;
	std::atomic<bool> mRunning { false };
	CFRunLoopRef mThreadRunLoop = nullptr;
	CFRunLoopSourceRef mIf0Src = nullptr;
	CFRunLoopSourceRef mIf1Src = nullptr;

	uint8_t mMidiInPipe = 0;
	uint8_t mPcmOutPipe = 0;
	uint8_t mPcmInPipe = 0;
	
	uint8_t mMidiInIf = 0xFF;
	uint8_t mPcmOutIf = 0xFF;
	uint8_t mPcmInIf = 0xFF;
	
	TransferMode mTransferMode = kTransferModeBulk;
	uint16_t mMidiByteOffset = 0;
	uint32_t mPacketSizeIn = 0;
	uint32_t mPacketSizeOut = 0;
	
	uint16_t mVendorID = 0;
	uint16_t mProductID = 0;
	FirmwareVersion mFW = {};
	
	uint8_t* mRxBufferControl = nullptr;
	uint8_t* mTxBufferControl = nullptr;
	
	std::atomic<uint64_t> mInputSequence { 0 };
	std::atomic<uint64_t> mOutputSequence { 0 };
	std::atomic<bool> mUsbShutdownInProgress { false };
	
	uint64_t mHwSampleTime = 0;
	uint64_t mAnchorHostTime = 0;
	bool mWasDriverReady = false;
};

#endif