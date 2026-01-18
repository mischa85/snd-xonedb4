#ifndef PloytecUSB_h
#define PloytecUSB_h

#include "OzzyKext.h"

// Forward Declarations
class IOUSBHostInterface;
class IOUSBHostPipe;

class PloytecUSB : public OzzyKext {
	OSDeclareDefaultStructors(PloytecUSB)

protected:
    // Lifecycle Overrides
	virtual bool ConfigureHardware() override;
	virtual void stop(IOService* provider) override;

private:
    // Engine Control
	void StartStreaming();
	
    // IO Submission
	void SubmitPCMin(uint32_t idx);
	void SubmitPCMout(uint32_t idx);
	void SubmitMIDIin();

    // IO Completions (Static Trampolines)
	static void PCMinComplete(OSObject* target, void* parameter, IOReturn status, uint32_t bytesTransferred);
	static void PCMoutComplete(OSObject* target, void* parameter, IOReturn status, uint32_t bytesTransferred);
	static void MIDIinComplete(OSObject* target, void* parameter, IOReturn status, uint32_t bytesTransferred);

    // Initialization Helpers (Ported from Userspace)
    bool ReadFirmwareVersion();
    bool ReadHardwareStatus();
    bool GetHardwareFrameRate();
    bool SetHardwareFrameRate(uint32_t rate);
    bool WriteHardwareStatus(uint16_t value);

    // Hardware Objects
	IOUSBHostInterface* mIntf0 = nullptr;
	IOUSBHostInterface* mIntf1 = nullptr;

	IOUSBHostPipe* mPipePcmOut = nullptr;
	IOUSBHostPipe* mPipePcmIn = nullptr;
	IOUSBHostPipe* mPipeMidiIn = nullptr;
	
    // State
	bool mRunning = false;
	bool mIsBulk = false;
	uint32_t mPacketSizeOut = 0;
	uint16_t mMidiOffset = 0;
    
    // Clock State
    uint64_t mHwSampleTime = 0;
};

#endif