#!/bin/sh
# Install the Raspberry Pi launcher as a service (docs/11-raspberry-pi.md), on
# the Pi, from a checkout of this repository where everything is built:
#   host/gbcpop                          -> /usr/local/bin/gbcpop
#   gb/bootstrap.bin, gb/loader_pi.bin   -> /usr/local/share/cardpop/
#   the built payloads in payloads/      -> /usr/local/share/cardpop/payloads/
#   host/gbcpop-launch.service           -> enabled and started
# Payloads already in the payload directory stay; ours are added or updated.
#
# Usage: host/pi-install-service.sh          (uses sudo)
set -eu
HOST=$(cd "$(dirname "$0")" && pwd)
REPO=$(dirname "$HOST")
BIN=${BIN:-/usr/local/bin}
SHARE=${SHARE:-/usr/local/share/cardpop}
UNITDIR=${UNITDIR:-/etc/systemd/system}
SUDO=${SUDO-sudo}

for f in host/gbcpop gb/bootstrap.bin gb/loader_pi.bin; do
    if [ ! -s "$REPO/$f" ]; then
        echo "error: $f is missing; build it first (docs/11-raspberry-pi.md)" >&2
        exit 1
    fi
done

$SUDO install -d "$BIN" "$SHARE/payloads" "$UNITDIR"
$SUDO install -m 755 "$HOST/gbcpop" "$BIN/gbcpop"
$SUDO install -m 644 "$REPO/gb/bootstrap.bin" "$REPO/gb/loader_pi.bin" "$SHARE/"

# The payloads as `gbcpop launch` finds them in a directory: payloads/<name>/
# with a manifest.txt (copied with its .bin files), or with a <name>.bin.
for d in "$REPO"/payloads/*/; do
    n=$(basename "$d")
    if [ -f "$d/manifest.txt" ]; then
        $SUDO install -d "$SHARE/payloads/$n"
        $SUDO install -m 644 "$d/manifest.txt" "$d"/*.bin "$SHARE/payloads/$n/"
        echo "payload $n (manifest)"
    elif [ -s "$d/$n.bin" ]; then
        $SUDO install -m 644 "$d/$n.bin" "$SHARE/payloads/"
        echo "payload $n.bin"
    else
        echo "skipped $n: not built"
    fi
done

$SUDO install -m 644 "$HOST/gbcpop-launch.service" "$UNITDIR/"
sync
$SUDO systemctl daemon-reload
$SUDO systemctl enable --now gbcpop-launch
echo
echo "gbcpop-launch is running and starts at boot. Log: journalctl -u gbcpop-launch -f"
