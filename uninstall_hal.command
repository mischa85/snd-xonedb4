#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

clear
echo "====================================================="
echo "      Ploytec USB Driver Uninstaller"
echo "====================================================="
echo ""

echo "This will remove:"
echo "   - /Library/Audio/Plug-Ins/HAL/ploytechal.driver"
echo "   - /Library/Audio/MIDI Drivers/ploytecmidi.plugin"
echo ""

sudo -v
if [ $? -ne 0 ]; then
    echo "❌ Authentication failed. Exiting."
    exit 1
fi

echo ""
echo "🗑️  Uninstalling..."

if [ -d "/Library/Audio/Plug-Ins/HAL/ploytechal.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/ploytechal.driver"
    echo "   - Removed Audio Driver"
else
    echo "   - Audio Driver not found (skipping)"
fi

if [ -d "/Library/Audio/MIDI Drivers/ploytecmidi.plugin" ]; then
    sudo rm -rf "/Library/Audio/MIDI Drivers/ploytecmidi.plugin"
    echo "   - Removed MIDI Driver"
else
    echo "   - MIDI Driver not found (skipping)"
fi

echo "   - Restarting Audio Services..."
sudo killall coreaudiod
sudo killall com.apple.audio.Core-Audio-Driver-Service.helper 2>/dev/null || true

echo "   - Restarting MIDI Server..."
sudo killall MIDIServer 2>/dev/null || true
sudo touch "/Library/Audio/MIDI Drivers"

echo ""
echo "✅ Uninstallation Complete."
echo ""
read -p "Press [Enter] to close..."