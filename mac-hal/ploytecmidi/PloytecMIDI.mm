#include "PloytecMIDI.h"
#include "../shared/PloytecSharedData.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <errno.h>

static PloytecSharedMemory* gSharedMem = nullptr;
static MIDIEndpointRef gMidiSourceRef = 0;
static MIDIDeviceRef gMidiDeviceRef = 0;
static MIDIDriverRef gDriverRef = nullptr;

static std::atomic<bool> gThreadShouldRun{false};
static pthread_t gPollThread = nullptr;
static uint32_t gLastSessionID = 0;
static uint32_t gErrorLogCounter = 0;

static os_log_t GetLog() {
	static os_log_t log = os_log_create("hackerman.ploytecmidi", "driver");
	return log;
}

static void SetDeviceOnline(bool online) {
	if (gMidiDeviceRef) {
		SInt32 offline = online ? 0 : 1;
		MIDIObjectSetIntegerProperty(gMidiDeviceRef, kMIDIPropertyOffline, offline);
		
		if (online && gSharedMem && gSharedMem->productName[0] != 0) {
			CFStringRef newName = CFStringCreateWithCString(NULL, gSharedMem->productName, kCFStringEncodingUTF8);
			if (newName) {
				MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyName, newName);
				CFRelease(newName);
			}
			
			CFStringRef newMfg = CFStringCreateWithCString(NULL, gSharedMem->manufacturerName, kCFStringEncodingUTF8);
			if (newMfg) {
				MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyManufacturer, newMfg);
				CFRelease(newMfg);
			}
		}
	}
}

static void EnsureMIDIDeviceCreated() {
	char nameBuf[64] = "Ploytec MIDI";
	char mfgBuf[64] = "Ploytec";

	if (gSharedMem) {
		if (gSharedMem->productName[0] != 0) strlcpy(nameBuf, gSharedMem->productName, 64);
		if (gSharedMem->manufacturerName[0] != 0) strlcpy(mfgBuf, gSharedMem->manufacturerName, 64);
	}

	CFStringRef deviceName = CFStringCreateWithCString(NULL, nameBuf, kCFStringEncodingUTF8);
	CFStringRef vendorName = CFStringCreateWithCString(NULL, mfgBuf, kCFStringEncodingUTF8);

	if (gMidiDeviceRef != 0) {
		if (deviceName) MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyName, deviceName);
		if (vendorName) MIDIObjectSetStringProperty(gMidiDeviceRef, kMIDIPropertyManufacturer, vendorName);
		SetDeviceOnline(true);
	} else {
		os_log_info(GetLog(), "[PloytecMIDI] Creating Device: '%{public}s'", nameBuf);
		OSStatus err = MIDIDeviceCreate(gDriverRef, deviceName, vendorName, deviceName, &gMidiDeviceRef);
		
		if (err == noErr) {
			MIDIEntityRef ent;
			MIDIDeviceAddEntity(gMidiDeviceRef, CFSTR("MIDI"), false, 1, 1, &ent);
			gMidiSourceRef = MIDIEntityGetSource(ent, 0);
			SInt32 val = 1;
			MIDIObjectSetIntegerProperty(gMidiDeviceRef, kMIDIPropertyDriverOwner, val);
			SetDeviceOnline(true);
		} else {
			os_log_error(GetLog(), "[PloytecMIDI] Failed to create device: %{public}d", (int)err);
		}
	}
	
	if (deviceName) CFRelease(deviceName);
	if (vendorName) CFRelease(vendorName);
}

static void DisconnectSharedMemory() {
	if (gSharedMem) {
		munmap(gSharedMem, sizeof(PloytecSharedMemory));
		gSharedMem = nullptr;
		os_log_info(GetLog(), "[PloytecMIDI] Disconnected from HAL");
		SetDeviceOnline(false);
	}
}

