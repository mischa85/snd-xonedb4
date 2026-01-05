#!/bin/bash
set -e

# ==========================================
#  Ploytec Audio Engine: Build & Install
#  (USB Daemon + HAL Plugin + MIDI Plugin)
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

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

# --- 2. Build USB Engine (Daemon) ---
echo "🔥 Compiling PloytecUSB Daemon..."
# Assuming source is in mac-hal/ploytecusb/
SRC_USB="mac-hal/ploytecusb/PloytecUSB.cpp"
BIN_USB="$DIR/build/Release/ploytecusb"

mkdir -p "$DIR/build/Release"

clang++ -o "$BIN_USB" "$SRC_USB" \
    -framework CoreFoundation \
    -framework IOKit \
    -std=c++17 -O3

if [ ! -f "$BIN_USB" ]; then
    echo "❌ USB Engine compilation failed."
    exit 1
fi

# --- 3. Build Audio & MIDI Plugins ---
echo "🏗️  Building PloytecAudio (HAL)..."
xcodebuild -project "mac-hal/ploytec.xcodeproj" \
           -scheme "PloytecAudio" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           build > /dev/null

echo "🏗️  Building PloytecMIDI (Plugin)..."
xcodebuild -project "mac-hal/ploytec.xcodeproj" \
           -scheme "PloytecMIDI" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           build > /dev/null

# --- 4. Sign Artifacts ---
echo "🔐 Signing Binaries..."
HAL_PATH="$DIR/build/Release/PloytecAudio.driver"
MIDI_PATH="$DIR/build/Release/PloytecMIDI.plugin"

codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$HAL_PATH"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$MIDI_PATH"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$BIN_USB"

# --- 5. Install (Sudo) ---
echo ""
echo "🔑 Administrator privileges required for installation..."
sudo -v

echo "🚀 Installing Plugins & Service..."

# 5a. Install USB Daemon
echo "   - Installing USB Service..."
sudo launchctl unload /Library/LaunchDaemons/hackerman.ploytecusb.plist 2>/dev/null || true
sudo cp "$BIN_USB" /usr/local/bin/
sudo chown root:wheel /usr/local/bin/ploytecusb
sudo chmod 755 /usr/local/bin/ploytecusb

# Using the existing plist from source
sudo cp "$DIR/mac-hal/ploytecusb/hackerman.ploytecusb.plist" /Library/LaunchDaemons/
sudo chown root:wheel /Library/LaunchDaemons/hackerman.ploytecusb.plist
sudo chmod 644 /Library/LaunchDaemons/hackerman.ploytecusb.plist

# 5b. Install HAL Driver
echo "   - Installing HAL Driver..."
if [ -d "/Library/Audio/Plug-Ins/HAL/PloytecAudio.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/PloytecAudio.driver"
fi
sudo cp -R "$HAL_PATH" "/Library/Audio/Plug-Ins/HAL/"
sudo chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/PloytecAudio.driver"
sudo chmod -R 755 "/Library/Audio/Plug-Ins/HAL/PloytecAudio.driver"

# 5c. Install MIDI Driver
echo "   - Installing MIDI Driver..."
if [ -d "/Library/Audio/MIDI Drivers/PloytecMIDI.plugin" ]; then
    sudo rm -rf "/Library/Audio/MIDI Drivers/PloytecMIDI.plugin"
fi
sudo cp -R "$MIDI_PATH" "/Library/Audio/MIDI Drivers/"
sudo chown -R root:wheel "/Library/Audio/MIDI Drivers/PloytecMIDI.plugin"
sudo chmod -R 755 "/Library/Audio/MIDI Drivers/PloytecMIDI.plugin"

# --- 6. Load & Restart ---
echo "🔄 Reloading Services..."
sudo launchctl load /Library/LaunchDaemons/hackerman.ploytecusb.plist
sudo killall coreaudiod
sudo killall MIDIServer 2>/dev/null || true
sudo touch "/Library/Audio/MIDI Drivers"

echo ""
echo "✅ Build & Install Complete! USB Engine is running."
read -p "Press [Enter] to exit..."
