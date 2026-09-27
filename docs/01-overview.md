# 1. Overview

This project runs homebrew code on a stock Game Boy Color, using a retail
*Pokémon Trading Card Game* cartridge, over the console's infrared port — driven
by a standalone ATtiny85 with an IR LED and receiver. No flash cartridge, no
link cable, no PC while it runs.

It works because the TCG's "Card Pop!" IR feature is built on a small
remote-procedure-call layer: two TCG cartridges exchange their Card Pop! data by
**reading, writing and calling** each other's memory ([chapter 2](02-tcg-cardpop-protocol.md)). Those RPCs
take the address as a parameter, so a cooperating peer — here, our ATtiny — can
use the same commands the game uses to place a small routine in the console's
work RAM and ask the game to run it. This is the game's own IR mechanism, used
as intended; the console and cartridge are unmodified and nothing is flashed.

## The stages

```
 ATtiny85                                   Game Boy Color (TCG on the Card Pop! screen)
 --------                                   ---------------------------------------------
 Stage 0   Card Pop! RPC: write  -------->  bootstrap (<=512 B) into WRAM $C000
           (the TCG's own layer)  call --->  jp $C000   -- our code runs from here
                                             screen turns teal
 Stage 1   our IR link: stream L -------->  bootstrap pulls the loader into WRAM bank 7
                         stream F,M ------>  loader draws a menu (font + text)
                         REQ <------------  the button the user pressed
 Stage 2   our IR link: stream N,B ------>  loader stages the chosen payload
 Stage 3                                     loader applies the segments, jp entry
                                             the payload runs
           (listening)             REQ <---  SELECT in the payload: back to the
                                             menu, the next button -> Stage 2
```

Only **Stage 0** uses the TCG's timing-tight bit layer, and only for the ~0.5 KB
bootstrap. Everything after runs on our own link ([chapter 5](05-loader-protocol.md)), which is chunked,
checksummed and acknowledged, so it tolerates a hand or distance change during a
transfer.

## Why two protocols

The TCG bit layer has almost no timing margin and aborts on a single bad byte
([chapter 2](02-tcg-cardpop-protocol.md)). That is acceptable for a few hundred bootstrap bytes with retries,
but not for kilobytes over a hand-aimed IR link. Once our bootstrap runs we own
both ends of the wire, so we replace the bit layer with a forgiving one
([chapter 5](05-loader-protocol.md)).

## What each part is

* **`gb/`** — the code that runs on the Game Boy: the Stage-0 bootstrap, the
  resident loader (menu, transfer, relocate-and-jump), and the shared IR receive
  layer. SM83 assembly, built with rgbds.
* **`attiny/`** + **`hardware/`** — the launcher itself: the ATtiny85
  firmware (all data precomputed into flash) and the IR board it runs on
  (chapters [4](04-attiny-hardware.md) and [6](06-attiny-firmware.md)). This is the main path; nothing else is needed at run time.
* **`host/`** — `gbcpop`, the PC-side tool. On the PC it generates the ATtiny's
  data (`attiny-inc`), and it is the **test bench**: it runs the same launch
  against a patched VisualBoyAdvance, which is where the protocol was
  developed, and where changes are checked before they are flashed.
* **`vba-ir-patch/`** — the files that give VisualBoyAdvance an IR "cable" for
  that test bench ([chapter 3](03-vba-ir-bridge.md)).
* **`payloads/`** — what gets launched: snake, the packaging of FIX94's audio
  dumper, the save patcher and the optional Card Pop! gift ([chapter 7](07-payloads.md)).

## Status

Proven end to end on real hardware (ATtiny85 + GBC), and runnable on the
patched emulator as a test bench:
a small game, the Card Pop! gift and the audio dumper launch (the default
menu), and so does the optional save patcher. The menu appears a few seconds
after power-on; a payload follows a second or few after the button. SELECT in
a payload returns to the menu, so several payloads can run in turn without a
restart; the ATtiny keeps no state and simply answers whichever side the GBC is
on ([chapter 6](06-attiny-firmware.md)).

`gbcpop` also has Raspberry Pi GPIO backends from the project's beginnings.
They only speak the Card Pop! stage, not the loader protocol, and are untested
with the current launcher. Driving the board from a Pi is optional future work
(chapters [4](04-attiny-hardware.md) and [9](09-roadmap.md)), not a supported path.

## Reading order

[Chapter 2](02-tcg-cardpop-protocol.md) (the TCG protocol) and [chapter 5](05-loader-protocol.md) (our link) are the core. [Chapter 3](03-vba-ir-bridge.md) is
the emulator test bench, [chapter 4](04-attiny-hardware.md) the ATtiny board, [chapter 6](06-attiny-firmware.md) the firmware, [chapter 7](07-payloads.md)
payloads. [Chapter 8](08-history-and-lessons.md) is the development history and the hardware lessons; [chapter 9](09-roadmap.md) is the roadmap; [chapter 10](10-tooling.md) is the tooling.
