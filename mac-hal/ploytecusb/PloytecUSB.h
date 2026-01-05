#ifndef PloytecUSB_h
#define PloytecUSB_h

#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <atomic>
#include <dispatch/dispatch.h>
#include "../shared/PloytecSharedData.h"

static constexpr uint8_t kPcmOutEp = 0x05;
static constexpr uint8_t kPcmInEp = 0x86;
static constexpr uint8_t kMidiInEp = 0x83;
static constexpr uint32_t kDefaultUrbs = 2;

struct FirmwareVersion {
	uint8_t ID;
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
};

enum TransferMode {
	kTransferModeBulk = 0,
	kTransferModeInterrupt = 1,
};

extern os_log_t gUSBEngineLog;
inline os_log_t GetLog() {
	if (!gUSBEngineLog) gUSBEngineLog = os_log_create("hackerman.ploytecusb", "engine");
	return gUSBEngineLog;
}

class PloytecUSB {
public:
	static PloytecUSB& GetInstance();
	void Run();

	PloytecUSB(const PloytecUSB&) = delete;
	void operator=(const PloytecUSB&) = delete;

private:
	PloytecUSB();
	~PloytecUSB();

	enum PloytecStatusMask : uint8_t {
		kStatusSyncReady       = 0x01,
		kStatusUsbInterfaceOn  = 0x02,
		kStatusDigitalLock     = 0x04,
		kStatusStreamingArmed  = 0x10,
		kStatusLegacyActive    = 0x20,
		kStatusHighSpeedActive = 0x80
	};

	io_service_t mUSBService = IO_OBJECT_NULL;
	IOUSBDeviceInterface** mUsbDevice = nullptr;
	IOUSBInterfaceInterface** mUsbInterface0 = nullptr;
	IOUSBInterfaceInterface** mUsbInterface1 = nullptr;

	CFStringRef mVendorName = nullptr;
	CFStringRef mProductName = nullptr;
	CFStringRef mSerialNumber = nullptr;

	uint16_t mVendorID = 0;
	uint16_t mProductID = 0;
	FirmwareVersion mFW = {};

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

	CFRunLoopRef mRunLoop = nullptr;
	IONotificationPortRef mNotifyPort = nullptr;
	CFRunLoopSourceRef mInterface0Src = nullptr;
	CFRunLoopSourceRef mInterface1Src = nullptr;

	io_iterator_t mAddedIter = IO_OBJECT_NULL;
	io_iterator_t mRemovedIter = IO_OBJECT_NULL;

	PloytecSharedMemory* mSHM = nullptr;
	int mShmFd = -1;

	uint8_t* mRxBufferControl = nullptr;
	uint8_t* mTxBufferControl = nullptr;

	mach_timebase_info_data_t mTimebase;
	uint64_t mHwSampleTime = 0;
	uint64_t mLastZeroReported = 0;
	uint64_t mAnchorHostTime = 0;
	bool mWasDriverReady = false;

	std::atomic<uint64_t> mInputSequence { 0 };
	std::atomic<uint64_t> mOutputSequence { 0 };
	std::atomic<uint64_t> mLastInputTime { 0 };

	std::atomic<bool> mUsbShutdownInProgress { false };
	std::atomic<bool> mIsResetting { false };
	std::atomic<bool> mIsRecovering { false };
	std::atomic<uint64_t> mRecoveryStartTime { 0 };
	dispatch_source_t mWatchdogTimer = nullptr;

	IOUSBInterfaceInterface** GetInterface(uint8_t index);
	void SetupSharedMemory();
	void DestroySharedMemory();

	static void* USBWorkLoop(void* arg);
	static void DeviceAdded(void* refCon, io_iterator_t iterator);
	static void DeviceRemoved(void* refCon, io_iterator_t iterator);

	bool OpenUSBDevice(io_service_t service);
	void CloseUSBDevice();
	bool CreateUSBPipes();

	bool StartStreaming(uint8_t urbCount);
	bool StopStreaming();
	bool DetectTransferMode();
	bool ConfigureStreamingFormat();

	bool SubmitPCMin(uint32_t packetIndex);
	bool SubmitPCMout(uint32_t packetIndex);
	bool SubmitMIDIin();

	static void PCMinComplete(void* refCon, IOReturn result, void* arg0);
	static void PCMoutComplete(void* refCon, IOReturn result, void* arg0);
	static void MIDIinComplete(void* refCon, IOReturn result, void* arg0);

	bool ReadFirmwareVersion();
	bool ReadHardwareStatus();
	bool WriteHardwareStatus(uint16_t value);
	bool GetHardwareFrameRate();
	bool SetHardwareFrameRate(uint32_t framerate);
};

#endif