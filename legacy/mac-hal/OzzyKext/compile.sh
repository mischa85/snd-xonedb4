#!/bin/bash
set -e

SDK_ROOT="$(pwd)/MacOSX26.1.sdk"
KERNEL_HEADERS="$SDK_ROOT/System/Library/Frameworks/Kernel.framework/Headers"
KEXT="OzzyKext"

rm -rf ${KEXT}.kext
mkdir -p ${KEXT}.kext/Contents/MacOS
cp Info.plist ${KEXT}.kext/Contents/

echo "🔨 Compiling ${KEXT}..."

# FLAGS:
# -I. : Look in current directory FIRST (Fixes SharedData include)
# -Wno-... : Silence the override warnings
CFLAGS="-x c++ -std=c++11 -mkernel -fno-builtin -fno-stack-protector -fno-common \
   -Wno-deprecated-declarations \
   -Wno-inconsistent-missing-override \
   -Wno-unused-private-field \
   -arch arm64e -mmacosx-version-min=12.0 \
   -isysroot $SDK_ROOT \
   -I. -I$KERNEL_HEADERS \
   -include OzzyPrefix.h \
   -DKERNEL -DKERNEL_PRIVATE -D__KERNEL__"

cc $CFLAGS -c OzzyKext.cpp -o OzzyKext.o
cc $CFLAGS -c PloytecUSB.cpp -o PloytecUSB.o

echo "🔗 Linking..."

cc -arch arm64e -mkernel -nostdlib -Xlinker -kext -lkmod \
   -undefined dynamic_lookup \
   -o ${KEXT}.kext/Contents/MacOS/${KEXT} \
   OzzyKext.o PloytecUSB.o

echo "✅ Done."