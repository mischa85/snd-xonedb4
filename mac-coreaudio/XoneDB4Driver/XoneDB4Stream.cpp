//
//  XoneDB4Stream.cpp
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 12/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

#include <AudioDriverKit/AudioDriverKit.h>
#include "XoneDB4Stream.h"

kern_return_t XoneDB4Stream::StartIO(IOUserAudioStartStopFlags in_flags)
{
	os_log(OS_LOG_DEFAULT, "STARTIOSTREAM CALLED");

	return kIOReturnSuccess;
}

kern_return_t XoneDB4Stream::StopIO(IOUserAudioStartStopFlags in_flags)
{
	os_log(OS_LOG_DEFAULT, "STOPIOSTREAM CALLED");

	return kIOReturnSuccess;
}
