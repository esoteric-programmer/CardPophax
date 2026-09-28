# Homebrew Loader IR protocol (stage‑2)

A robust, timing‑forgiving IR protocol used **after** a small loader is resident
in the Game Boy's RAM, to stream the loader, a menu and an arbitrary payload from
the host (the ATtiny85 launcher, or `gbcpop` on a Raspberry Pi or against VBA) to the Game Boy, and
the menu choice back.

It exists because the TCG's own Card Pop! protocol
([`02-tcg-cardpop-protocol.md`](02-tcg-cardpop-protocol.md)) is fixed by the game's ROM
and is timing‑tight (exact 440 T cells, aborts on one bad byte). Once *our* code
runs in RAM we own **both** ends of the wire, so we replace that bit layer with
one that is self‑clocking, chunked, acknowledged and resynchronising.

This document describes the protocol as implemented. How it got here, and the
designs that did not work, is in [chapter 8](08-history-and-lessons.md); ideas
for later are in [chapter 9](09-roadmap.md).

Code: GB side `gb/` (`irbit.asm` bit layer,
`recvstream.inc` stream receive, `bootstrap.asm`, `loader.asm`); host side
`host/gbcpop.c` (`build_frame`, `send_stream`, `attiny-inc`) and
`attiny/main-tcg-loader.asm`.

---

## 0. The four stages

```
 Stage 0   host --Card Pop! protocol--> GB     upload the BOOTSTRAP (<=512 B)
           (tight timing, retried per write)   into $C000 (the WRAM region free
                                                during serve); cmd4 jump.
 Stage 1   host --THIS PROTOCOL--> GB           stream 'L' the loader (into bank 7),
                                                'F' the menu font, 'M' the menu text;
                                                the loader draws the menu
           GB --> host                          REQ: the chosen payload id (1 byte)
 Stage 2   host --THIS PROTOCOL--> GB           stream 'N' the manifest, 'B' the body
 Stage 3   GB (loader)                          apply the segments, jp entry
 (return)  GB (payload)                         SELECT -> jp LOADER_RETURN: the
                                                menu again, next REQ (Stage 2)
```

Only **Stage 0** touches the fragile TCG timing, and it moves only the ~0.5 KB
bootstrap. Everything after is on this protocol. A payload may return to the
menu (§7a), so one launch can run several payloads in turn.

---

## 1. Design goals

1. **Self‑clocking.** No shared clock: the receiver times the gaps between
   marks, so RC‑oscillator error and op‑amp reshaping are harmless.
2. **Reliable.** Every chunk is checksummed and acknowledged; a bad or missing
   chunk is resent, and a lost ACK never duplicates or corrupts data.
3. **Forgiving, not tight.** Wide timing windows; a late byte costs a resend,
   never a corrupt session. Receivers may join a transmission at any point.
4. **Small on both ends.** The bootstrap must fit 512 B with everything it needs;
   the ATtiny85 host has 8 KB of flash including all data.

---

## 2. Layer 1 — bit encoding (mark/space, from Gen‑2 Mystery Gift)

Each bit is a **short mark (LED on) followed by a space; the space length is the
bit**. Bytes are sent MSB first, followed by a **trailing mark** that bounds the
last bit's space, then an idle gap (the *lead*) before the next byte.

| | host → GB (gbcpop / ATtiny) | GB → host (`ir_send_byte`) |
|---|---|---|
| mark | 50 µs / 48 µs | ~67 µs |
| space 0 / 1 | 64 / 224 µs | mark‑to‑mark ~190 / ~490 µs |
| lead before a byte | 800 µs | — |
| receiver split | GB: `RX_THRESH` 9 counts ≈ 137 µs | host: 240 µs (gbcpop on VBA), 336 µs (ATtiny, gbcpop on a Pi) |

* The GB receiver (`ir_recv_byte`) syncs on a byte by requiring `FRAME_IDLE`
  (26 polls, ~400 µs) of dark before the first mark, then measures 8 spaces;
  ~21 ms (`RX_SYNC_BUDGET`) without a byte is a timeout.
* A host→GB byte takes ~2.3 ms on average (lead ~35 %, marks ~19 %, spaces
  the rest; a `1` costs 160 µs more than a `0`): ~3.4 kbit/s on the wire,
  ~3.3 kbit/s including frame headers and ACKs. The GB→host direction only
  carries ACKs and the REQ, so its slower timing costs nothing.
