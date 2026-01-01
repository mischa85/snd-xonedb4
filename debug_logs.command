#!/bin/bash

# ==========================================
#  Ploytec Debug Logger
#  "The Black Box Recorder"
# ==========================================

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$DIR"

# 1. Setup Log File on Desktop
TIMESTAMP=$(date "+%Y-%m-%d_%H-%M-%S")
LOGFILE="$HOME/Desktop/Ploytec_Debug_Log_$TIMESTAMP.txt"

# 2. Define the search filter (HAL Audio, HAL MIDI, DriverKit)
PREDICATE='subsystem == "hackerman.ploytechal" OR subsystem == "hackerman.ploytecmidi" OR sender == "sc.hackerman.ploytecdriver.dext" OR sender == "Ploytec Driver Extension"'

clear
echo "====================================================="
echo "      Ploytec Driver Debugger"
echo "====================================================="
echo ""
echo "📝 Writing logs to: $LOGFILE"
echo ""

# 3. Dump History (Last 60 Minutes)
echo "⏳ Retrieving log history (last 60 minutes)..."
echo "-----------------------------------------------------" | tee -a "$LOGFILE"
echo "HISTORY (Start)" | tee -a "$LOGFILE"

log show --style compact --info --debug --last 1h --predicate "$PREDICATE" | tee -a "$LOGFILE"

echo "HISTORY (End)" | tee -a "$LOGFILE"
echo "-----------------------------------------------------" | tee -a "$LOGFILE"
echo ""

# 4. Start Live Stream
echo "🔴 LIVE LOGGING STARTED"
echo "   Please reproduce your issue now."
echo "   Press [Ctrl+C] when finished to stop."
echo "-----------------------------------------------------" | tee -a "$LOGFILE"
echo "LIVE STREAM (Start)" | tee -a "$LOGFILE"

# 'tee -a' appends the live stream to the file while showing it on screen
log stream --style compact --info --debug --predicate "$PREDICATE" | tee -a "$LOGFILE"