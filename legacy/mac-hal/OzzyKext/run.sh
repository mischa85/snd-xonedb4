#!/bin/bash

# Configuration
KEXT_NAME="OzzyKext.kext"
BUNDLE_ID="com.ozzy.kext.OzzyKext"

# 1. Unload existing instance (ignore error if not loaded)
echo "🛑 Unloading existing driver..."
sudo kextunload -b "$BUNDLE_ID" 2>/dev/null

# 2. Fix Permissions (Crucial! Kexts must be root:wheel)
echo "🔧 Fixing permissions..."
sudo chown -R root:wheel "$KEXT_NAME"
sudo chmod -R 755 "$KEXT_NAME"

# 3. Load the Kext
echo "🚀 Loading $KEXT_NAME..."
# We use kextload instead of kmutil for faster iteration in this specific legacy environment
sudo kextload -v "$KEXT_NAME"

# Check if load failed
if [ $? -ne 0 ]; then
    echo "❌ Load failed. Check permissions or SIP status."
    exit 1
fi

echo "✅ Loaded successfully!"

# 4. Stream Logs automatically
# Filters for "[OzzyKext]" or "[PloytecUSB]"
echo "👀 Streaming Logs (Press Ctrl+C to stop)..."
echo "---------------------------------------------------"
sudo log stream --predicate 'process == "kernel" AND (eventMessage CONTAINS "OzzyKext" OR eventMessage CONTAINS "Ploytec")' --info