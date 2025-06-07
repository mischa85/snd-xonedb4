#!/bin/bash

wholeline="$(security find-identity -v | head -n 1)"
if [[ "$(echo ${wholeline} | cut -d " " -f 4 | tr -d ':')" == "Development" ]]; then
	CODE_SIGN_IDENTITY="$(echo ${wholeline} | cut -d " " -f 2)"
else
	echo "No developer ID found, cannot sign!!!!"
	echo "Please check if your system is configured with a valid Apple Developer ID!"
	exit 1
fi

set -e
set -x
security unlock-keychain -p '{}' /Users/${USER}/Library/Keychains/login.keychain

codesign --sign $CODE_SIGN_IDENTITY --entitlements mac-coreaudio/PloytecDriver/PloytecDriver.entitlements --options runtime --verbose --force build/Release/Ozzy.app/Contents/Library/SystemExtensions/sc.hackerman.ploytecdriver.dext
codesign --verify --verbose build/Release/Ozzy.app/Contents/Library/SystemExtensions/sc.hackerman.ploytecdriver.dext
codesign --sign $CODE_SIGN_IDENTITY --entitlements mac-coreaudio/Ozzy/Ozzy.entitlements --options runtime --verbose --force build/Release/Ozzy.app
codesign --verify --verbose build/Release/Ozzy.app
