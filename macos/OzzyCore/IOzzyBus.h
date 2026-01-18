#ifndef IOzzyBus_h
#define IOzzyBus_h

#include <stdint.h>

class OzzyEngine;

class IOzzyBus {
public:
    virtual ~IOzzyBus() {}

    // --- Core Transport ---
    // Returns true if successfully queued.
    // contextID: A generic tag (usually packet index) passed back in OnPacketComplete
    virtual bool SubmitUSBPacket(uint8_t pipeID, void* buffer, uint32_t size, void* contextID) = 0;
    
    // Blocking Control Request (Used for Initialization)
    // type: bmRequestType, req: bRequest
    virtual bool VendorRequest(uint8_t type, uint8_t req, uint16_t val, uint16_t idx, void* data, uint16_t len) = 0;

    // --- System Services ---
    virtual void Sleep(uint32_t ms) = 0;
    virtual uint64_t GetTime() = 0; // Returns mach_absolute_time()
    virtual void Log(const char* fmt, ...) = 0;
    
    // --- Linkage ---
    virtual void SetEngine(OzzyEngine* engine) = 0;
};

#endif