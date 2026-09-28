# CardPop!hax

*Homebrew on a stock Game Boy Color through the Pokémon TCG's infrared "Card
Pop!", driven by a lone ATtiny85 or a Raspberry Pi 3 — no flashcart, no link
cable, no mods.*

The *Pokémon Trading Card Game*'s "Card Pop!" IR feature exposes a remote
read/write/call of the console's memory. This project uses it to place a small
bootstrap in RAM, pull a resident loader over a custom IR link, show a menu,
stream the chosen payload and jump to it — with no flashcart and no link cable.

**Two supported launchers**, both driving the same IR board:

* **ATtiny85** — standalone, no PC at run time; a PC is only needed to build
  and flash the firmware.
* **Raspberry Pi 3** (B or B+) with the RT image — Raspberry Pi OS Lite
  (64-bit) and the official PREEMPT_RT kernel. `gbcpop` does the whole launch
  and also the Card Pop! commands ([chapter 11](docs/11-raspberry-pi.md)).

The same code also runs against a patched VisualBoyAdvance. That emulator path
(`gbcpop` + `vba-ir-patch/`) is a local test bench, used to catch errors before
a build goes onto real hardware, not a launch path.

Proven on real hardware (ATtiny85 + GBC, Raspberry Pi 3 B+ + GBC). The default
menu launches:
* **A**: a small game (snake)
* **B**: a Card Pop! gift: adds a Mew or Venusaur to the TCG's own save, as a
  Card Pop! would
* **START**: FIX94's [GameBoy Audio Dumper](https://github.com/FIX94/gameboy-audio-dumper),
  which dumps the inserted cartridge (or a *different* one, swapped in) to a
  PC over the audio jack

SELECT in a payload returns to the menu. An optional fourth payload, a save
patcher, adds Mew or Celebi to a swapped-in Red/Blue/Yellow/Gold/Silver/Crystal,
or enables Crystal's GS Ball event. All of them run on hardware.

## Just want to build one?

The whole write-up is chapters 01–11 below, but to build a working board:

1. **Build the board** — schematic, parts and assembly: [chapter 04](docs/04-attiny-hardware.md).
2. **Build and flash the firmware.** See [chapter 10](docs/10-tooling.md) for
   versions and the full sequence. Two paths:

   ```sh
   # A. Default menu (snake / card-pop / audio-dumper): uses the committed data
   #    as-is — the dumper's bytes are already baked into loader_data.inc, so
   #    there is nothing else to build and no gbcpop needed.
   make -C attiny flash                  # reassembles with avra; PORT=/dev/ttyACM0

   # B. Pick the payloads (regenerates the data + menu). Needs gbcpop — and its
   #    attiny-inc generator only exists in the SHM build:
   make -C host SHM=1
   #    If audio-dumper is in your list, build it once first: its .bin files are
   #    not committed and the attiny Makefile won't build them for you.
   git submodule update --init payloads/audio-dumper/upstream
   (cd payloads/audio-dumper && ./build.sh)
   make -C attiny PAYLOADS="snake audio-dumper save-patcher" flash
   ```

There is no prebuilt `.hex` in the repo: `make flash` always reassembles from
source with `avra`. Path A simply doesn't regenerate the payload data — the
committed `loader_data.inc` is used as-is.

**Or use a Raspberry Pi 3 instead of the ATtiny** (step 2): the RT image, the
build and the launch are in [chapter 11](docs/11-raspberry-pi.md).

## Layout

```
docs/    the write-up (read 01 first)
gb/      Game Boy side: bootstrap, loader, receive layer, test ROM (SM83, rgbds)
attiny/  the ATtiny85 launcher firmware (avra) + its generated data + sim harness
hardware/  the ATtiny85 IR board: schematic (PNG, SVG + Schemdraw source), photo
host/    PC build + test tools: gbcpop (generates the ATtiny data; drives the
         launch against VBA on the test bench; the launch and Card Pop! on a
         Raspberry Pi 3, set up by pi-setup.sh) + helpers
vba-ir-patch/  test bench: our IR files for VisualBoyAdvance 1.8.0
payloads/      snake (our game), save-patcher (Mew / Celebi / GS Ball event),
               card-pop (optional: Mew / Venusaur into the TCG save)
               + the audio-dumper packaging (our patch upstream)
LICENSE  GPL-2.0-or-later
```

## Documentation

| # | Chapter |
|---|---|
| [01](docs/01-overview.md) | Overview |
| [02](docs/02-tcg-cardpop-protocol.md) | The TCG "Card Pop!" IR protocol |
| [03](docs/03-vba-ir-bridge.md) | The VisualBoyAdvance IR bridge (the local test bench) |
| [04](docs/04-attiny-hardware.md) | ATtiny85 board (hardware): schematic, parts, assembly |
| [05](docs/05-loader-protocol.md) | The loader IR protocol (bootstrap → loader → menu → payload) |
| [06](docs/06-attiny-firmware.md) | ATtiny85 firmware (idle loop, stages, LEDs, debug build) |
| [07](docs/07-payloads.md) | Payloads (and the audio-dumper patch) |
| [08](docs/08-history-and-lessons.md) | History and lessons learned |
| [09](docs/09-roadmap.md) | Ideas and limits |
| [10](docs/10-tooling.md) | Tooling and third-party software |
| [11](docs/11-raspberry-pi.md) | The Raspberry Pi 3 launcher (RT image, build, launch) |

## License

GPL-2.0-or-later (`LICENSE`), matching VisualBoyAdvance, from which the IR patch
derives. The audio-dumper payload stays MIT (its own `LICENSE.md`).