static bool UpdateConnectionState() {
	if (gSharedMem) {
		if (gSharedMem->magic != 0x504C4F59) {
			os_log_error(GetLog(), "[PloytecMIDI] Bad Magic. Disconnecting.");
			DisconnectSharedMemory();
			return false;
		}
		if (gSharedMem->sessionID != gLastSessionID) {
			os_log_info(GetLog(), "[PloytecMIDI] RAM Session Changed. Reconnecting.");
			DisconnectSharedMemory();
			return false;
		}
		return true;
	}

	int fd = shm_open(kPloytecSharedMemName, O_RDWR, 0666);
	if (fd == -1) {
		if (++gErrorLogCounter > 200) { gErrorLogCounter = 0; }
		return false;
	}

	struct stat sb;
	if (fstat(fd, &sb) == -1) { close(fd); return false; }
	if (sb.st_size < sizeof(PloytecSharedMemory)) { close(fd); return false; }

	void* ptr = mmap(NULL, sizeof(PloytecSharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);

	if (ptr == MAP_FAILED) return false;

	gSharedMem = (PloytecSharedMemory*)ptr;
	
	if (gSharedMem->magic != 0x504C4F59) {
		munmap(ptr, sizeof(PloytecSharedMemory));
		gSharedMem = nullptr;
		return false;
	}

	gLastSessionID = gSharedMem->sessionID;
	
	os_log_info(GetLog(), "[PloytecMIDI] Connected (Session: 0x%{public}08X)", gLastSessionID);
	EnsureMIDIDeviceCreated();
	return true;
}

static int GetExpectedDataLength(uint8_t status) {
	if (status >= 0xF0) {
		switch (status) {
			case 0xF1: return 1;
			case 0xF2: return 2;
			case 0xF3: return 1;
			default: return 0;
		}
	}
	uint8_t high = status & 0xF0;
	return (high == 0xC0 || high == 0xD0) ? 1 : 2;
}

static void* MidiInputPollThread(void* arg) {
	os_log_info(GetLog(), "[PloytecMIDI] Poll Thread Started");
	Byte msgBuffer[16];
	int msgIndex = 0;
	int msgExpected = 0;
	int checkCounter = 0;
	
	while (gThreadShouldRun.load()) {
		if (!gSharedMem) {
			if (!UpdateConnectionState()) {
				usleep(50000);
				continue;
			}
		} else {
			if (++checkCounter > 400) {
				checkCounter = 0;
				int fd = shm_open(kPloytecSharedMemName, O_RDONLY, 0666);
				if (fd == -1) {
					DisconnectSharedMemory();
					continue;
				}
				void* t = mmap(NULL, sizeof(PloytecSharedMemory), PROT_READ, MAP_SHARED, fd, 0);
				close(fd);
				if (t != MAP_FAILED) {
					PloytecSharedMemory* tm = (PloytecSharedMemory*)t;
					if (tm->magic == 0x504C4F59 && tm->sessionID != gSharedMem->sessionID) {
						os_log_info(GetLog(), "[PloytecMIDI] Disk SessionID changed. Reconnecting.");
						DisconnectSharedMemory();
					}
					munmap(t, sizeof(PloytecSharedMemory));
				}
			}
		}

		if (!gSharedMem) continue;
		
		uint32_t r = std::atomic_load_explicit(&gSharedMem->midiIn.readIndex, std::memory_order_relaxed);
		uint32_t w = std::atomic_load_explicit(&gSharedMem->midiIn.writeIndex, std::memory_order_acquire);
		bool didWork = false;
		
		while (r != w) {
			didWork = true;
			uint8_t byte = gSharedMem->midiIn.buffer[r];
			r = (r + 1) & kMidiRingMask;
			
			if (byte >= 0xF8) {
				Byte rtBuf[1] = { byte };
				if (gMidiSourceRef) {
					MIDIPacketList packetList;
					MIDIPacket *curPacket = MIDIPacketListInit(&packetList);
					curPacket = MIDIPacketListAdd(&packetList, sizeof(packetList), curPacket, mach_absolute_time(), 1, rtBuf);
					if (curPacket) MIDIReceived(gMidiSourceRef, &packetList);
				}
				continue;
			}
			
			if (byte >= 0x80) {
				msgBuffer[0] = byte;
				msgIndex = 1;
				msgExpected = 1 + GetExpectedDataLength(byte);
				
				if (byte == 0xF6 || byte == 0xF7 || msgExpected == 1) {
					if (gMidiSourceRef) {
						MIDIPacketList packetList;
						MIDIPacket *curPacket = MIDIPacketListInit(&packetList);
						curPacket = MIDIPacketListAdd(&packetList, sizeof(packetList), curPacket, mach_absolute_time(), 1, msgBuffer);
						if (curPacket) MIDIReceived(gMidiSourceRef, &packetList);
					}
					msgIndex = 0; msgExpected = 0;
				}
				continue;
			}
			
			if (msgIndex > 0 && msgIndex < 16) {
				msgBuffer[msgIndex++] = byte;
				if (msgIndex == msgExpected) {
					if (gMidiSourceRef) {
						MIDIPacketList packetList;
						MIDIPacket *curPacket = MIDIPacketListInit(&packetList);
						curPacket = MIDIPacketListAdd(&packetList, sizeof(packetList), curPacket, mach_absolute_time(), msgIndex, msgBuffer);
						if (curPacket) MIDIReceived(gMidiSourceRef, &packetList);
					}
					msgIndex = 0; msgExpected = 0;
				}
			}
		}
		
		if (didWork) {
			std::atomic_store_explicit(&gSharedMem->midiIn.readIndex, r, std::memory_order_release);
		} else {
			usleep(500);
		}
	}
	
	DisconnectSharedMemory();
	if (gMidiDeviceRef) { MIDIDeviceDispose(gMidiDeviceRef); gMidiDeviceRef = 0; }
	return NULL;
}

static OSStatus FindDevices(MIDIDriverRef self, MIDIDeviceListRef devList) {
	if (gMidiDeviceRef) MIDIDeviceListAddDevice(devList, gMidiDeviceRef);
	return noErr;
}
static OSStatus Configure(MIDIDriverRef, MIDIDeviceRef) { return noErr; }
static OSStatus EnableSource(MIDIDriverRef, MIDIEndpointRef, Boolean) { return noErr; }
static OSStatus Flush(MIDIDriverRef, MIDIEndpointRef, void*, void*) { return noErr; }
static OSStatus Monitor(MIDIDriverRef, MIDIEndpointRef, const MIDIPacketList*) { return noErr; }

static OSStatus Start(MIDIDriverRef self, MIDIDeviceListRef devList) {
	gDriverRef = self;
	if (!gThreadShouldRun.load()) {
		gThreadShouldRun.store(true);
		pthread_create(&gPollThread, NULL, MidiInputPollThread, NULL);
	}
	return noErr;
}

static OSStatus Stop(MIDIDriverRef self) {
	if (gThreadShouldRun.load()) {
		gThreadShouldRun.store(false);
		pthread_join(gPollThread, NULL);
		gPollThread = nullptr;
	}
	DisconnectSharedMemory();
	if (gMidiDeviceRef) { MIDIDeviceDispose(gMidiDeviceRef); gMidiDeviceRef = 0; }
	gDriverRef = nullptr;
	return noErr;
}

static OSStatus Send(MIDIDriverRef self, const MIDIPacketList *pktlist, void *destRefCon1, void *destRefCon2) {
	if (!gSharedMem) UpdateConnectionState();
	if (!gSharedMem) return kMIDINoConnection;
	
	const MIDIPacket *packet = &pktlist->packet[0];
	for (unsigned int i = 0; i < pktlist->numPackets; ++i) {
		for (unsigned int j = 0; j < packet->length; ++j) {
			uint32_t w = std::atomic_load_explicit(&gSharedMem->midiOut.writeIndex, std::memory_order_relaxed);
			uint32_t r = std::atomic_load_explicit(&gSharedMem->midiOut.readIndex, std::memory_order_acquire);
			uint32_t nextW = (w + 1) & kMidiRingMask;
			if (nextW != r) {
				gSharedMem->midiOut.buffer[w] = packet->data[j];
				std::atomic_store_explicit(&gSharedMem->midiOut.writeIndex, nextW, std::memory_order_release);
			}
		}
		packet = MIDIPacketNext(packet);
	}
	return noErr;
}

static HRESULT QueryInterface(void* self, REFIID iid, void** out) {
	CFUUIDBytes interfaceBytes = CFUUIDGetUUIDBytes(kPloytecInterfaceUUID);
	if (memcmp(&iid, &interfaceBytes, sizeof(REFIID)) == 0) {
		*out = self;
		((PloytecMIDIDriver*)self)->_refCount++;
		return 0;
	}
	*out = NULL;
	return 0x80004002;
}
static ULONG AddRef(void* self) { return ++((PloytecMIDIDriver*)self)->_refCount; }
static ULONG Release(void* self) {
	PloytecMIDIDriver* driver = (PloytecMIDIDriver*)self;
	if (--driver->_refCount == 0) { free(driver); return 0; }
	return driver->_refCount;
}

static MIDIDriverInterface gPloytecInterfaceTable = {
	NULL, QueryInterface, AddRef, Release, FindDevices, Start, Stop, Configure, Send, EnableSource, Flush, Monitor
};

extern "C" {
	__attribute__((visibility("default")))
	void* PloytecMIDIFactory(CFAllocatorRef allocator, CFUUIDRef typeID) {
		if (CFEqual(typeID, kPloytecMIDIDriverTypeID)) {
			PloytecMIDIDriver* driver = (PloytecMIDIDriver*)calloc(1, sizeof(PloytecMIDIDriver));
			driver->_vtbl = &gPloytecInterfaceTable;
			driver->_refCount = 1;
			return driver;
		}
		return NULL;
	}
}
