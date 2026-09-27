#!/bin/bash
# Headless VBA <-> gbcpop test runner: starts the patched VisualBoyAdvance with
# a ROM in the background (IR bridge on /dev/shm), runs one gbcpop command
# against it, then summarises the GB's IR transmissions from the VBA log.
#   ./run_cal.sh <rom> <gbcpop command...>
#   e.g. ./run_cal.sh ../gb/boottest.gb loadertest ../gb/loader_test.bin <payload>
# VBA = path to the patched emulator (default: VisualBoyAdvance on $PATH).
# Note: VBA ignores SIGTERM, hence kill -9.
set -u
cd "$(dirname "$0")"
VBA=${VBA:-VisualBoyAdvance}
LOG=${LOG:-/tmp/gbcpop_vba.log}
ROM="$1"; shift
rm -f /dev/shm/tcg_gb /dev/shm/tcg_pi
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  VBA_IR_LOCAL_SHM=/dev/shm/tcg_gb VBA_IR_PEER_SHM=/dev/shm/tcg_pi \
  "$VBA" "$ROM" >"$LOG" 2>&1 &
VBAPID=$!
sleep 1.5
timeout 60 ./gbcpop "$@"
echo "gbcpop rc=$?"
sleep 0.5
kill -9 $VBAPID 2>/dev/null
echo "edges: TX=$(grep -c ' TX ' "$LOG") RX=$(grep -c ' RX ' "$LOG")"
python3 decode_txlog.py "$LOG" | tail -1