* **How the values are sized.** The GB polls the port in fixed loops of
  ~15.3 µs (`.won`, `.sync`) and ~10.5 µs (`.woff`) at normal speed; every
  window is a whole number of those polls with margin on both sides:
  * **Mark** ≥ ~40 µs: the ATtiny's `wait` is not synchronised to the Timer0
    prescaler (±8 µs), and a mark must span at least one GB poll. Marks are only
    ~19 % of a byte, so there is little to gain here.
  * **Space 0 / 1** ≈ 4 / 15 polls around a threshold of 9: ~5 polls
    (~75 µs) either side, far more than RC‑oscillator drift. The GB must see
    at least one dark poll between two marks, so space 0 bounds how much the
    GB's receiver may stretch a mark (~50 µs).
  * **`FRAME_IDLE`** must exceed the longest space (~170 µs of margin).
  * **Lead**: between two bytes the GB also stores the byte, and may expand
    a whole RLE run (up to `RLE_MAX_RUN` = 32 bytes, ~0.2 ms, §8) before it
    starts counting `FRAME_IDLE`; trailing space + lead − that work must
    still exceed `FRAME_IDLE` (~220 µs of margin). **`MS_LEAD`,
    `FRAME_IDLE` and `RLE_MAX_RUN` are coupled**: shortening the lead means
    shortening the run cap too.
* All GB constants are loop counts at **CGB normal speed**: in the TCG, cmd 4
  runs inside `IR_Begin`'s single‑speed section, so our code runs at normal
  speed (a bootstrap for a game in double speed must switch down first).
* In VBA the host's µs are GB‑clock µs (`cT`); on the ATtiny and on a Pi
  (the BCM system timer) they are real µs.

---

## 3. Layer 2 — the chunk frame

Every stream is cut into chunks of at most **128 raw bytes**; each chunk is one
frame:

```
 $55 $55  START  ID  SEQ  len_lo len_hi  <len bytes>  CK_lo CK_hi
```

* `$55 $55` — preamble. **START only counts directly after a `$55`**: receivers
  join transmissions mid‑frame, and bodies contain `$3C`/`$3D` (a stray one once
  became a START with a random 20 KB length).
* `START` — `$3C` body stored as‑is, `$3D` body RLE‑compressed (§8).
* `ID` — stream ID: `L` loader, `F` font, `M` menu text, `N` manifest, `B` body.
* `SEQ` — bits 0–6 = chunk index, bit 7 = **last chunk** of the stream.
* `len` — wire length of the body (< 256); `CK` — 16‑bit sum of ID, SEQ and
  the wire body.
* The receiver checks, **before storing a byte**: preamble, START, the expected
  ID and SEQ, `len` < 256, and that the decoded chunk fits the destination
  buffer.

---

## 4. Layer 3 — ACK, repeats and linger

**Stop‑and‑wait per chunk.** The host sends a chunk frame, then stays silent and
listens (gbcpop 30 ms of GB time; ATtiny 1 ms + 16 × 2.05 ms). The GB, after a
chunk passes its checksum, waits until the line has been dark ~4 ms
(`ACK_IDLE`, i.e. the host stopped; gives up after ~17 ms) and sends **one ACK
byte = the chunk's `CK_lo`**. On the ACK the host moves to the next chunk; else it
resends the same chunk, up to **8 times** (`TRIES`), then aborts (during
Stages 0–1 the ATtiny restarts the round from Stage 0).

**Repeats.** If the host missed an ACK it resends that chunk. The GB recognises a
repeat of the **last accepted chunk** (`hLastId` + `hLastSeq`, which survive the
bootstrap → loader jump), checks it, **does not store it** (its destination may
already be in use — the loader runs where a repeated loader chunk would land)
and ACKs it again. Any other unexpected frame is ignored.

**Linger.** Before the GB stops receiving — after the menu's last chunk (it then
draws and waits for a button) and after the body's last chunk (it then jumps) —
`recv_linger` keeps re‑ACKing repeats until the line was quiet ~100 ms. So a
lost final ACK is repaired, and "no ACK after 8 tries" really means "not
received" (the host reports a failure; the GB has not jumped).

