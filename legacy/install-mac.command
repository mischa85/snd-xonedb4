#!/bin/bash
set -e

# ==========================================
#  Ozzy Audio Engine: Build & Install
#  (Ozzy Daemon + HAL Plugin + MIDI Plugin)
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

# Conflict Check
if [ -d "/Applications/Ploytec Driver Extension.app" ]; then
    clear
    echo "====================================================="
    echo "❌ CONFLICT DETECTED"
    echo "====================================================="
    echo "You have the DriverKit App installed at:"
    echo "  /Applications/Ploytec Driver Extension.app"
    echo ""
    echo "You cannot have both. Please delete the app from Applications"
    echo "and reboot before installing the HAL driver."
    echo ""
    read -p "Press [Enter] to exit..."
    exit 1
fi

clear
echo "====================================================="
echo "      Ploytec Audio Engine: Build & Install"
echo "====================================================="
echo ""

# --- 1. Identity Detection ---
echo "🔍 Finding Apple Developer Identity..."
IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Apple Development" | head -n 1)"
if [ -z "$IDENTITY_LINE" ]; then
    IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -n 1)"
fi

if [ -n "$IDENTITY_LINE" ]; then
    CODE_SIGN_IDENTITY=$(echo "$IDENTITY_LINE" | awk '{print $2}')
    echo "✅ Signing with: $(echo "$IDENTITY_LINE" | cut -d '"' -f 2)"
else
    echo "❌ Error: No valid signing identity found."
    echo "   CoreAudio requires signed plugins."
    exit 1
fi
echo ""

# --- 2. Build Daemon (Ozzy) ---
echo "🔥 Compiling Ozzy Daemon..."
SRC_DAEMON="mac-hal/Ozzy/Ozzy.cpp"
SRC_DRIVER="mac-hal/Ozzy/PloytecUSB.cpp"
BIN_DAEMON="$DIR/build/Release/Ozzy"

mkdir -p "$DIR/build/Release"

# Standard build - NO KERNEL FLAGS here
clang++ -o "$BIN_DAEMON" "$SRC_DAEMON" "$SRC_DRIVER" \
    -framework CoreFoundation \
    -framework IOKit \
    -std=c++17 -O3 \
    -Wno-deprecated-declarations \
    -I"$DIR/mac-hal/Shared" 

if [ ! -f "$BIN_DAEMON" ]; then
    echo "❌ Daemon compilation failed."
    exit 1
fi

# --- 3. Build Audio & MIDI Plugins ---
echo "🏗️  Building OzzyHAL (HAL)..."
# We inject OTHER_LDFLAGS to link IOKit without modifying the xcodeproj file
xcodebuild -project "mac-hal/Ozzy.xcodeproj" \
           -scheme "OzzyHAL" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           OTHER_LDFLAGS="-framework IOKit -framework CoreFoundation" \
           build

echo "🏗️  Building OzzyMIDI (Plugin)..."
xcodebuild -project "mac-hal/Ozzy.xcodeproj" \
           -scheme "OzzyMIDI" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           build

# --- 4. Sign Artifacts ---
echo "🔐 Signing Binaries..."
HAL_PATH="$DIR/build/Release/OzzyHAL.driver"
MIDI_PATH="$DIR/build/Release/OzzyMIDI.plugin"

codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$HAL_PATH"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$MIDI_PATH"

# --- 5. Install (Sudo) ---
echo ""
echo "🔑 Administrator privileges required for installation..."
sudo -v

echo "🚀 Installing Plugins & Service..."

# 5a. Install Daemon (Ozzy)
echo "   - Installing Ozzy Service..."

sudo mkdir -p /usr/local/bin

# Copy binary
sudo cp "$BIN_DAEMON" /usr/local/bin/
sudo chown root:wheel /usr/local/bin/Ozzy
sudo chmod 755 /usr/local/bin/Ozzy

# Copy existing plist from source
sudo cp "$DIR/mac-hal/Ozzy/Ozzy.plist" /Library/LaunchDaemons/
sudo chown root:wheel /Library/LaunchDaemons/Ozzy.plist
sudo chmod 644 /Library/LaunchDaemons/Ozzy.plist

# 5b. Install HAL Driver
echo "   - Installing HAL Driver..."
if [ -d "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
fi
sudo cp -R "$HAL_PATH" "/Library/Audio/Plug-Ins/HAL/"
sudo chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"
sudo chmod -R 755 "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver"

# 5c. Install MIDI Driver
echo "   - Installing MIDI Driver..."
if [ -d "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin" ]; then
    sudo rm -rf "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
fi
sudo cp -R "$MIDI_PATH" "/Library/Audio/MIDI Drivers/"
sudo chown -R root:wheel "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"
sudo chmod -R 755 "/Library/Audio/MIDI Drivers/OzzyMIDI.plugin"

# --- 6. Load & Restart ---
echo "🔄 Reloading Services..."
sudo launchctl load /Library/LaunchDaemons/Ozzy.plist
sudo killall coreaudiod
sudo killall MIDIServer 2>/dev/null || true
sudo touch "/Library/Audio/MIDI Drivers"

echo ""
echo "✅ Build & Install Complete! Ozzy is running."
read -p "Press [Enter] to exit..."