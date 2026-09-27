# save-patcher — Mew, Celebi and the GS Ball event for Gen 1 / Gen 2 saves

A WRAM-resident payload that patches the save of a cartridge you swap in:
Pokémon Red / Blue / Yellow, Gold / Silver or Crystal, Western releases
(English, German, French, Italian, Spanish).

## Use

1. Launch it from the loader menu (`SAVE PATCHER`). It asks you to insert a
   game.
2. Swap the TCG cartridge for a Pokémon game and press **START**. The patcher
   identifies the cartridge and shows only the buttons that apply:

   | Button | Action | Games |
   |---|---|---|
   | **A** | add **Mew** to the party | R/B/Y, G/S, C |
   | **B** | add **Celebi** to the party | G/S, C |
   | **START** | enable the **GS Ball / Celebi event** | C |
   | **SELECT** | back to the "insert game" screen without patching | all |

   SELECT on the "insert game" screen itself returns to the loader's menu
   (payload build only; not shown on screen).

3. The result stays on screen for about 2.5 s, then the "insert game" screen
   comes back so you can patch the next cartridge, or the same one again.

| Screen | Meaning |
|---|---|
| `DONE.` (green) | written, checksum resealed, read back |
| `ALREADY DONE` (yellow) | the GS Ball event was already on; nothing written |
| `PARTY FULL` (yellow) | six Pokémon already; nothing written |
| `NOT SUPPORTED` (red) | a different, unseated or Japanese/Korean cartridge |
| `NO SAVE FOUND` (red) | no valid save (check value / checksum wrong) |
| `WRITE FAILED` (red) | the read-back did not match (SRAM or battery problem) |

When you press A/B/START, the cartridge is identified again. If it was swapped
while the menu was showing, you get `NOT SUPPORTED` rather than a write to the
wrong game.

## The Pokémon

Each one is stored in the payload byte for byte (party struct, OT name,
nickname). They are generated from real saves by `extract_mon.py` and committed:

| File | Pokémon | Source |
|---|---|---|
| `mew.inc` | Mew, Lv 5, OT YOSHIRB, ID 55702 (Toys "R" Us distribution) | Gen-1 party, slot 1 of the distribution save |
| `mew2.inc` | the same Mew as the **Time Capsule** converts it for Gen 2 | derived from the same save |
| `celebi.inc` | Celebi, Lv 5, OT JENS, ID 20322 (German distribution, Hamburger Dom) | Gen-2 box 14 slot 13 of the restored Silver save |

