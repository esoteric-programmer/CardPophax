# 6. ATtiny85 firmware

`attiny/main-tcg-loader.asm` is the launcher: the whole launch, from an ATtiny85,
with no PC at run time. (`gbcpop loader` does the same from a Raspberry Pi 3,
[chapter 11](11-raspberry-pi.md), and against VBA as the test bench, [chapter 3](03-vba-ir-bridge.md).) Assembles
with `avra`: about 4 KB of code and bootstrap/loader/menu data, plus the payloads.
The default image (snake, card-pop, audio dumper on A, B, START) uses 7336 of the 8192
bytes of flash; [chapter 7](07-payloads.md) lists other payload sets.

Its data — the bootstrap, the loader, the menu and the payloads, all already
cut into wire frames — is precomputed on the PC into `attiny/loader_data.inc`
by `gbcpop attiny-inc` ([chapter 7](07-payloads.md)). The firmware never builds a frame at run
time; it streams the prepared bytes and reads the ACKs. That keeps the AVR code
small and guarantees the ATtiny and `gbcpop` send identical bytes.

## The idle loop and the stages

The ATtiny keeps **no state** about what the GBC is doing. Its idle loop, over
and over:

1. **Probe for the TCG** (~0.4 s): send the Card Pop! sync `$AA` up to 12
   times, ~33 ms apart. The TCG, sitting on its Card Pop! screen (do **not**
   press A there; the game is then a slave running `IR_ServeLoop`), answers
   `$33` → run Stages 0 and 1.
2. **Otherwise listen for a REQ** (~0.4 s, `REQ_LISTEN` × 33 ms): a byte
   1..n from the loader's menu → run Stage 2.

So it does not matter where the GBC is — on the TCG's screen, in the menu, or
in a payload that later returns to the menu (SELECT, [chapter 5](05-loader-protocol.md) §7a) —
nor whether the GBC was switched off and on, or the ATtiny reset. The loader
sends its REQ only after ~66 ms of dark, which the ~33 ms gaps of step 1 never
give, so the REQ always lands in step 2.

* **Stage 0 — Card Pop! RPC.** Write the bootstrap into WRAM `$C000` in
  128-byte chunks (command 3), then call it (command 4). This is the only part
  on the timing-tight TCG bit layer ([chapter 2](02-tcg-cardpop-protocol.md)). Once the TCG has answered, each
  chunk write is retried up to `WRITE_TRIES` times if its ack is wrong (the
  first write after power-up was seen to come back one bit off; see
  [chapter 8](08-history-and-lessons.md)).
* **Stage 1 — our link.** Wait 20 ms first: the bootstrap colours the screen
  in vblank (up to ~17 ms) before it listens, and would miss the loader's
  first copy. Then send the loader, the menu font and the menu text,
  each chunked and ACKed ([chapter 5](05-loader-protocol.md)). The bootstrap jumps into the loader, which
  draws the menu. Back to the idle loop (unless the REQ already came during
  the menu's last ACK windows).
* **Stage 2 — the payload.** On a REQ, send the chosen payload's manifest and
  body. The loader applies the segments and jumps. Back to the idle loop,
  whether it worked or not: if the REQ was lost or the transfer broke off, the
  loader sends the REQ once more after ~0.5 s by itself.

## LEDs

Two status LEDs (blue = PB2, red = PB4; the IR LED is PB0):

| State | LEDs |
|---|---|
| power-on | 1 blue blink |
| idle loop: probing for the TCG, listening for a REQ | off |
| sending, the GBC answers (Card Pop! sync, chunk ACKs) | blue |
| a chunk copy got no ACK | red, until the next ACK |
| menu shown, waiting for the button | off |
| payload sent | off |
| Stages 0–1 failed after the TCG had answered | 3 red blinks, then the idle loop |

Both LEDs are switched **off during every ACK window** — the GBC's IR receiver
picks them up (see [chapter 8](08-history-and-lessons.md)), and this is not visible to the eye.

## Release and debug builds

`make` builds the release firmware; `make DEBUG=1` adds the EEPROM trace below,
and its bootstrap recolours the GBC screen per chunk (teal = chunk OK, red =
copy failed; [chapter 5](05-loader-protocol.md) §8). The release bootstrap only turns the screen teal
once, when it starts. The debug build writes its data to
`loader_data_debug.inc` (not committed) with the default payloads, or with
`PAYLOADS=...`; the committed release data is left alone. With the default
payloads the debug image uses 7726 of 8192 bytes.

## EEPROM trace (debug build)

`avrdude -c stk500v2 -p t85 -P /dev/ttyACM0 -U eeprom:r:ee.bin:r` (or
`make eeprom-dump`). The trace starts at a round's first Card Pop! sync and
runs through Stages 0–2 (a payload that returns and is followed by another is
appended; the buffer wraps). The lower half (`$000`) holds the last round or
payload that **failed**; the upper half (`$100`) holds the trace as of the last
**payload sent**.

Each half: `[0]` PROGRESS (Stage-0 chunks written), `[1]` SUBSTEP
(10 bootstrap called, 20 loader+menu sent, 40 payload sent),
`[2]` TRACE_PTR, `[3]` TRACE_WRAPPED, then `[tag, value]` records:

| tag | meaning |
|---|---|
| `0xAC v` | a Stage-0 write ack (`v`); a wrong one is retried |
| `0x5F 00` | a Card Pop! sync failed during a round (the idle loop's probes are not logged) |
| `0xE0 id` | REQ byte received |
| `0xC1..0xC8 b` | reply heard after chunk copy 1..8 (`b = FF`: nothing heard) |
| `0xCF id` | stream `id` failed (no ACK after all copies) |

**Trace quirk:** a chunk whose ACK byte (the frame's `CK_lo`) happens to be
`$FF` is logged the same as "nothing heard".

## Fuses

Factory: `lfuse 0x62 hfuse 0xdf efuse 0xff` — the internal 8 MHz RC oscillator,
no external clock. The firmware assumes this. All timing constants (`MS_*`,
`RX_THRESH`, the Timer0/Timer1 windows) are counts at 8 MHz; they were tuned on
an ATtiny85 powered with 5 V from USB. The internal oscillator's speed varies
with supply voltage and from chip to chip, so another chip may need retuning —
or an external 8 MHz oscillator on PB3 with changed fuses ([chapter 4](04-attiny-hardware.md), *Clock*).

## Porting / retuning

The link constants are the values that worked on VBA and on the one hardware
setup used here. On different optics or distance, expect to retune `MS_MARK`,
`MS_SPACE0/1`, `MS_LEAD` and `RX_THRESH` ([chapter 5](05-loader-protocol.md) §9 lists them; §2 explains
how they are sized, and that `MS_LEAD`, the GB's `FRAME_IDLE` and `RLE_MAX_RUN`
must change together). The Card
Pop! cell size (`TCG_CELL`) may also need a sweep, as it did here — [chapter 8](08-history-and-lessons.md).
