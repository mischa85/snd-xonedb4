#ifndef OzzyKext_h
#define OzzyKext_h

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include "../Shared/PloytecSharedData.h"

// Forward Declarations (Break Header Dependencies)
class IOUSBHostDevice;
class IOUSBHostInterface;
class IOUSBHostPipe;

class OzzyKext : public IOService {
	OSDeclareAbstractStructors(OzzyKext)

public:
	virtual bool start(IOService* provider) override;
	virtual void stop(IOService* provider) override;
	virtual IOReturn newUserClient(task_t t, void* sec, UInt32 type, OSDictionary* p, IOUserClient** h) override;

	IOMemoryDescriptor* GetMemory() { return mMem; }
	PloytecSharedMemory* GetSHM() { return mSHM; }

protected:
	// Virtual, but not pure. Prevents "Abstract Class" allocation errors.
	virtual bool ConfigureHardware() { return false; }
	
	IOUSBHostDevice* mHostDevice = nullptr;
	IOBufferMemoryDescriptor* mMem = nullptr;
	PloytecSharedMemory* mSHM = nullptr;
};

class OzzyUserClient : public IOUserClient {
	OSDeclareDefaultStructors(OzzyUserClient)
public:
	virtual bool initWithTask(task_t t, void* s, UInt32 type, OSDictionary* p) override;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits* options, IOMemoryDescriptor** memory) override;
private:
	OzzyKext* mOwner = nullptr;
};

#endif