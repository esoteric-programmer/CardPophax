# 7. Payloads

A payload is what the loader stages and jumps to ([chapter 5](05-loader-protocol.md), Stage 3). Two
forms:

* **a bare `.bin`** — loaded to `$C000` and entered there. `payloads/snake/`
  is this: a small Snake game we wrote, WRAM-resident at `$C000`. `make
  snake.bin` there produces the payload image; `make` alone gives a bootable
  standalone `snake.gbc` too. You can drop any other `$C000`-based `.bin` in as
  a payload the same way. `payloads/save-patcher/` (below) is another one.
* **a `manifest.txt`** — a multi-segment payload. Each `segment <dest> <file>`
  line names a blob and where it goes; `entry = <addr>` is the jump target;
  `flags` may request the LCD be off during apply (needed for VRAM segments).

`gbcpop attiny-inc` (and the `loader`/`loadertest` commands) accept either form.
The Raspberry Pi launcher also takes a whole directory of them and shows it as a
scrolling menu ([chapter 11](11-raspberry-pi.md)).

## The audio dumper

`payloads/audio-dumper/` packages FIX94's **GameBoy Audio Dumper** as a
three-segment payload: the dumper code at WRAM `$C000` (entry) plus two font
blobs in VRAM (`$8800`, `$8E00`). It dumps a cartridge's ROM/save out of the
headphone jack for a PC to record — and it is designed to survive a cartridge
**hot-swap**, which is exactly what the loader menu allows (swap the cart while
the menu is shown, then pick the dumper).

Upstream: https://github.com/FIX94/gameboy-audio-dumper (MIT, © 2018 FIX94),
tested against commit `6188213249dbcfd8e6989554a75753e38a6116db`.

### What our patch changes

`homebrew-launch.patch` (against `sender/sender.asm`) does three things:

1. **RAM-resident build.** Upstream builds a ROM; we wrap the code in a
   `SECTION "wrap", ROM0[$150]` / `LOAD "dump", WRAM0[$C000]` block so modern
   rgbds links it at `$C000` while it lives at ROM offset `$150`, which
   `build.sh` extracts. (Same trick the loader itself uses.)
2. **APU off during the swap.** Upstream switches the wave channel on at
   startup, at 100 %. On real hardware that made the GBC reset when a cartridge
   was pulled or inserted (a supply dip on the loaded speaker line). The patch
   keeps the APU off until the new cartridge has passed the header check
   (`audioOn` at `checkok`) and turns it off again after a dump. The recording
   itself is unaffected.
3. **SELECT returns to the loader's menu** at any prompt (`readButtons`),
   [chapter 5](05-loader-protocol.md) §7a. Not while it sends, and not shown on screen.

Everything else is upstream, unchanged.

### Building it

The upstream source is a pinned git submodule at
`payloads/audio-dumper/upstream`:

```sh
git submodule update --init payloads/audio-dumper/upstream
cd payloads/audio-dumper && ./build.sh
```

This produces `c000.bin` (the patched dumper, 940 B) and copies the two prebuilt
font blobs `8800.bin` / `8e00.bin`. `manifest.txt` ties the three together. Only
our patch, `build.sh` and `manifest.txt` are committed here; the upstream code is
referenced as a submodule, not copied in, so no MIT-licensed upstream source is
redistributed in this repository beyond the small patch context.

The patched dumper stays MIT (see `LICENSE.md` in that folder).

## The save patcher (Mew, Celebi, GS Ball event)

`payloads/save-patcher/` patches the save of a cartridge swapped in at its
prompt: Red/Blue/Yellow, Gold/Silver or Crystal, Western releases. After START
it identifies the cartridge and offers only the buttons that apply:

* **A**: add the Toys "R" Us **Mew** to the party. On Gold/Silver/Crystal it
  is the Mew as the Time Capsule converts it, holding a Bitter Berry.
* **B**: add the German (Hamburger Dom, OT JENS) **Celebi** (G/S/C).
* **START**: enable Crystal's **GS Ball / Celebi event**.
* **SELECT**: go back to the prompt without patching.

SELECT at the prompt itself returns to the loader's menu (not shown on screen).

The Pokémon goes to the first free party slot, its Pokédex bits are set and the
save checksum is resealed. The patcher refuses a full party or a missing save,
reads everything back, and returns to its prompt for the next cartridge. The
Pokémon are baked into the payload from real saves (`extract_mon.py`).
Details, the evidence for the addresses, and the PyBoy tests are in its
`README.md`.

It is a bare 1373-byte `.bin`. It borrows the loader's menu font from VRAM
instead of carrying its own.

**Back up saves before patching.** Hot-swapping can damage a save (the
inserted cart gets no reset, and the inrush current can dip the supply,
worst on weak batteries). This happened once on hardware; see the patcher's
`README.md`. Use fresh batteries or a mains adapter.

## The Card Pop! gift (optional)

`payloads/card-pop/` is in the default set (button B). Its one screen offers a
**Mew** (A) or a **Venusaur** (B); the button checks that the inserted
cartridge is a supported TCG (else "NOT SUPPORTED") and adds the card to its
save exactly as a successful Card Pop! records it:
* the pop counter,
* the partner's name in the 16-entry history,
* the card (in the backup bank and the main bank),
* the album progress.

The partner is always `ATTINY`; the two RNG bytes at the end of the name are
chosen so the name is new and leads into the game's rare-card branch, which
really gives Mew. A real Card Pop! can never give Venusaur (a parity bug); its
entry is the nearest possible, a name for that same branch. After the result
(~2.5 s) the menu is back; SELECT there returns to the loader's menu (not shown
on screen). It is a 907-byte `.bin`; add it with
`make PAYLOADS="… card-pop"`. Details and tests are in its `README.md`.