**REQ.** After drawing the menu the GB waits for A / B / Start (after all
buttons were released), waits for the line to be idle **~66 ms**
(`wait_link_idle`), and sends the payload id as one byte (mark/space). The
66 ms are longer than the ~33 ms gaps between the ATtiny's Card Pop! probes, so
the REQ always falls into the ATtiny's listen phase (chapter 6). During the
menu's last ACK windows the host also accepts a reply 1..n as that REQ (it
implies the GB has the menu).

**A lost REQ / a broken-off transfer.** REQ is not acknowledged. Instead the GB
receives the manifest and the body with a give-up timeout: if ~0.5 s
(`GIVEUP_TIMEOUTS` × ~21 ms of silence) pass without an ACKed chunk, it **sends
the REQ once more by itself**; if that fails too, it returns to the menu (still
on screen) and the user presses the button again. The count restarts with
every ACKed chunk and is longer than a host retrying one chunk 8 times, so the
GB never leaves a transfer that is still going. The hosts match it: after a
failed manifest/body they simply listen for the next REQ. Tested with
`GBCPOP_TEST_DROPREQ` (the host ignores the first REQ; the automatic resend
must bring the payload).

```
  host                                   GB
   |--- L chunk 0 ----------------------->|   (bootstrap)
   |<-------------------- ACK CK_lo ------|
   |--- L chunk 1 ... L chunk 7 (last) -->|   jp loader
   |--- F chunks, M chunk (last) -------->|   linger ~100 ms, draw menu
   |<------------------------ REQ id -----|   user pressed a button
   |--- N chunk (last) ------------------>|
   |--- B chunks ... B chunk (last) ----->|   linger ~100 ms, apply, jp entry
```

---

## 5. Streams and their contents

| ID | Stream | Receiver buffer (max) | Body |
|---|---|---|---|
| `L` | loader | `$D000`, 8 pages | the loader image (always raw: the bootstrap has no RLE) |
| `F` | menu font | `$D800`, 3 pages | `first_code, count, count × 8` 1bpp rows (§8) |
| `M` | menu text | `$DB00`, 2 pages (Pi: 3) | `{row, col, ASCII…, 0}*`, `$FF` (padded after `$FF` so its last ACK is never 1..3); Pi: then the payload list (§8) |
| `N` | manifest | `$DE00`, 1 page | §6 |
| `B` | payload body | `$C000`, 16 pages (4 KB) | the segments back to back |

---

## 6. Payload manifest (stream `N`)

A payload is not just "bytes to `$C000`": the audio dumper is three blobs
(WRAM `$C000`, VRAM `$8800`, VRAM `$8E00`) plus an entry. The manifest is a small
segment table (at most 6 segments):

```
 nseg(1)
 repeat nseg:  dest_lo dest_hi  len_lo len_hi        ; 4 bytes / segment
 entry_lo entry_hi                                    ; jp target
 flags(1)                                             ; bit0 = LCD off before apply
```

Stream `B` is the concatenation of the segment bodies in manifest order (≤ 4 KB,
the staging buffer). At Stage 3 the trampoline copies each slice to its `dest`
with the LCD off, then `jp entry`.

Example — **audio dumper** (`manifest.txt`): `$C000` 940 B (code, entry),
`$8800` 1024 B and `$8E00` 512 B (font tiles), flags `0x01`.
**Snake** is the degenerate case: `nseg=1`, dest `$C000`, entry `$C000`, flags 0.

---

## 7. Memory map — airtight against both the TCG and the payload

### The binding constraint: the safe upload window during Stage 0
While the TCG serves our `cmd3` writes it is **not idle between commands** —
`IR_End` re‑enables interrupts and runs a VBlank each transaction (Card Pop! spec
§1.2), so its **VBlank ISR is live**: OAM/DMA and palettes at `$CA00+`, the frame
counters/trampolines around `$CACD`, `wDefaultText` at `$C590`, the RPC packet at
`$CE8C‑$CE93`, and the **stack growing down from `$E000`** (`ld sp,$e000`) are all
in use. Verified against `src/wram.asm`: `$C600‑$CE8B` holds 237 engine variables
— **not** free.

