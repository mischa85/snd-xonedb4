//
//  PloytecDriverKeys.h
//  PloytecApp
//
//  Created by Marcel Bierling on 04/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#ifndef PloytecDriverKeys_h
#define PloytecDriverKeys_h

enum PloytecDriverExternalMethod
{
	PloytecDriverExternalMethod_Open,
	PloytecDriverExternalMethod_Close,
	PloytecDriverExternalMethod_GetFirmwareVer,
	PloytecDriverExternalMethod_GetDeviceName,
	PloytecDriverExternalMethod_GetDeviceManufacturer,
	PloytecDriverExternalMethod_ChangeBufferSize,
	PloytecDriverExternalMethod_GetPlaybackStats,
};

#endif /* PloytecDriverKeys_h */