## Writing your own payload

A new payload needs no change to the loader or the firmware. It is a folder
`payloads/<name>/` with one of:

| Form | Contents | Use for |
|---|---|---|
| `<name>.bin` | the image, loaded and entered at `$C000` | most payloads |
| `Makefile` with a `<name>.bin` target | built by `make PAYLOADS=…` (below) | payloads kept as source; `payloads/snake/` is the template |
| `manifest.txt` + segment files | several segments (e.g. code at `$C000` plus VRAM data), `entry = …`, `flags = LCD_OFF` | payloads with their own graphics; see `payloads/audio-dumper/` |

Then `make PAYLOADS="snake <name>" flash` in `attiny/`, or pass the path to
`gbcpop attiny-inc` directly.

**Menu name.** It comes from the `.bin` file name (`my-game.bin` -> `MY GAME`) or,
for a `manifest.txt`, the folder name. It is upper-cased, and `-`/`_` become
spaces. The menu font covers A-Z, 0-9, space and `, - . / : ; < = > ? @ [ \ ] ^ _`;
other characters show as `?`. The line is cut at the screen edge: 16 characters
after `A: `/`B: `, 12 after `START: `.

**Size.** All segments together must fit the 4 KB staging buffer
(`$C000`-`$CFFF`, [chapter 5](05-loader-protocol.md)). ATtiny flash is the tighter limit: the firmware
without payloads needs about 4 KB of the 8 KB, so all payloads together get
about 4 KB, a little less after framing. avra refuses an image that does not
fit.

**State at entry** (after the Stage-3 trampoline, [chapter 5](05-loader-protocol.md)):
* IME = 0, CGB normal speed, LCD on with the TCG's LCDC, VBK = 0
* SVBK = 7 (WRAM bank 7 at `$D000`)
* SP still in the loader's stack, so set your own (the save patcher uses `ld sp, $D000`)
* the TCG cartridge still inserted, its save RAM locked

**Rules of thumb:**
* **Don't enable interrupts.** The vectors are in the cartridge ROM, which may
  be pulled. Wait for VBlank with `halt` and IME off (IE = VBlank, clear IF
  first), as snake and the save patcher do.
* **If the user swaps cartridges, turn the APU off** (`$FF26` = 0, see the
  audio-dumper patch). Don't touch the cartridge while waiting for the swap,
  and enable its save RAM only for as long as you need it.
* **The menu font is free to use.** It is still in VRAM, tiles `$2C`-`$5F`,
  glyph = ASCII code. The save patcher prints with it and carries no font.
* **Returning to the menu** is optional: leave WRAM bank 7 and HRAM
  `$FFE0`-`$FFF9` alone, then `ld a, 7` / `ldh [$FF70], a` / `jp $D003`
  ([chapter 5](05-loader-protocol.md) §7a). Ours do it on SELECT, in the payload build only: the
  standalone ROM (`-DSTANDALONE`) has no loader to return to.

**Testing without hardware.** Build the payload the way snake does: a ROM whose
startup code copies the payload to `$C000` and jumps there (an rgbds `LOAD`
block). Then it runs in any emulator, and the same Makefile extracts the
`.bin`. The save patcher goes further, with fake cartridge headers and PyBoy
tests.

## Choosing the payloads for the ATtiny

The menu has three buttons (A, B, START), so the ATtiny carries one to three
payloads. `PAYLOADS` picks them, in button order, and regenerates
`loader_data.inc` before the firmware is assembled:

```sh
cd attiny
make PAYLOADS="snake card-pop audio-dumper"   # the default; + flash
make PAYLOADS="save-patcher"                  # just the patcher
make                                              # the current loader_data.inc
```

Each `PAYLOADS=` build regenerates `loader_data.inc` from scratch, so any set
and order can follow any other; nothing has to be restored in between. Plain
`make` uses whatever `loader_data.inc` is there, which is the last one generated.
The committed one is the default (snake, card-pop, audio dumper);
`git checkout attiny/loader_data.inc` brings it back.

**The menu text always matches the build.** `gbcpop attiny-inc` writes the menu
into `loader_data.inc` from the same list:
* one line per payload, named after its file (`save-patcher.bin` ->
  `A: SAVE PATCHER`, `audio-dumper/manifest.txt` -> `B: AUDIO DUMPER`)
* only the buttons that are used
* a matching prompt: "PRESS A", "PRESS A OR B" or "PRESS A, B OR START"

So `make PAYLOADS="save-patcher"` shows just `A: SAVE PATCHER` and "PRESS A". The
firmware's payload count comes from the same file. A button without a payload
is ignored by the ATtiny; the loader then asks once more after about 0.5 s and
returns to its menu after about 1 s (the lost-choice recovery).

Each name is a folder in `payloads/`. Its `manifest.txt` is used if it has one,
otherwise `<name>.bin`, which is built there with `make <name>.bin` when the
folder has a Makefile. It needs the SHM build of `gbcpop` (`cd host && make
SHM=1`). The audio dumper needs its `build.sh` run once. ATtiny flash used, out
of 8192 bytes:

| PAYLOADS | flash |
|---|---|
| snake audio-dumper *(hardware-tested)* | 6304 |
| snake audio-dumper save-patcher *(hardware-tested)* | 7798 |
| audio-dumper save-patcher | 7310 |
| snake save-patcher | 5432 |
| snake save-patcher card-pop *(hardware-tested)* | 6464 |
| snake card-pop audio-dumper *(default, committed; hardware-tested)* | 7336 |
| audio-dumper save-patcher card-pop | 8340 — **does not fit** |

These are release builds. A `DEBUG=1` build ([chapter 6](06-attiny-firmware.md)) needs about 390 bytes
more; the default set uses 7726, and snake + audio dumper + save patcher just
fits (8188).
