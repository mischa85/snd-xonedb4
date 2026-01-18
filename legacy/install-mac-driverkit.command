#!/bin/bash
set -e

# ==========================================
#  Ploytec Dext: Build, Sign & Install
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

# --- 1. Conflict Checks ---
if [ -d "/Library/Audio/Plug-Ins/HAL/OzzyHAL.driver" ]; then
    echo "❌ CONFLICT: Please delete /Library/Audio/Plug-Ins/HAL/OzzyHAL.driver and reboot."
    exit 1
fi

# --- 2. Identity Detection ---
echo "🔍 Finding Identity..."
IDENTITY_LINE="$(security find-identity -v -p codesigning | head -n 1)"

if [ -z "$IDENTITY_LINE" ]; then
    echo "❌ No valid signing identity found in Keychain."
    exit 1
fi

CODE_SIGN_IDENTITY=$(echo "$IDENTITY_LINE" | awk '{print $2}')
IDENTITY_NAME=$(echo "$IDENTITY_LINE" | cut -d '"' -f 2)
echo "✅ Using Identity: $IDENTITY_NAME"

# --- 3. Build ---
echo "🏗️  Building..."
rm -rf "$DIR/build"

# 🟢 FIX: Added '-project' to point to the correct subfolder
xcodebuild -project "mac-coreaudio/PloytecDriver.xcodeproj" \
           -configuration Release \
           SYMROOT="$DIR/build" > /dev/null

if [ $? -ne 0 ]; then
    echo "❌ Build Failed. Check if 'mac-coreaudio/PloytecDriver.xcodeproj' exists."
    exit 1
fi

# --- 4. Force Sign (Exact logic from your codesign.sh) ---
echo "🔐 Force Signing..."
APP_PATH="$DIR/build/Release/Ploytec Driver Extension.app"
DEXT_PATH="$APP_PATH/Contents/Library/SystemExtensions/sc.hackerman.ploytecdriver.dext"

codesign --sign "$CODE_SIGN_IDENTITY" \
         --entitlements "mac-coreaudio/PloytecDriver/PloytecDriver.entitlements" \
         --options runtime --verbose --force \
         "$DEXT_PATH"

codesign --sign "$CODE_SIGN_IDENTITY" \
         --entitlements "mac-coreaudio/PloytecApp/PloytecApp.entitlements" \
         --options runtime --verbose --force \
         "$APP_PATH"

# --- 5. Install ---
echo "🚀 Installing to /Applications..."
DEST_APP="/Applications/Ploytec Driver Extension.app"

if [ -d "$DEST_APP" ]; then rm -rf "$DEST_APP"; fi
cp -R "$APP_PATH" "/Applications/"

xattr -d -r com.apple.quarantine "$DEST_APP" 2>/dev/null || true

echo "✅ Installation Complete."
echo "👉 Launching..."
open "$DEST_APP"

echo "NOTE: Ensure SIP is disabled if using a Development Cert!"