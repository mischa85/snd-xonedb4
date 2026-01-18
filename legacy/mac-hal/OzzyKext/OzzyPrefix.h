#ifndef OzzyPrefix_h
#define OzzyPrefix_h

// 1. Force Kernel Mode
// These defines tell the Apple SDK: "Don't give me User Space headers."
#define KERNEL 1
#define KERNEL_PRIVATE 1
#define BSD_KERNEL_PRIVATE 1
#define __KERNEL__ 1

// 2. Block DriverKit
// Explicitly disable DriverKit logic in shared headers
#define IOUSBHOSTFAMILY_USE_DRIVERKIT 0

// 3. Common Kernel Includes
#include <libkern/c++/OSObject.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>

#endif