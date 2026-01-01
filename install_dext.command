#!/bin/bash
set -e

# ==========================================
#  Ploytec Dext: Build, Sign & Install
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

# Visual Header
clear
echo "====================================================="
echo "      Ploytec DriverKit: Build & Install"
echo "====================================================="
echo ""

# --- 1. Identity Detection ---
echo "🔍 Finding Apple Developer Identity..."
IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Apple Development" | head -n 1)"

if [ -z "$IDENTITY_LINE" ]; then
    echo "⚠️  No 'Apple Development' certificate found. Trying 'Developer ID'..."
    IDENTITY_LINE="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -n 1)"
fi

if [ -z "$IDENTITY_LINE" ]; then
    echo "❌ Error: No valid signing identity found in Keychain."
    echo "   Please add your Apple ID in Xcode -> Settings -> Accounts."
    read -p "Press [Enter] to exit..."
    exit 1
fi

CODE_SIGN_IDENTITY=$(echo "$IDENTITY_LINE" | awk '{print $2}')
IDENTITY_NAME=$(echo "$IDENTITY_LINE" | cut -d '"' -f 2)
echo "✅ Identity: $IDENTITY_NAME"
echo ""

# --- 2. Build ---
echo "🏗️  Building Project..."
# Clean build to ensure freshness
rm -rf "$DIR/build/Release/Ploytec Driver Extension.app"

xcodebuild -project "mac-coreaudio/PloytecDriver.xcodeproj" \
           -scheme "PloytecDriver" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           build > /dev/null

xcodebuild -project "mac-coreaudio/PloytecDriver.xcodeproj" \
           -scheme "PloytecApp" \
           -configuration Release \
           SYMROOT="$DIR/build" \
           build > /dev/null

if [ $? -ne 0 ]; then
    echo "❌ Build Failed. Check logs."
    read -p "Press [Enter] to exit..."
    exit 1
fi

# --- 3. Sign ---
echo "🔐 Signing Binaries..."
APP_PATH="$DIR/build/Release/Ploytec Driver Extension.app"
DEXT_PATH="$APP_PATH/Contents/Library/SystemExtensions/sc.hackerman.ploytecdriver.dext"

codesign --sign "$CODE_SIGN_IDENTITY" --entitlements "mac-coreaudio/PloytecDriver/PloytecDriver.entitlements" --options runtime --force "$DEXT_PATH"
codesign --sign "$CODE_SIGN_IDENTITY" --entitlements "mac-coreaudio/PloytecApp/PloytecApp.entitlements" --options runtime --force "$APP_PATH"

# --- 4. Install ---
echo "🚀 Installing to /Applications..."
DEST_APP="/Applications/Ploytec Driver Extension.app"

if [ -d "$DEST_APP" ]; then rm -rf "$DEST_APP"; fi
cp -R "$APP_PATH" "/Applications/"

# Fix Quarantine
xattr -d -r com.apple.quarantine "$DEST_APP" 2>/dev/null

echo "✅ Installation Complete."
echo "👉 Launching App to activate extension..."
open "$DEST_APP"

echo ""
echo "NOTE: If this is a fresh install, allow the extension in System Settings."
read -p "Press [Enter] to exit..."