The one region proven free is **`wTempCardCollection` `$C000‑$C1FF`** (512 B —
Snake uploaded there and ran). `$C000‑$C58F` (below `wDefaultText`) is very likely
free as well, since the duel variables there are unused by Card Pop!, but
`$C000‑$C1FF` is the certainty.

512 B is too small for the loader + menu font (~1.5–2 KB), so **Stage 0 uploads
only a tiny bootstrap** that fits it; the bootstrap pulls the full loader over
this (robust) protocol.

### Layout

| Region | Address | Holds | Live during |
|---|---|---|---|
| Bootstrap | `$C000‑$C1FF` (≤512 B, today 442 B; 493 B as `DEBUG` build) | bit layer + chunk RX (+ screen status in `DEBUG`) | Stage 0 |
| **Loader image** | **WRAM bank 7, `$D000‑$D7FF`** (SVBK=7; `LOADER_MAX`) | the resident loader (~1 KB) | Stages 1–2 |
| Menu buffers | bank 7, `$D800` font (3 pages), `$DB00` menu text (512 B; 768 B in `loader_pi.bin`) | decoded menu streams | Stage 1 |
| Manifest, stack | bank 7, `wManifest` `$DE00`; SP from `$DFFF` (bootstrap too, after SVBK=7) | | Stages 0–3 |
| Staging buffer | bank 0, `$C000‑$CFFF` (4 KB) | received payload body, pre‑apply | Stage 2 |
| Payload finals | per manifest (`$C000`, VRAM, …) | applied destinations | Stage 3 |
| Stage‑3 trampoline | HRAM `$FF80‑$FFDF` | LCD‑off · copy staging→finals · `jp entry` | Stage 3 |
| Loader vars | HRAM `$FFE0‑$FFEA` | choice, manifest walk, saved LCDC | Stages 1–3 |
| Receive state | HRAM `$FFEC‑$FFF9` (`hardware.inc`) | RLE, stream ID/SEQ, last accepted chunk, linger, bootstrap screen status | Stages 0–2 |

### Flow
1. **Stage 0** — `cmd3` the bootstrap into `$C000‑$C1FF` (128‑byte writes, each
   retried by the ATtiny if its ack is wrong); `cmd4` `jp $C000`.
2. Bootstrap: `di`; lock cart SRAM (`$00`→`$0000`); screen → teal; `SVBK=7`;
   `SP=$DFFF`; receive stream `L` (the loader) into `$D000`; `jp $D000`.
3. **Stages 1–2** run from bank 7: streams `F` and `M` (menu), the button, REQ,
   streams `N` (manifest) and `B` (body → staging buffer in bank 0, free now).
4. **Stage 3** — the HRAM trampoline turns the LCD off, sets VBK=0, copies each
   segment staging→dest, restores the TCG's LCDC, then `jp entry`. Because it
   runs from HRAM it may overwrite any WRAM bank — including the loader's — so
   a payload can use the whole of WRAM. Only a payload that leaves bank 7 alone
   can return to the menu (§7a).

### Why it cannot collide
* Stage 0 writes only `$C000‑$C1FF` — clear of the live VBlank‑ISR RAM (`$CA00+`),
  `wDefaultText` (`$C590`), the RPC packet (`$CE8C`), and the `$E000` stack.
* Post‑jump the loader sits in **bank 7**, which the TCG never used (its state and
  stack are in bank 1). Payloads live in bank 0 + VRAM — a different bank, so no
  overlap even at equal `$Dxxx` addresses.
* The only case where the loader and a payload want the same physical bank is
  reconciled by the HRAM trampoline, which relocates over the loader in Stage 3.

Interrupts stay **off** throughout (`IME=0`); the bootstrap and loader poll IR,
the joypad and `LY` (for vblank) — the ROM interrupt vectors belong to whatever
cart is inserted (it may be swapped while the menu is shown), so we never vector
through them. Cart SRAM is locked (`$00` → `$0000`) first thing in both.

---

## 7a. Returning to the menu

The loader stays in WRAM bank 7 while a payload runs, so a payload can hand
control back to it:

```
    ld a, 7           ; SVBK: WRAM bank 7, where the loader lives
    ldh [$FF70], a
    jp $D003          ; LOADER_RETURN (hardware.inc)
```

