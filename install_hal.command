#!/bin/bash
set -e

# ==========================================
#  Ploytec HAL: Build, Sign & Install
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

clear
echo "====================================================="
echo "      Ploytec HAL & MIDI: Build & Install"
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
    echo "❌ Error: No valid signing identity found in Keychain."
    echo "   CoreAudio requires a signed plugin to load."
    echo "   Please add your Apple ID in Xcode -> Settings -> Accounts."
    read -p "Press [Enter] to exit..."
    exit 1
fi
echo ""

# --- 2. Build ---
echo "🏗️  Building HAL Driver..."
xcodebuild -project "mac-hal/ploytec.xcodeproj" \
           -scheme "ploytechal" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           build > /dev/null

echo "🏗️  Building MIDI Plugin..."
xcodebuild -project "mac-hal/ploytec.xcodeproj" \
           -scheme "ploytecmidi" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           build > /dev/null

# --- 3. Sign ---
echo "🔐 Signing Plugins..."
HAL_PATH="$DIR/build/Release/ploytechal.driver"
MIDI_PATH="$DIR/build/Release/ploytecmidi.plugin"

codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$HAL_PATH"
codesign --sign "$CODE_SIGN_IDENTITY" --force --options runtime "$MIDI_PATH"

# --- 4. Install (Requires Sudo) ---
echo ""
echo "🔑 Administrator privileges required to install to /Library/Audio..."
sudo -v

echo "🚀 Installing..."
# Clean old
if [ -d "/Library/Audio/Plug-Ins/HAL/ploytechal.driver" ]; then
    sudo rm -rf "/Library/Audio/Plug-Ins/HAL/ploytechal.driver"
fi
if [ -d "/Library/Audio/MIDI Drivers/ploytecmidi.plugin" ]; then
    sudo rm -rf "/Library/Audio/MIDI Drivers/ploytecmidi.plugin"
fi

# Copy
sudo cp -R "$HAL_PATH" "/Library/Audio/Plug-Ins/HAL/"
sudo cp -R "$MIDI_PATH" "/Library/Audio/MIDI Drivers/"

# Permissions & Quarantine
echo "🔧 Fixing permissions..."
sudo chown -R root:wheel "/Library/Audio/Plug-Ins/HAL/ploytechal.driver"
sudo chmod -R 755 "/Library/Audio/Plug-Ins/HAL/ploytechal.driver"
sudo xattr -d -r com.apple.quarantine "/Library/Audio/Plug-Ins/HAL/ploytechal.driver" 2>/dev/null

sudo chown -R root:wheel "/Library/Audio/MIDI Drivers/ploytecmidi.plugin"
sudo chmod -R 755 "/Library/Audio/MIDI Drivers/ploytecmidi.plugin"
sudo xattr -d -r com.apple.quarantine "/Library/Audio/MIDI Drivers/ploytecmidi.plugin" 2>/dev/null

# --- 5. Restart ---
echo "🔄 Restarting Audio Services..."
sudo killall coreaudiod
sudo killall com.apple.audio.Core-Audio-Driver-Service.helper 2>/dev/null || true
sudo killall MIDIServer 2>/dev/null || true
sudo touch "/Library/Audio/MIDI Drivers"

echo ""
echo "✅ Build & Install Complete!"
read -p "Press [Enter] to exit..."