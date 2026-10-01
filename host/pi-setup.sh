#!/bin/sh
# Set up a Raspberry Pi 3 for gbcpop (docs/11-raspberry-pi.md), on the Pi
# itself, from a checkout of this repository:
#   1. installs the official PREEMPT_RT kernel (linux-image-rpi-v8-rt) and
#      selects it in /boot/firmware/config.txt
#   2. installs the build tools and builds gbcpop (direct /dev/mem backend)
# Reboot afterwards to start the RT kernel.
#
# Usage: host/pi-setup.sh          (uses sudo for the apt and config.txt steps)
set -eu
HOST=$(cd "$(dirname "$0")" && pwd)
CONFIG=/boot/firmware/config.txt

if [ "$(uname -m)" != aarch64 ]; then
    echo "error: needs the 64-bit Raspberry Pi OS (uname -m is $(uname -m));" >&2
    echo "       the RT kernel is only packaged for arm64." >&2
    exit 1
fi
if ! grep -q "Raspberry Pi 3" /proc/device-tree/model 2>/dev/null; then
    echo "warning: not a Raspberry Pi 3 ($(tr -d '\0' < /proc/device-tree/model 2>/dev/null));" >&2
    echo "         gbcpop's direct GPIO backend is written for the Pi 3." >&2
fi

sudo apt-get update
sudo apt-get install -y linux-image-rpi-v8-rt build-essential git

# The kernel package copies itself to /boot/firmware/kernel8_rt.img; the
# firmware boots it only when config.txt names it.
if [ ! -s /boot/firmware/kernel8_rt.img ]; then
    echo "error: /boot/firmware/kernel8_rt.img missing after the install" >&2
    exit 1
fi
if ! grep -q '^kernel=kernel8_rt.img' "$CONFIG"; then
    printf '\n[all]\n# PREEMPT_RT kernel (gbcpop, docs/11-raspberry-pi.md)\nkernel=kernel8_rt.img\n' \
        | sudo tee -a "$CONFIG" >/dev/null
    echo "added kernel=kernel8_rt.img to $CONFIG"
fi

make -C "$HOST" gbcpop

echo
if uname -v | grep -q PREEMPT_RT; then
    echo "Done. Already running the RT kernel: $(uname -r)"
else
    echo "Done. Reboot to start the RT kernel:  sudo reboot"
fi