`$D000` is the bootstrap's entry, `$D003` the return entry; both are fixed
jumps at the start of the loader image. The contract for a returning payload:

* leave **WRAM bank 7** (`$D000‑$DFFF`: loader code, menu buffers, manifest)
  and **HRAM `$FFE0‑$FFF9`** (loader variables, the TCG's saved LCDC, receive
  state) alone. A stack left at the loader's `$DFFF` is fine as long as it stays
  small (it grows down towards the manifest at `$DE00`);
* be in CGB **normal speed** when jumping.

The loader then resets what a payload may have changed — `di`, its stack, save
RAM off, `IE`/`IF` = 0, sound off, the IR port, the last accepted chunk — and
redraws the menu from its buffers (`draw_menu` turns the LCD off, without
waiting for a vblank if it is off already, and resets VRAM attributes, the
palette, scroll and LCDC). From there it is Stage 2 again. The host needs no
notice: the ATtiny's idle loop hears the next REQ like any other (chapter 6).

Our payloads return on **SELECT**, which none of them shows on screen:

| Payload | SELECT returns |
|---|---|
| snake | any time |
| audio dumper | at any prompt (not while it is sending) |
| card-pop | on its menu |
| save patcher | on its start prompt; on its action menu SELECT stays "back to the prompt" |

Their standalone ROMs (`-DSTANDALONE`) have no loader to return to and ignore
SELECT there. Tested on hardware (2026‑09‑26, the default set) and on the test
bench with `GBCPOP_TEST_RETURN` (§10).

---

## 8. Stage 1 — the menu

Right after the loader, the host sends two more streams:

```
 font  (ID 'F'):  first_code(1)  count(1)  count x 8 rows (1bpp, 8x8)
 menu  (ID 'M'):  { row(1) col(1) ASCII... 0 }*  $FF          (20x18 visible cells)
```

* **ASCII-indexed tiles.** Glyph `c` goes into tile `c` ($8000 addressing), so
  menu text is plain ASCII. The loader zeroes tiles `$20-$5F` and fills the BG
  map with `$20`, so space and any unsent character are blank. Each 1bpp row is
  written to both bitplanes (colour 3).
* The host (`gbcpop`) ships a 5x7 font for `$2C-$5F` (`,-./0-9:;<=>?@A-Z[\]^_`,
  52 glyphs, 418 B raw) and builds the text from the payload list (names from
  the file / directory name).
* CGB reset done by `draw_menu`: LCD off in vblank, attribute map (VBK=1) -> 0,
  BG palette 0 -> white/light/dark/black, LCDC=$91 (BG, tiles $8000, map $9800,
  no window/sprites), SCX=SCY=0. The TCG's original LCDC is saved at loader entry
  and restored by the trampoline before `jp entry`.
* Then `read_choice` polls A / B / Start -> `REQ id` 1 / 2 / 3.

### The Raspberry Pi menu (`loader_pi.bin`)

The Raspberry Pi launcher uses the same loader assembled with `-D PI_MENU`
(`gb/loader_pi.bin`); the ATtiny's `loader.bin` is unchanged. Its menu stream
continues after the `$FF` with the payload list, and the menu buffer is 3
pages (`$DB00-$DDFF`, 768 B):

```
 menu  (ID 'M'):  { row(1) col(1) ASCII... 0 }*  $FF  count(1)  { ASCII... 0 } x count
```

* The static text (title, key hints) still comes from the host; the loader
  draws the list itself on rows 6, 8 and 10 (arrow in column 1, name from
  column 3, at most 17 characters) and `n/N` on row 13. Three names are
  visible: the chosen one in the middle, clamped so the first is on the top
  row and the last on the bottom row.
* UP/DOWN move the arrow (repeating when held); a move redraws the four rows
  from a buffer at the start of vblank, with the LCD on. **A** sends
  `REQ id` = list position + 1 (1..count), so the host pads the stream until
  its last ACK is not 1..count. The position (`wSel`, bank 7) survives a
  payload that returns to the menu (§7a).
* **SELECT** restarts the Game Boy: MBC, banks, IR port and sound as after
  power-on, then from an HRAM stub (the loader itself is in WRAM bank 7) SVBK
  to bank 1, the CGB boot register values (`A = $11`) and `jp $0100`.

### RLE (per chunk)

