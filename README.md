# CardPop!hax

*Homebrew on a stock Game Boy Color through the Pokémon TCG's infrared "Card
Pop!", driven by a lone ATtiny85 — no flashcart, no link cable, no mods.*

The *Pokémon Trading Card Game*'s "Card Pop!" IR feature exposes a remote
read/write/call of the console's memory. This project uses it to place a small
bootstrap in RAM, pull a resident loader over a custom IR link, show a menu,
stream the chosen payload and jump to it — all from a standalone ATtiny85, with
no PC, no flashcart and no link cable once the board is built.

https://github.com/user-attachments/assets/29bf8663-e8c6-4c92-b796-cf61f9d05cbf

**The ATtiny85 board is the project.** A PC is only needed to build the
firmware and flash it. The same code also runs against a patched
VisualBoyAdvance. That emulator path (`gbcpop` + `vba-ir-patch/`) is a local
test bench, used to catch errors before a build goes onto the ATtiny, not a
second way to launch. Driving the IR board from a Raspberry Pi instead of the
ATtiny is optional and untested with the loader (future work, [chapter 9](docs/09-roadmap.md)).

Proven on real hardware (ATtiny85 + GBC). The default menu launches:
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

The whole write-up is chapters 01–10 below, but to build a working board:

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

## Layout

```
docs/    the write-up (read 01 first)
gb/      Game Boy side: bootstrap, loader, receive layer, test ROM (SM83, rgbds)
attiny/  the ATtiny85 launcher firmware (avra) + its generated data + sim harness
hardware/  the ATtiny85 IR board: schematic (PNG, SVG + Schemdraw source), photo
host/    PC build + test tools: gbcpop (generates the ATtiny data; drives the
         launch against VBA on the test bench) + helpers
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

## License

GPL-2.0-or-later (`LICENSE`), matching VisualBoyAdvance, from which the IR patch
derives. The audio-dumper payload stays MIT (its own `LICENSE.md`).
