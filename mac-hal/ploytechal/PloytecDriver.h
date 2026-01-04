#ifndef Ploytec_Driver_h
#define Ploytec_Driver_h

#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOTypes.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <os/log.h>
#include <atomic>
#include <memory>

#define kPloytecDeviceID          100
#define kPloytecInputStreamID     150
#define kPloytecOutputStreamID    200

class PloytecAudioDevice;
struct PloytecDriver_IVars;

struct FirmwareVersion {
	uint8_t ID;
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
};

enum TransferMode {
	BULK = 0,
	INTERRUPT = 1,
};

extern os_log_t gPloytecLog;
inline os_log_t GetLog() {
	if (!gPloytecLog) gPloytecLog = os_log_create("hackerman.ploytechal", "driver");
	return gPloytecLog;
}

class PloytecDriver {
public:
	static constexpr uint32_t BUFFER_SIZE_OUT = 131072;
	static constexpr uint32_t BUFFER_SIZE_IN = 163840;

	static PloytecDriver& GetInstance();
	PloytecAudioDevice* GetAudioDevice() const;

	void InitSharedMemory();
	void CleanupSharedMemory();

	void SetHost(AudioServerPlugInHostRef inHost) { mHost = inHost; }
	void Initialize();

	bool IsConnected() const { return mIsConnected.load(std::memory_order_relaxed); }
	UInt32 GetDeviceCount() const { return IsConnected() ? 1 : 0; }

	uint8_t* GetInputBuffer();
	uint8_t* GetOutputBuffer();
	TransferMode GetTransferMode();

	void ClearOutputBuffer();

	CFStringRef GetManufacturerName() const;
	CFStringRef GetProductName() const;
	CFStringRef GetSerialNumber() const;

	void NotifyDeviceListChanged();

private:
	PloytecDriver();
	~PloytecDriver();

	PloytecDriver_IVars* ivars = nullptr;

	static void* USBWorkLoop(void* arg);
	static void DeviceAdded(void* refCon, io_iterator_t iterator);
	static void DeviceRemoved(void* refCon, io_iterator_t iterator);

	bool CreateBuffers();
	void DestroyBuffers();

	bool OpenUSBDevice(io_service_t service);
	void CloseUSBDevice();
	bool CreateUSBPipes();
	bool StartStreaming(uint8_t urbCount);
	bool StopStreaming();
	bool DetectTransferMode();

	bool ReadFirmwareVersion();
	bool ReadHardwareStatus();
	bool WriteHardwareStatus(uint16_t value);
	bool GetHardwareFrameRate();
	bool SetHardwareFrameRate(uint32_t framerate);

	io_service_t mUSBService = IO_OBJECT_NULL;

	static void PCMinComplete(void* refCon, IOReturn result, void* arg0);
	static void PCMoutComplete(void* refCon, IOReturn result, void* arg0);
	static void MIDIinComplete(void* refCon, IOReturn result, void* arg0);

	bool ConfigureStreamingFormat(uint16_t inputFrames, uint16_t outputFrames);

	bool SubmitPCMin(uint32_t seg);
	bool SubmitPCMout(uint32_t seg);
	bool SubmitMIDIin();

	FirmwareVersion mFW;
	void SetConnected(bool connected);

	CFRunLoopRef mRunLoop = nullptr;
	AudioServerPlugInHostRef mHost = nullptr;
	std::atomic<bool> mIsConnected { false };

	IONotificationPortRef mNotifyPort = nullptr;
	io_iterator_t mAddedIter = IO_OBJECT_NULL;
	io_iterator_t mRemovedIter = IO_OBJECT_NULL;

	void StartWatchdog();
	void StopWatchdog();
	void RecoverDevice();
	void SendVendorReset();

	dispatch_source_t mWatchdogTimer = nullptr;
	std::atomic<bool> isRecovering { false };
	std::atomic<uint64_t> lastInputTime { 0 };

	IOReturn PingDevice();
};

#endif