**Where they come from:**
* **Mew:** the **Toys "R" Us distribution, United States, 8–12 December 1999**
  ("Peel & Win" cards), from a save preserved from that event
  ([digiex](https://digiex.net/threads/pokemon-gen1-legit-yoshirb-mew-from-1999-toysrus-distribution-red-blue-yellow-download.14648/)).
  OT YOSHIRB, ID 55702, Lv 5, as stored in that save.
* **Celebi:** from the **German Celebi Tour 2001**, a Pokémon truck at six
  cities, 6–17 September 2001; this one from **Hamburg (10 September 2001)**
  ([PokéWiki](https://www.pokewiki.de/Celebi-Tour)). OT JENS, ID 20322, Lv 5,
  per its owner. The public sources don't list the distribution OT. It was
  levelled to 100 once and restored: stat experience zeroed, moves back in
  event order.
* **The GS Ball / Celebi event** can't be triggered on any Western Crystal
  cartridge (US/EU). It was a Japanese Mobile System distribution. Only the
  2018 3DS Virtual Console releases unlock it (after the Hall of Fame). On a
  cartridge, the START action is the only way to get it.

`mew2.inc` follows pokecrystal `engine/link/link.asm` (`Function2868a` /
`.ConvertToGen2`) exactly:
* species index → Pokédex number
* **held item from the Gen-1 catch rate**: 45 → **Bitter Berry**
  (`TimeCapsule_CatchRateItems`)
* current HP, status, moves, OT ID, experience, stat experience, DVs, PP,
  level, max HP / Attack / Defense / Speed copied unchanged
* Special Attack / Special Defense computed with the Gen-2 formula
* friendship 70, Pokérus and caught data 0

`celebi.inc` is the 32-byte box struct plus the party-only tail (status 0,
HP = max HP, stats from the Gen-2 formula: 25/16/16/15/15/15).

Regenerate them with `make mew.inc mew2.inc MEW_SAV=x.sav [MEW_SLOT=n]` or
`make celebi.inc CELEBI_SAV=x.sav CELEBI_BOX=b CELEBI_SLOT=s`.

## Adding to the party

One routine does this for every game. A per-game **Layout** block describes
where the save data is, and a per-Pokémon **Mon** block says what to add:

* The save must exist: a valid checksum, plus check value 99 at `$A008` on
  Gen 2 (a zeroed SRAM would otherwise pass the 16-bit sum).
* With `n` Pokémon in the party (`n` < 6), the new one goes to **slot `n+1`**.
  With 3 in the party, three presses fill slots 4, 5 and 6; the fourth press
  shows `PARTY FULL`. The same species may be added again, even if it is
  already in the party or the Pokédex.
* It writes the species plus a new `$FF` terminator, count `n+1`, the struct,
  OT name and nickname.
* It ORs the Pokédex owned + seen bits (bits that are already set stay set),
  reseals the checksum and reads it all back.

| | Gen 1 (R/B/Y) | Gold / Silver | Crystal |
|---|---|---|---|
| party (count, 6 species + `$FF`, structs, OT names, nicknames) | `$AF2C`, 44-byte structs | `$A88A`, 48-byte | `$A865`, 48-byte |
| Pokédex owned / seen | `$A5A3` / `$A5B6` | `$AA4C` / `$AA6C` | `$AA27` / `$AA47` |
| checksum | `$A598`–`$B522`, 8-bit complement at `$B523` | `$A009`–`$AD68`, 16-bit LE at `$AD69` | `$A009`–`$AB82`, 16-bit LE at `$AD0D` |

All of this is in SRAM bank 1 (pokered / pokegold / pokecrystal `sram.asm`; PKHeX
`SAV1Offsets.INT` / `SAV2Offsets`). Gen 2 also keeps a backup save. Only the main
copy is patched: when its checksum is valid, the game loads it and rewrites the
backup from it (`TryLoadSaveFile` in pokegold and pokecrystal).

## The GS Ball event (Crystal)

In Japan, Crystal's GS Ball event (Goldenrod Pokécenter → GS Ball → Kurt →
Ilex Forest shrine → Celebi) was a Mobile System distribution. The Western
carts contain it, gated by one byte outside the save checksum
(pokecrystal `sram.asm`, "SRAM Crystal Data"):

| | SRAM | `.sav` offset |
|---|---|---|
| `sMobileEventIndex` | bank 1 `$BE3C` | `0x3E3C` |
| `sMobileEventIndexBackup` | bank 1 `$BE44` | `0x3E44` |

Both copies are set to `$0B` (`MOBILE_EVENT_OBJECT_GS_BALL`). The address is
the same in every Western release: PKHeX's `SAV2.EnableGSBallMobileEvent()`
uses it for every non-Japanese Crystal. The main save must exist (check values
99/127).

In the game: walk into the Goldenrod City Pokémon Center and the receptionist
hands over the GS Ball.

## Cartridge check

The patcher checks all of these before it enables SRAM:

* header checksum (`$014D`) correct, destination `$014A` = 1 (not Japanese),
  32 KB SRAM
* **Crystal**: title `PM_CRYSTAL`, game code `BYT?`, CGB-only, MBC3+TIMER+RAM+BATTERY
* **Gold/Silver**: title `POKEMON_…`, game code `AA??`, MBC3+TIMER+RAM+BATTERY
* for both, the code's language letter must be one of `E D F I S`
* **Red/Blue/Yellow**: title `POKEMON …` (with a space) and MBC3+RAM+BATTERY
  (`$13`) or MBC5+RAM+BATTERY (`$1B`). The prefix is checked instead of the
  full title so that localised titles are accepted.

SRAM is enabled only while a patch is running. The cartridge is never accessed
while the patcher waits for a swap, and the APU stays off (see the audio-dumper
notes in chapter 7).

## Build and test

```sh
make save-patcher.bin   # the $C000 payload (1373 B) for `gbcpop attiny-inc`
make                    # save-patcher.gbc: standalone ROM (copies itself to WRAM)
make tests && ./test_patcher.py   # needs PyBoy (pip install pyboy)
```

The payload build has no font of its own. It draws with the menu font that the
loader left in VRAM (tiles `$2C`–`$5F`, glyph = ASCII code). `save-patcher.gbc`
and the test carts are built with `-DSTANDALONE`, which adds the same font.

It isn't in the default ATtiny set. With snake and the audio dumper
(`make PAYLOADS="snake audio-dumper save-patcher" flash` in `attiny/`), the
ATtiny image is 7798 of 8192 bytes.

`make tests` builds fake carts with Crystal (`BYTE`, `BYTD`, `BXTJ`),
Gold/Silver (`AAXD`, `AAUE`, `AAUJ`), Red, Yellow and Japanese-Red headers. The
patcher then checks and patches its own cartridge in an emulator, with no swap.

`test_patcher.py` runs 25 cases, and all pass. Each one:
* checks the menu,
* presses a button and compares every byte of SRAM afterwards with the
  expected result,
* checks the return to the prompt, and
* presses the button a second time.

The cases cover:
* every action on every game type
* party full, no save, bad checksum
* SELECT back
* a button that doesn't apply being ignored
* Japanese carts
* the 3 → 4 → 5 → 6 → full sequence

It was also run against real saves: the German Crystal save (with its party cut
to 4, Celebi and Mew land in slots 5 and 6) and the restored German Silver save
(Mew in slot 6). Both checksums were valid afterwards. A check with the
Toys "R" Us save also passed: with its party emptied and resealed, patching
rebuilds the original save's bank 1 byte for byte.

A patched save was also loaded in Pokémon Silver itself: pret/pokegold built
in PyBoy. With your restored German Silver save plus Celebi, the title,
Continue, overworld, party and summary all work (Celebi Lv5, 25/25 HP, correct
moves and PP).

**Status:** works on hardware (GBC + ATtiny, German Silver). **Back up your
saves first.** Swapping a cartridge into a running console is risky: the
inserted cart gets no reset, so its memory controller starts in an undefined
state, and stray writes while the contacts connect can damage the save. The
patcher can't detect damage outside the data it checks.

In one real run, the patcher reported "DONE.", but the first load crashed and
the second showed "Der Spielstand ist zerstört!". The save was restored from a
backup. Repeating the same patch then worked. The patched data itself is
verified in real Silver code (above), so the likely cause is the swap. The
likely mechanism is a power dip: inserting a cart draws an inrush current. With
weak batteries that dip resets the GBC (arbitrary reboots were seen), and a
smaller dip can glitch the CPU or the cart's memory controller without a reset.
Use fresh batteries or a mains adapter for swapping sessions, and insert
cartridges firmly in one motion, with clean contacts.
