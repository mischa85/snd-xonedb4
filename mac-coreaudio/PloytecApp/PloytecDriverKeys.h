#ifndef PloytecDriverKeys_h
#define PloytecDriverKeys_h

enum PloytecDriverExternalMethod
{
	PloytecDriverExternalMethod_Open,
	PloytecDriverExternalMethod_Close,
	PloytecDriverExternalMethod_GetFirmwareVer,
	PloytecDriverExternalMethod_GetDeviceName,
	PloytecDriverExternalMethod_GetDeviceManufacturer,
	PloytecDriverExternalMethod_GetPlaybackStats,
	PloytecDriverExternalMethod_RegisterForMIDINotification,
};

#endif /* PloytecDriverKeys_h */
