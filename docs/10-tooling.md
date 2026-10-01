# 10. Tooling and third-party software

Everything here is standard, packaged software (Debian names given) plus one
program built from source (rgbds). Nothing in this list is vendored into the
repository; you install it yourself.

Reference environment: **Debian 13 (trixie)**, x86-64.

## Build and flash

| Tool | Version used | Debian package | Used for |
|---|---|---|---|
| rgbds (`rgbasm`, `rgblink`, `rgbfix`) | 1.0.1 | not in Debian — build from https://github.com/gbdev/rgbds | assemble the GB bootstrap, loader and the audio-dumper payload |
| avra | 1.4.2 | `avra` | assemble the ATtiny85 firmware; ships `tn85def.inc` |
| avrdude | 7.1 | `avrdude` | flash the ATtiny85 and read its EEPROM (debug build) |
| gcc | 14.2 | `build-essential` | build `gbcpop`: generates the ATtiny data; also the test-bench driver |
| GNU make | any | `make` | the Makefiles |
| Python | 3.13 (any 3.x) | `python3` | `decode_txlog.py`, `gen2save.py`, build scripts |

`rgbds` is not packaged on Debian; a recent release builds in a couple of
minutes. avra's `tn85def.inc` is found automatically, so the ATtiny build needs
no `-I` path.

## Test bench and simulation (optional)

Used to check changes on the PC before they are flashed; not needed to build
or run the ATtiny launcher.

| Tool | Version | Debian package | Used for |
|---|---|---|---|
| VisualBoyAdvance 1.8.0 + our IR patch | — | build from source; patch in `vba-ir-patch/` | run the GB side against `gbcpop` without hardware |
| SDL 1.2 | 1.2.x | `libsdl1.2-compat-dev` | VBA dependency |
| PyBoy | 2.7 | not in Debian — `pip install pyboy` in a venv | the payload tests (`test_patcher.py`, `test_card_pop.py`) |
| simavr + headers | 1.6 | `simavr`, `libsimavr-dev` | `attiny/sim/pb0_trace.c`: check ATtiny pin timing without hardware |
| Schemdraw | 0.23 | not in Debian — `pip install schemdraw` in a venv | `hardware/schematic.py` → `schematic.svg` |
| Inkscape | 1.4 | `inkscape` | export `hardware/schematic.svg` to PNG |

## Hardware (for the real launch)

* An **ATtiny85** (DIP-8), factory fuses (`lfuse 0x62 hfuse 0xdf efuse 0xff`,
  8 MHz internal RC oscillator — no external clock).
* An **stk500v2-compatible ISP programmer** on a serial port (this project used
  a "PROG-S2" on `/dev/ttyACM0`); adjust `PORT`/`PROG` in `attiny/Makefile`.
* The IR board itself — schematic, parts list and assembly notes in
  `04-attiny-hardware.md`.
* **Or**, instead of the ATtiny85 and the programmer: a **Raspberry Pi 3**
  (B or B+) with the RT image, plugged onto the board's J2 —
  `11-raspberry-pi.md`.

## Building it all

```sh
# Host tool: generates the ATtiny data (the SHM build; it is also the
# test-bench driver for VBA). The GPIO backends (plain `make`, PIGPIO=1,
# WIRINGPI=1) are for a Raspberry Pi; plain `make` on a Pi 3 also does the
# whole launch, see 11-raspberry-pi.md (host/pi-setup.sh).
cd host && make SHM=1

# GB bootstrap + loaders (loader.bin for the ATtiny, loader_pi.bin for the
# Raspberry Pi launcher) + headless test builds
cd ../gb && make all tests

# Audio-dumper payload (needs an upstream checkout, see 07-payloads.md)
cd ../payloads/audio-dumper && ./build.sh /path/to/gameboy-audio-dumper

# Snake payload (our own, in the repo)
cd ../snake && make snake.bin

# Card Pop! gift (Mew / Venusaur into the TCG save)
cd ../card-pop && make card-pop.bin

# ATtiny data (from the GB binaries + payloads) and firmware
cd ../../host && ./gbcpop attiny-inc ../attiny/loader_data.inc \
    ../gb/bootstrap.bin ../gb/loader.bin \
    ../payloads/snake/snake.bin ../payloads/card-pop/card-pop.bin \
    ../payloads/audio-dumper/manifest.txt
cd ../attiny && make && make flash        # PORT=/dev/ttyACM0 by default
```

`make DEBUG=1 flash` in `attiny/` builds the debug variant instead: EEPROM
trace (`make eeprom-dump`) and a bootstrap that colours the GBC screen per
chunk ([chapter 6](06-attiny-firmware.md)). It generates its own `loader_data_debug.inc`, so the
committed release data stays as it is.

Or let the ATtiny Makefile pick the payloads and regenerate
`loader_data.inc` itself ([chapter 7](07-payloads.md)):
`cd attiny && make PAYLOADS="snake card-pop audio-dumper" flash`.

## Third-party licenses

* **VisualBoyAdvance** — GPL-2.0-or-later. Our patch files (`vba-ir-patch/`) are
  under the same license.
* **GameBoy Audio Dumper** (FIX94) — MIT. Our patch keeps it MIT; see
  `payloads/audio-dumper/LICENSE.md`.
* This repository as a whole is GPL-2.0-or-later (`../LICENSE`).
