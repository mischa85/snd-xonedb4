#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

clear
echo "====================================================="
echo "      Ploytec Audio Engine Uninstaller"
echo "====================================================="
echo ""

echo "This will remove:"
echo "   - /usr/local/bin/Ozzy (Daemon)"
echo "   - /Library/LaunchDaemons/Ozzy.plist"
echo "   - /Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
echo "   - /Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
echo ""

sudo -v
if [ $? -ne 0 ]; then
    echo "❌ Authentication failed. Exiting."
    exit 1
fi

echo ""
echo "🛑 Stopping Ozzy Service..."
sudo launchctl unload /Library/LaunchDaemons/Ozzy.plist 2>/dev/null || true

echo "🗑️  Removing Files..."
sudo rm -f /usr/local/bin/Ozzy
sudo rm -f /Library/LaunchDaemons/Ozzy.plist

if [ -d "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
    echo "   - Removed Audio Driver"
fi

if [ -d "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin" ]; then
    sudo rm -rf "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
    echo "   - Removed MIDI Driver"
fi

echo "🔄 Restarting Audio Services..."
sudo killall coreaudiod
sudo killall MIDIServer 2>/dev/null || true
sudo touch "/Library/Audio/MIDI Drivers"

echo ""
echo "✅ Uninstallation Complete."
read -p "Press [Enter] to close..."