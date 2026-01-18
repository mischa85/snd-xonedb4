#include <iostream>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include "../Shared/PloytecSharedData.h" 

int main() {
    std::cout << "🔍 Looking for OzzyKext..." << std::endl;

    // 1. Find the Service
    CFMutableDictionaryRef matching = IOServiceMatching("OzzyKext");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matching);
    
    if (!service) {
        std::cerr << "❌ OzzyKext not found! Is it loaded?" << std::endl;
        return -1;
    }
    std::cout << "✅ Found Service." << std::endl;

    // 2. Open User Client
    io_connect_t connect;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connect);
    IOObjectRelease(service);
    
    if (kr != KERN_SUCCESS) {
        std::cerr << "❌ Failed to open User Client (Error: 0x" << std::hex << kr << ")" << std::endl;
        return -1;
    }
    std::cout << "✅ Connected to Driver." << std::endl;

    // 3. Map Shared Memory (Type 0)
    // NOTE: In user space, we just deal with addresses, not Descriptors.
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    
    kr = IOConnectMapMemory(connect, 0, mach_task_self(), &addr, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) {
        std::cerr << "❌ Failed to map memory (Error: 0x" << std::hex << kr << ")" << std::endl;
        IOServiceClose(connect);
        return -1;
    }
    
    // Cast the raw address to our shared struct
    volatile PloytecSharedMemory* shm = (volatile PloytecSharedMemory*)addr;
    
    std::cout << "✅ Memory Mapped at 0x" << std::hex << addr << " (" << std::dec << size << " bytes)" << std::endl;
    std::cout << "---------------------------------------------------" << std::endl;
    std::cout << "Product: " << (char*)shm->productName << std::endl;
    std::cout << "Session: " << shm->sessionID << std::endl;
    
    // 4. Monitor Loop
    uint32_t lastSeq = 0;
    while (true) {
        // Read volatile fields
        bool hardware = shm->audio.hardwarePresent;
        bool driver = shm->audio.driverReady;
        uint32_t seq = shm->audio.timestamp.sequence;
        uint64_t sampleTime = shm->audio.timestamp.sampleTime;
        uint32_t r = shm->midiOut.readIndex;
        uint32_t w = shm->midiOut.writeIndex;
        
        // Print status (Carriage return \r overwrites the line)
        printf("\r[Status] HW:%d Rdy:%d | Seq: %-10u | Time: %-10llu | MIDI R/W: %u/%u", 
               hardware, driver, seq, sampleTime, r, w);
        fflush(stdout);
        
        if (seq == lastSeq && hardware) {
            // Optional: detect stalls
        }
        lastSeq = seq;
        
        usleep(100000); // 100ms update rate
    }

    return 0;
}