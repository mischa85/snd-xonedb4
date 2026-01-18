#!/bin/bash

if [ "$EUID" -ne 0 ]; then 
  echo "Please run as root"
  exit 1
fi

echo "Unloading module..."
rmmod snd_usb_xonedb4 2>/dev/null

KERNEL_VER=$(uname -r)
DEST_DIR="/lib/modules/$KERNEL_VER/kernel/sound/usb/ploytec"

if [ -d "$DEST_DIR" ]; then
    echo "Removing driver files..."
    rm -rf "$DEST_DIR"
    depmod -a
    echo "✅ Uninstalled."
else
    echo "Driver not found."
fi
