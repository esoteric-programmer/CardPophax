# 11. The Raspberry Pi 3 launcher

The second supported launcher, next to the ATtiny85. The IR board's J2 header
plugs onto a Raspberry Pi 3 ([chapter 4](04-attiny-hardware.md), *Raspberry Pi mode*), and `gbcpop`
drives it instead of the ATtiny: the whole launch (bootstrap → loader → menu →
payload, [chapter 5](05-loader-protocol.md)), plus the TCG's Card Pop! commands ([chapter 2](02-tcg-cardpop-protocol.md)): `probe`,
`pop`, `give`, `take`, `dump-save`, … Proven on a Raspberry Pi 3 Model B+.

**Supported: Raspberry Pi 3 (B or B+) with the RT image** — Raspberry Pi OS
Lite (64-bit) plus the official PREEMPT_RT kernel, as set up below. Other
models are not supported: the RT kernel is 64-bit only, `gbcpop`'s direct GPIO
backend sets the input pull-up the Pi 1–3 way (not the Pi 4's), and the Pi 5
has a different GPIO block.

## Image

**Raspberry Pi OS Lite (64-bit)**, from
<https://www.raspberrypi.com/software/operating-systems/> or through Raspberry
Pi Imager. Prepared with the 2026-09-15 release (Debian 13 "trixie"):
[`2026-09-15-raspios-trixie-arm64-lite.img.xz`](https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-09-15/2026-09-15-raspios-trixie-arm64-lite.img.xz),
SHA-256 `cdf4f3bfac35ae947b46e4e767f935453810549779ac3290e05a6754aee627e5`.
Newer 64-bit Lite releases work the same way; the 32-bit images do not.

There is no separate "RT image": the PREEMPT_RT kernel is an official package
(`linux-image-rpi-v8-rt`) that is installed on top in the next step. It
matters because `gbcpop` bit-bangs the IR line from userspace at `SCHED_FIFO`
priority. On a stock kernel an interrupt (network traffic is enough) can still
take the CPU in the middle of a byte, and the byte is lost.

## RT kernel and build

Once the Pi is installed and online:

```sh
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y git
git clone https://github.com/esoteric-programmer/cardpophax.git
cd cardpophax
host/pi-setup.sh
sudo reboot
```

`host/pi-setup.sh` does the following, which can also be done by hand:

1. `sudo apt install -y linux-image-rpi-v8-rt build-essential`. The kernel
   package installs itself as `/boot/firmware/kernel8_rt.img`.
2. It appends `kernel=kernel8_rt.img` (under `[all]`) to
   `/boot/firmware/config.txt`. Without that line the firmware keeps booting
   the stock kernel. `auto_initramfs=1`, set in the stock `config.txt`, makes
   the firmware load the matching `initramfs8_rt`.
3. It runs `make -C host`, which builds `host/gbcpop` with the direct
   `/dev/mem` backend. It needs no libraries. Only this backend has the
   loader's IR link; the `PIGPIO=1` and `WIRINGPI=1` builds speak only the
   Card Pop! stage.

After the reboot, `uname -v` should contain `PREEMPT_RT`. `apt full-upgrade`
keeps the RT kernel updated. To return to the stock kernel, remove the
`kernel=` line.

## Attach the board

Power off, **take the ATtiny85 out of its socket**, leave J1 unpowered and
plug J2 onto the Pi's header ([chapter 4](04-attiny-hardware.md)). The defaults match the board's
wiring: GPIO17 (pin 11) drives the IR LED, and GPIO18 (pin 12) reads the
detector, with the Pi's internal pull-up.

`gbcpop` needs root, for `/dev/mem` and the real-time priority. Nothing else
may use GPIO17/18 while it runs.

## Launch the payloads

The launch sends the same files the ATtiny has in its flash: the bootstrap,
the loader and the payloads. They are Game Boy code and not in the
repository; build them with rgbds as in [chapter 10](10-tooling.md) (rgbds is not packaged
in Debian), on the Pi or on a PC, and copy them to the same paths in the
Pi's checkout. The audio dumper also needs its upstream checkout ([chapter 7](07-payloads.md)).

Put the TCG on its **Card Pop!** screen, **don't press A**, and hold the GBC's
IR port a few centimetres from the board. Then:

```sh
cd cardpophax
sudo host/gbcpop -v loader gb/bootstrap.bin gb/loader.bin \
    payloads/snake/snake.bin payloads/card-pop/card-pop.bin \
    payloads/audio-dumper/manifest.txt
```

It waits up to ~20 s for the Game Boy, uploads the bootstrap (the screen
turns teal), and the menu appears a few seconds later. **A**, **B** and
**START** pick the first, second and third payload given on the command line.
**SELECT** in a payload returns to the menu for the next one. `gbcpop` exits
after 60 s without a request. `-v` prints the stages, retries and link
statistics at the end.

## Card Pop! commands

```sh
cd cardpophax/host
sudo ./gbcpop selftest      # LED + detector check, then listens 3 s for a Game Boy
sudo ./gbcpop probe         # handshake: prints the game's mode and player name
sudo ./gbcpop pop           # a Card Pop! (--want <id> forces a card; ids: ./gbcpop cards)
sudo ./gbcpop --help        # everything else (give, take, dump-save, ...)
```

For `probe` and `pop`, the Game Boy waits on its Card Pop! screen as for the
launch. `probe` only reads, so the game then reports the Pop! as unsuccessful.
That is expected.

If nothing gets through, `sudo ./gbcpop trace` records the IR pulses for 3 s
while you press A in Card Pop!. Its report tells a detector problem ("no light seen at
all") from bad timing (check `uname -v` for `PREEMPT_RT`).
