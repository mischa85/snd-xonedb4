#!/bin/bash


# Use this to find your certificate identity:
#	security find-identity
#
readonly CODE_SIGN_IDENTITY=CHANGEME

set -e

set -x
security  unlock-keychain -p '{}' /Users/${USER}/Library/Keychains/login.keychain 


codesign --sign $CODE_SIGN_IDENTITY \
    --entitlements XoneDB4Driver/XoneDB4Driver.entitlements \
    --options runtime --verbose --force \
    build/Release/XoneDB4App.app/Contents/Library/SystemExtensions/sc.hackerman.xonedb4driver.dext

codesign --verify --verbose \
    build/Release/XoneDB4App.app/Contents/Library/SystemExtensions/sc.hackerman.xonedb4driver.dext

codesign --sign $CODE_SIGN_IDENTITY \
    --entitlements XoneDB4App/XoneDB4App.entitlements \
    --options runtime --verbose --force \
    build/Release/XoneDB4App.app

codesign \
    --verify --verbose \
    build/Release/XoneDB4App.app
