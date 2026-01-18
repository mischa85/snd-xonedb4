#include "OzzyEngine.h"

// The base class implementation is minimal because most logic is protocol-specific.
// However, we can implement shared helpers here if needed in the future.

// Currently, the Init method is inline in the header, and Start/Stop are pure virtual.
// If we add shared state management (e.g. tracking "isRunning"), it goes here.

// For now, this file exists to ensure the vtable is emitted and to provide a home 
// for any future shared logic (like generic ring buffer utilities).

void OzzyEngine::Init(IOzzyBus* bus, void* sharedMemory) {
    mBus = bus;
    mSharedMemory = sharedMemory;
    
    if (mBus) {
        mBus->Log("OzzyEngine Base Initialized");
    }
}

// We can provide a default "do nothing" implementation for callbacks 
// so derived classes only implement what they need.

/*
void OzzyEngine::OnPacketComplete(uint8_t pipeID, void* ctx, int status, uint32_t bytes) {
    // Default: Do nothing. 
    // Derived classes (PloytecEngine) override this to handle their specific pipes.
}
*/