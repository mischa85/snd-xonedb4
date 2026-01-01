#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

clear
echo "====================================================="
echo "      Ploytec DriverKit Uninstaller"
echo "====================================================="
echo ""

APP_NAME="Ploytec Driver Extension.app"
DEST_APP="/Applications/$APP_NAME"

echo "This will delete: $DEST_APP"
echo "System Extension will be unloaded by macOS automatically."
echo ""

if [ -d "$DEST_APP" ]; then
    echo "🗑️  Deleting App..."
    rm -rf "$DEST_APP"
    echo "✅ Deleted."
else
    echo "⚠️  App not found in /Applications."
fi

echo ""
echo "Note: If the driver persists, please Reboot."
echo ""
read -p "Press [Enter] to close..."