#!/bin/bash

# =======================================================
#  Ozzy / Ploytec "Deep Clean" Uninstaller
#  Removes all current and legacy/cruft driver versions
# =======================================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

clear
echo "====================================================="
echo "      ☢️  NUKE LEGACY DRIVERS ☢️"
echo "====================================================="
echo ""
echo "This will forcibly remove ALL variations of:"
echo " - Ozzy / Ploytec USB Daemon"
echo " - PloytecHAL / ploytechal / PloytecAudio"
echo " - PloytecMIDI / ploytecmidi"
echo ""

# 1. Elevate Privileges
sudo -v
if [ $? -ne 0 ]; then
    echo "❌ Administrator privileges required."
    exit 1
fi

echo ""
echo "🛑 Stopping Services..."

# 2. Unload all potential LaunchDaemons
# We use '|| true' so the script doesn't stop if the service isn't found
sudo launchctl bootout system/hackerman.Ozzy 2>/dev/null || true
sudo launchctl bootout system/hackerman.ploytecusb 2>/dev/null || true
sudo launchctl unload /Library/LaunchDaemons/hackerman.Ozzy.plist 2>/dev/null || true
sudo launchctl unload /Library/LaunchDaemons/Ozzy.plist 2>/dev/null || true
sudo launchctl unload /Library/LaunchDaemons/hackerman.ploytecusb.plist 2>/dev/null || true

# 3. Kill Processes
sudo killall Ozzy 2>/dev/null || true
sudo killall ploytecusb 2>/dev/null || true

echo "🗑️  Deleting Files..."

# 4. Define List of Cruft
CRUFT_FILES=(
    # --- Daemons & Binaries ---
    "/usr/local/bin/Ozzy"
    "/usr/local/bin/ploytecusb"
    "/usr/local/bin/PloytecUSB"
    
    # --- LaunchConfigs ---
    "/Library/LaunchDaemons/hackerman.Ozzy.plist"
    "/Library/LaunchDaemons/Ozzy.plist"
    "/Library/LaunchDaemons/hackerman.ploytecusb.plist"
    
    # --- HAL Drivers (Audio) ---
    "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
    "/Library/Audio/Plug-Ins/HAL/PloytecHAL.driver"
    "/Library/Audio/Plug-Ins/HAL/ploytechal.driver"
    "/Library/Audio/Plug-Ins/HAL/PloytecAudio.driver"
    "/Library/Audio/Plug-Ins/HAL/ploytecaudio.driver"
    
    # --- MIDI Drivers ---
    "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
    "/Library/Audio/MIDI Drivers/PloytecMIDI.plugin"
    "/Library/Audio/MIDI Drivers/PloytecMidi.plugin"
    "/Library/Audio/MIDI Drivers/ploytecmidi.plugin"
    "/Library/Audio/MIDI Drivers/PloytecMIDI.driver"

    # --- DriverKit (Extension App) ---
    "/Applications/Ploytec Driver Extension.app"
)

# 5. Execute Delete
COUNT=0
for FILE in "${CRUFT_FILES[@]}"; do
    if [ -e "$FILE" ]; then
        sudo rm -rf "$FILE"
        echo "   [DELETED] $FILE"
        ((COUNT++))
    fi
done

if [ $COUNT -eq 0 ]; then
    echo "   (No legacy files found. System is clean.)"
else
    echo "   ✅ Removed $COUNT items."
fi

# 6. Flush Core Audio
echo ""
echo "🔄 Flushing Audio System..."
sudo killall coreaudiod
sudo killall MIDIServer 2>/dev/null || true
# Touching the directory forces the system to rescan plugins
sudo touch "/Library/Audio/Plug-Ins/HAL"
sudo touch "/Library/Audio/MIDI Drivers"

echo ""
echo "✅ Cleanup Complete. You are ready for a clean install."
read -p "Press [Enter] to exit..."