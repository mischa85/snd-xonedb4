#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

clear
echo "====================================================="
echo "      Ploytec Audio Engine Uninstaller"
echo "====================================================="
echo ""

echo "This will remove:"
echo "   - /usr/local/bin/ploytecusb (Daemon)"
echo "   - /Library/LaunchDaemons/hackerman.ploytecusb.plist"
echo "   - /Library/Audio/Plug-Ins/HAL/PloytecAudio.driver"
echo "   - /Library/Audio/MIDI Drivers/PloytecMIDI.plugin"
echo ""

sudo -v
if [ $? -ne 0 ]; then
    echo "❌ Authentication failed. Exiting."
    exit 1
fi

echo ""
echo "🛑 Stopping USB Service..."
sudo launchctl unload /Library/LaunchDaemons/hackerman.ploytecusb.plist 2>/dev/null || true

echo "🗑️  Removing Files..."
sudo rm -f /usr/local/bin/ploytecusb
sudo rm -f /Library/LaunchDaemons/hackerman.ploytecusb.plist

if [ -d "/Library/Audio/Plug-Ins/HAL/PloytecAudio.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/PloytecAudio.driver"
    echo "   - Removed Audio Driver"
fi

if [ -d "/Library/Audio/MIDI Drivers/PloytecMIDI.plugin" ]; then
    sudo rm -rf "/Library/Audio/MIDI Drivers/PloytecMIDI.plugin"
    echo "   - Removed MIDI Driver"
fi

# Also clean up old legacy names if present
if [ -d "/Library/Audio/Plug-Ins/HAL/ploytechal.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/ploytechal.driver"
    echo "   - Removed Legacy Audio Driver"
fi

echo "🔄 Restarting Audio Services..."
sudo killall coreaudiod
sudo killall MIDIServer 2>/dev/null || true
sudo touch "/Library/Audio/MIDI Drivers"

echo ""
echo "✅ Uninstallation Complete."
read -p "Press [Enter] to close..."
