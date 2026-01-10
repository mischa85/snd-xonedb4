#!/bin/bash

# ==========================================
#  Ozzy Debug Logger
#  "The Black Box Logging Inspector"
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

TIMESTAMP=$(date "+%Y-%m-%d_%H-%M-%S")
LOGFILE="$HOME/Desktop/Ozzy_Debug_Log_$TIMESTAMP.txt"

PREDICATE='subsystem == "OzzyHAL" OR subsystem == "OzzyMIDI" OR subsystem == "PloytecUSB" OR subsystem == "Ozzy" OR sender == "sc.hackerman.ploytecdriver.dext" OR sender == "Ploytec Driver Extension"'

clear
echo "====================================================="
echo "      Ozzy Driver Debugger (History Dump)"
echo "====================================================="
echo ""
echo "This tool collects logs from the *PAST* 60 minutes."
echo "Use this after you have reproduced a bug or crash."
echo ""
echo "📝 Writing logs to: $LOGFILE"
echo ""

echo "⏳ Reading system log database (last 2h)..."
echo "   (This may take a few seconds)"

echo "-----------------------------------------------------" >> "$LOGFILE"
echo "HISTORY (Start - Last 2h)" >> "$LOGFILE"

log show --style compact --info --debug --last 2h --no-signpost --predicate "$PREDICATE" >> "$LOGFILE"

echo "HISTORY (End)" >> "$LOGFILE"
echo "-----------------------------------------------------" >> "$LOGFILE"

echo ""
echo "✅ Done."
echo "📄 Log saved to Desktop: $LOGFILE"
echo ""
read -p "Press [Enter] to exit..."