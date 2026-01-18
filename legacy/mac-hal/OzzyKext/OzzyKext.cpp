#include "OzzyKext.h"
#include "KextBus.h"
#include "../../OzzyCore/OzzyFactory.h"
#include "../../Shared/OzzySharedData.h"

// System Includes
#include <IOKit/IOLib.h>
#include <sys/errno.h>
#include <mach/mach_types.h>
#include <libkern/libkern.h>

// Suppress deprecation warnings for Kexts (Apple wants us to use DriverKit, but we are building a Kext)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define Log(fmt, ...) IOLog("[OzzyKext] " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(OzzyKext, IOService)

// --------------------------------------------------------------------------
// IOKit Lifecycle Methods
// --------------------------------------------------------------------------

bool OzzyKext::start(IOService* provider) {
    // 1. Call Parent Start
    if (!IOService::start(provider)) return false;
    Log("🚀 Service Starting...");

    // 2. Hook into USB Device
    mHostDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!mHostDevice) return false;
    mHostDevice->retain();
    
    // Check if we can open it (exclusive access)
    if (!mHostDevice->open(this)) {
        Log("❌ Failed to open USB Device (Exclusive Access?)");
        return false;
    }

    // 3. Setup Shared Memory
    if (!MapSharedMemory()) {
        Log("❌ Shared Memory Init Failed");
        return false;
    }

    // 4. Create the Bus (The Plumbing)
    mBus = new KextBus(mHostDevice);
    if (!mBus) return false;

    // 5. Create the Engine (The Brain)
    const StandardUSB::DeviceDescriptor* desc = mHostDevice->getDeviceDescriptor();
    uint16_t vid = desc ? desc->idVendor : 0;
    uint16_t pid = desc ? desc->idProduct : 0;

    mEngine = OzzyFactory::CreateEngine(vid, pid);
    if (!mEngine) {
        Log("❌ No Engine supports device %04x:%04x", vid, pid);
        return false;
    }
    Log("🧠 Engine Loaded for %04x:%04x", vid, pid);

    // 6. Configure Hardware (The Robust Retry Loop)
    if (!ConfigureHardware()) {
        Log("❌ Hardware Config Failed");
        return false;
    }

    // 7. Link & Launch
    mBus->SetPipes(mPipePcmOut, mPipePcmIn, mPipeMidiIn);
    
    // Pass the raw pointer to the Engine (it casts it to OzzySharedMemory*)
    mEngine->Init(mBus, mSHM);
    
    if (!mEngine->Start()) {
        Log("❌ Engine Start Failed");
        return false;
    }

    Log("✅ Driver Fully Active");
    return true;
}

void OzzyKext::stop(IOService* provider) {
    Log("🛑 Stop");

    // 1. STOP THE NOISE (Critical: Kill Pipes First)
    // We must abort all I/O and release pipes before destroying the Engine/Bus.
    // This prevents callbacks from firing on dead objects.
    for (int i=0; i < kMaxPipes; i++) {
        if (mPipes[i]) { 
            mPipes[i]->abort(); // Cancel pending I/O synchronously
            mPipes[i]->close(this);
            mPipes[i]->release(); 
            mPipes[i] = nullptr; 
        }
    }

    // 2. NOW it is safe to stop the Engine
    // (No new packets can trigger callbacks now)
    if (mEngine) { 
        mEngine->Stop(); 
        SafeKernelDelete(mEngine); 
        mEngine = nullptr; 
    }
    
    // 3. Destroy Bus
    if (mBus) { 
        SafeKernelDelete(mBus); 
        mBus = nullptr; 
    }
    
    // 4. Close Interfaces
    for (int i=0; i < kMaxInterfaces; i++) {
        if (mInterfaces[i]) { 
            mInterfaces[i]->close(this); 
            mInterfaces[i]->release(); 
            mInterfaces[i] = nullptr; 
        }
    }
    
    UnmapSharedMemory();
    
    if (mHostDevice) { 
        mHostDevice->close(this); 
        mHostDevice->release(); 
        mHostDevice = nullptr; 
    }
    
    IOService::stop(provider);
}

// --------------------------------------------------------------------------
// Setup Helpers
// --------------------------------------------------------------------------

