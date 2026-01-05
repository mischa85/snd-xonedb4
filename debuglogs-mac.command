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
PREDICATE='subsystem == "hackerman.ploytecaudio" OR subsystem == "hackerman.ploytecmidi" OR subsystem == "hackerman.ploytecusb" OR sender == "sc.hackerman.ploytecdriver.dext" OR sender == "Ploytec Driver Extension"'

# --- CLEANUP HANDLER ---
# When user presses Ctrl+C, kill the background logger
cleanup() {
    echo ""
    echo "-----------------------------------------------------"
    echo "🛑 STOPPING LIVE LOGGING..."
    if [ -n "$LOG_PID" ]; then
        kill "$LOG_PID" 2>/dev/null
        wait "$LOG_PID" 2>/dev/null
    fi
    echo "✅ Done. Log saved to Desktop:"
    echo "   $LOGFILE"
    exit
}
trap cleanup INT EXIT

clear
echo "====================================================="
echo "      Ploytec Driver Debugger"
echo "====================================================="
echo ""
echo "📝 Writing logs to: $LOGFILE"
echo ""

# 3. Dump History (Last 60 Minutes)
echo "⏳ Retrieving log history (last 60 minutes)..."
echo "-----------------------------------------------------" >> "$LOGFILE"
echo "HISTORY (Start - Last 60m)" >> "$LOGFILE"

log show --style compact --info --debug --last 1h --no-signpost --predicate "$PREDICATE" >> "$LOGFILE"

echo "HISTORY (End)" >> "$LOGFILE"
echo "-----------------------------------------------------" >> "$LOGFILE"
echo "✅ History retrieved."
echo ""

# 4. Start Live Stream
echo "🔴 LIVE LOGGING STARTED"
echo "   Please reproduce your issue now."
echo "   Press [Ctrl+C] when finished to stop."
echo "-----------------------------------------------------" >> "$LOGFILE"
echo "LIVE STREAM (Start)" >> "$LOGFILE"

log stream --style compact --info --debug --no-signpost --predicate "$PREDICATE" >> "$LOGFILE" &
LOG_PID=$!

echo "   (Monitoring file...)"
echo "-----------------------------------------------------"
tail -n +1 -f "$LOGFILE"
