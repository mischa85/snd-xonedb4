#!/bin/bash

# ==========================================
#  Ploytec Linux ALSA Driver Installer
# ==========================================

if [ "$EUID" -ne 0 ]; then 
  echo "Please run as root (sudo ./install_linux.sh)"
  exit 1
fi

SRC_DIR="$DIR/linux-alsa" 

if [ ! -d "$SRC_DIR" ]; then
    # Fallback if the files are in the root for some reason
    SRC_DIR="$DIR"
fi

echo "Compiling..."
cd "$SRC_DIR"
make
if [ $? -ne 0 ]; then
    echo "❌ Build failed."
    exit 1
fi

MODULE_NAME="snd-usb-xonedb4.ko"
KERNEL_VER=$(uname -r)
DEST_DIR="/lib/modules/$KERNEL_VER/kernel/sound/usb/ploytec"

echo "Installing $MODULE_NAME to $DEST_DIR..."
mkdir -p "$DEST_DIR"
cp "$MODULE_NAME" "$DEST_DIR/"

echo "Updating module dependencies..."
depmod -a

echo "Loading module..."
modprobe snd-usb-xonedb4

# Check if loaded
if lsmod | grep -q "snd_usb_xonedb4"; then
    echo "✅ Module loaded successfully."
    dmesg | tail -n 5
else
    echo "⚠️  Module failed to load immediately. Check dmesg."
fi