`START = $3D` instead of `$3C` marks an RLE chunk body; `len` and the checksum
cover the wire (compressed) bytes; each chunk is compressed on its own. Decoded
on the fly by `rle_put` (`recvstream.inc`), so font, menu and payloads get it
(the size-capped bootstrap is built with `NO_RLE`; the loader stream is always
raw). PackBits-like: control `$00-$7F`
= n+1 literals follow; `$80-$FF` = next byte repeated (n&$7F)+1 times. The host
uses RLE only when it is shorter, and caps runs at `RLE_MAX_RUN` = 32 because the
GB writes a whole run between two IR bytes, inside the `MS_LEAD` gap (§2).

### Bootstrap screen colour

The bootstrap turns the whole screen **teal** when it starts. The `DEBUG` build
(`make DEBUG=1`, which defines `SCREEN_STATUS` for the bootstrap only; the
loader never recolours the menu or later phases) also shows the link on the
whole screen, like the ATtiny's LEDs: **teal** = the last chunk passed its
checksum, **red** = a copy failed or nothing arrived for ~63 ms (copies missed
completely). All 8 BG palettes are set to one colour in vblank; the colour is
set *before* the chunk's ACK so the vblank wait falls into the host's listen
window, and only a change repaints.

---

## 9. Timing summary

| What | Value | Where |
|---|---|---|
| chunk | ≤ 128 raw bytes, ~0.3 s on the wire | `CHUNK_RAW` (gbcpop), `CHUNK_RAW_MAX` |
| host wait after cmd 4 | 20 ms (the bootstrap waits for vblank before it listens) | `do_loader` (ATtiny) |
| host listen window after a copy | 30 ms GB time (gbcpop); 1 + 32.8 ms (ATtiny, Timer0) | `ms_rx_byte_gbtime`, `ACK_WIN` |
| GB wait before an ACK | ~4 ms of dark, give up after ~17 ms | `ACK_IDLE`, `ACK_BUDGET` |
| copies per chunk | 8 | `TRIES` |
| linger after a last chunk | 5 × ~21 ms of quiet | `LINGER_TIMEOUTS` |
| wait before REQ | ~66 ms of idle line | `wait_link_idle` |
| REQ resends | 1, after ~0.5 s without the payload | `menu_loop` |
| bit timing | mark 48, space 64 / 224, lead 800 µs; GB split 9 polls, byte sync 26 polls | `MS_*`, `RX_THRESH`, `FRAME_IDLE` (§2) |
| RLE run cap | 32 (a run is written between two IR bytes) | `RLE_MAX_RUN` |

Stable on hardware (ATtiny85 + GBC, 2026‑09‑26). Wire time of Stages 1–2
with the default payloads: the menu ~3.9 s after the bootstrap starts, snake
~1.1 s after the button, the Card Pop! gift ~2.4 s, the audio dumper ~5.5 s.
In the EEPROM traces every chunk is ACKed on its first copy.

---

## 10. Test hooks (gbcpop, VBA)

`run_cal.sh ../gb/boottest.gb loadertest ../gb/loader_test.bin <manifest>` (in `host/`)
runs the chain headless (boottest.gb = the bootstrap at normal speed; the
test loader picks payload 1 and sends a `$AA` heartbeat before it applies).
Environment switches: `GBCPOP_TEST_MIDJOIN` (an unframed loader tail first),
`GBCPOP_TEST_REPEAT` (ignore every chunk's first ACK), `GBCPOP_TEST_BADLAST`
(corrupt the body's last chunk: must fail), `GBCPOP_TEST_STALL` (300 ms of
silence mid‑loader), `GBCPOP_TEST_DROPREQ` (ignore the first REQ: the loader
must send it again by itself), `GBCPOP_TEST_RETURN=n` (the first n launches
get a stub that returns to the menu at once, §7a: the loader must redraw and
ask again, n times), `GBCPOP_DEBUG_ACK` (log ACK windows).

---

## 11. Known limits

See [`roadmap.md`](09-roadmap.md) §3: no whole‑payload CRC; streams ≤ 16 KB (7‑bit SEQ) and the body ≤ 4 KB (staging buffer); of
gbcpop's GPIO backends only the direct one (the default build) has the
mark/space layer; pigpio and wiringPi speak only the Card Pop! stage.