bool OzzyKext::MapSharedMemory() {
    IOBufferMemoryDescriptor* mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIOMemoryPhysicallyContiguous, sizeof(OzzySharedMemory), 0);
    
    if (!mem) return false;
    
    if (mem->prepare() != kIOReturnSuccess) {
        mem->release(); return false;
    }
    
    mShmMap = mem->map();
    mSHM = (OzzySharedMemory*)mShmMap->getVirtualAddress();
    memset(mSHM, 0, sizeof(OzzySharedMemory));
    
    // Setup Header
    mSHM->magic = kOzzyMagic;
    mSHM->version = 1;
    
    return true;
}

void OzzyKext::UnmapSharedMemory() {
    if (mShmMap) { mShmMap->release(); mShmMap = nullptr; }
    mSHM = nullptr;
}

bool OzzyKext::ConfigureHardware() {
    // 1. Set Configuration
    // IOUSBHostDevice doesn't let us easily check "current" config,
    // so we just attempt to set it. If it fails, we log it but proceed,
    // assuming it might already be set or will be handled by the interface lookup.
    IOReturn err = mHostDevice->setConfiguration(1);
    if (err != kIOReturnSuccess) {
        Log("⚠️ setConfiguration(1) returned 0x%08x (Device may already be configured)", err);
    }
    IOSleep(200); // Give firmware time to enumerate endpoints

    // 2. Interface Retry Loop (Wait for macOS to publish interfaces)
    int attempts = 0;
    bool found0 = false, found1 = false;
    const char* planes[] = { "IOService", "IOUSB" };

    while (attempts < 50) { 
        for (int p = 0; p < 2; p++) {
            const IORegistryPlane* plane = getPlane(planes[p]);
            if (!plane) continue;
            
            OSIterator* it = mHostDevice->getChildIterator(plane);
            if (it) {
                IOUSBHostInterface* iface = nullptr;
                while ((iface = OSDynamicCast(IOUSBHostInterface, it->getNextObject()))) {
                    const StandardUSB::InterfaceDescriptor* d = iface->getInterfaceDescriptor();
                    if (!d) continue;

                    if (d->bInterfaceNumber == 0 && !mIntf0) { 
                        if (iface->open(this)) {
                            iface->retain(); mIntf0 = iface;
                            mIntf0->selectAlternateSetting(1);
                            found0 = true;
                            Log("✅ Interface 0 Secured");
                        }
                    }
                    if (d->bInterfaceNumber == 1 && !mIntf1) { 
                        if (iface->open(this)) {
                            iface->retain(); mIntf1 = iface;
                            mIntf1->selectAlternateSetting(1);
                            found1 = true;
                            Log("✅ Interface 1 Secured");
                        }
                    }
                }
                it->release();
            }
            if (found0 && found1) break;
        }
        if (found0 && found1) break;
        IOSleep(100); attempts++;
    }

    if (!found0 && !found1) {
        Log("❌ Timeout waiting for interfaces");
        return false;
    }

    // 3. Create Pipes
    if (mIntf0) {
        mPipePcmOut = mIntf0->copyPipe(0x05); 
        mPipeMidiIn = mIntf0->copyPipe(0x83); 
    }
    if (mIntf1) {
        mPipePcmIn = mIntf1->copyPipe(0x86);  
    }

    if (!mPipePcmOut || !mPipePcmIn) return false;

    return true;
}

// --------------------------------------------------------------------------
// Kernel Module Entry Points (kmod_info)
// --------------------------------------------------------------------------

extern "C" {
    kern_return_t OzzyKext_start(kmod_info_t * ki, void *d) { return KERN_SUCCESS; }
    kern_return_t OzzyKext_stop(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }

    __attribute__((visibility("default"))) kmod_start_func_t *_realmain = OzzyKext_start;
    __attribute__((visibility("default"))) kmod_stop_func_t  *_antimain = OzzyKext_stop;

    __attribute__((visibility("default"))) kmod_info_t kmod_info = {
        0, KMOD_INFO_VERSION, 0, 
        "com.ozzy.kext.OzzyKext", // MUST match Info.plist
        "1.0.0", -1, 0, 0, 0, 0, 
        OzzyKext_start, OzzyKext_stop
    };
}