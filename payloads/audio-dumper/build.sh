#!/bin/sh
# Build the audio-dumper payload from upstream + our patch.
#   ./build.sh                       # uses the pinned upstream/ submodule
#   ./build.sh /path/to/gameboy-audio-dumper   # or an explicit checkout
# Upstream: https://github.com/FIX94/gameboy-audio-dumper (MIT, (C) 2018 FIX94),
# tested with commit 6188213249dbcfd8e6989554a75753e38a6116db.
# Produces c000.bin (the patched dumper code, RAM-resident at $C000) and copies
# the two prebuilt font blobs; manifest.txt ties the three segments together.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
UP=${1:-$HERE/upstream}
if [ ! -f "$UP/sender/sender.asm" ]; then
	echo "upstream not found at $UP" >&2
	echo "run: git submodule update --init payloads/audio-dumper/upstream" >&2
	exit 1
fi
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/sender"
cp "$UP/sender/sender.asm" "$UP/sender/charmap.asm" "$W/sender/"
( cd "$W" && patch -p1 < "$HERE/homebrew-launch.patch" )
cd "$W/sender"
rgbasm  -o dumper.o sender.asm
rgblink -o dumper.gb -m dumper.map dumper.o
n=$(grep '"dump"' dumper.map | sed -E 's/.*\(\$([0-9A-Fa-f]+) bytes.*/\1/')
dd if=dumper.gb bs=1 skip=$((0x150)) count=$((0x$n)) of="$HERE/c000.bin" status=none
cp "$UP/sender/standalone/8800.bin" "$HERE/8800.bin"
cp "$UP/sender/standalone/8E00.bin" "$HERE/8e00.bin"
echo "c000.bin = $((0x$n)) bytes, 8800.bin + 8e00.bin copied"
