# Card Pop! — the Pokémon TCG (GBC) infrared protocol

Complete reverse-engineered specification of the infrared link used by
*Pokémon Trading Card Game* (Game Boy Color) for **Card Pop!**, card transfer
and deck-configuration transfer — everything needed to re-implement either end
from scratch (e.g. on a Raspberry Pi with an IR LED and an IR photodiode).

---

## 0. Where the code is

The IR stack lives in **ROM bank 6**. All addresses below were recovered from
the European English ROM (`POKECARD`, cart code `AXQP`) and are given as
`bank:address`. The US build (`AXQE`) has the same code roughly `$6e` bytes
lower in the bank; locate it by byte signature rather than by address:

| Routine | Signature (first bytes) |
|---|---|
| `IR_SendBit`  | `38 13 36 C1 3E 05 18 00 3D 20 FD 36 C0 3E 0E 18 00 3D 20 FD C9` |
| `IR_SendByte` | `E5 21 56 FF D5 C5 47 37 CD ?? ?? B7 CD ?? ?? 0E 08 0E 08` |
| `IR_RecvByte` | `D5 C5 E5 06 00 21 56 FF CB 4E 28 0A 05 20 F9` |

Entry points (EU addresses):

| Address | Name used here | Purpose |
|---|---|---|
| `06:$56DF` | `IR_SendBit`        | one bit cell |
| `06:$56FD` | `IR_SendByte`       | start bit + 8 data bits |
| `06:$572B` | `IR_RecvByte`       | wait for start pulse, sample 8 bits |
| `06:$5725` | `IR_RecvByteOr0`    | `IR_RecvByte`, returns `0` on timeout |
| `06:$576B` | `IR_Abort`          | returns `a=$FF`, carry set |
| `06:$5770` | `IR_SyncAsSender`   | send `$AA` until `$33` comes back |
| `06:$5789` | `IR_SyncAsReceiver` | wait for `$AA`, answer `$33` |
| `06:$57A3` | `IR_SendCommand`    | sync + `"IR"` + 8-byte register packet |
| `06:$57B9` | `IR_RecvCommand`    | sync + expect `"IR"` + 8-byte packet |
| `06:$57D3` | `IR_SendBlock`      | `c` bytes from `hl` + checksum |
| `06:$57E8` | `IR_RecvBlock`      | `c` bytes to `hl` + checksum check |
| `06:$57FD` | `IR_Begin`          | `di`, single speed, `rP1=$10`, `rRP=$C0` |
| `06:$580A` | `IR_End`            | `rP1=$30`, VBlank sync, double speed, `ei` |
| `06:$5823` | `IR_Off`            | `rRP = $00` |
| `06:$5828` | `IR_ServeLoop`      | act as slave: execute RPCs until disconnect |
| `06:$5848` | `IR_Dispatch`       | RPC jump table dispatch (5 commands) |
| `06:$5888` | `IR_TryMaster`      | 4 attempts at `$AA`/`$33` |
| `06:$58AD` | `IR_TrySlave`       | wait for `$AA` (A/B abort) |
| `06:$58D0` | `IR_Disconnect`     | RPC command 0 |
| `06:$58E1` | `IR_RemoteRead`     | RPC command 2 (read peer memory) |
| `06:$58F4` | `IR_RemoteWrite`    | RPC command 3 (write peer memory) |
| `06:$5AC0` | `CP_InitBuffers`    | build the `mode/'P'/'K'/'1'` header |
| `06:$5B22` | `CP_ExchangeIdent`  | header + player-name exchange |
| `06:$5D1D` | `CP_Session`        | the whole Card Pop! exchange |
| `06:$5DB4` | `CP_CheckDuplicate` | "already popped with this friend?" |
| `06:$5DFD` | `CP_PickCard`       | **decides which card you get** |
| `06:$5E62` | `CP_BuildPool`      | build the candidate list for a rarity |
| `06:$5E9D` | `CP_SumXor`         | sum/xor of a 16-byte name |

---

## 1. Hardware layer

### 1.1 The GBC IR port (`rRP`, `$FF56`)

```
bit 7-6   data read enable   (%00 = disabled, %11 = enabled)
bit 1     read data          (0 = IR light is being received, 1 = idle)   [read only]
bit 0     write data         (0 = LED off, 1 = LED on)                    [write only]
```

The game only ever writes three values:

| Value | Meaning |
|---|---|
| `$C0` | receiver enabled, LED off — the idle state during a transfer |
| `$C1` | receiver enabled, LED **on** |
| `$00` | everything off (written by `IR_Off` when leaving the feature) |

**There is no carrier.** The GBC drives its IR LED with plain DC and its
receiver is a bare photodiode + comparator. A 38 kHz demodulating receiver
module (TSOP17xx / TSOP382xx) **will not work** for receiving from the Game
Boy: it needs a modulated burst and would output nothing for a 26 µs DC pulse.

### 1.2 CPU speed and interrupts

`IR_Begin` (`06:$57FD`) does:

```
di
call SwitchToCGBNormalSpeed      ; <- IR always runs at SINGLE speed
ld a, $10 : ldh [rP1], a         ; select the button keys (A/B/Select/Start)
ld a, $C0 : ldh [rRP], a         ; enable the IR receiver, LED off
```

`IR_End` (`06:$580A`) writes `rP1 = $30`, waits for the LCD to enter VBlank
(`STAT & 3 == 1`), switches back to double speed and re-enables interrupts.

Consequences for an external implementation:

* **All timings below are in T-cycles of the 4.194304 MHz single-speed clock**
  (1 T = 238.42 ns).
* Interrupts are off *inside* one transaction, so the Game Boy never stalls
  mid-byte — but **between** transactions it re-enables interrupts, waits for a
  VBlank and switches speed twice. Expect gaps of **up to ~20 ms** between
  consecutive RPC transactions.
* `rP1` is left selecting the buttons so the transfer loops can poll **B** as
  an abort key without touching the port.

---

## 2. Bit layer

### 2.1 Encoding

A bit cell is **440 T = 104.90 µs**. Encoding is *pulse-present = 0*:

| Bit | Line activity |
|---|---|
| `0` | LED **on** for the first **108 T ≈ 25.8 µs** of the cell, then off |
| `1` | LED off for the whole cell |

`IR_SendBit` is written so that both branches take exactly 384 T, which is what
makes the cell length independent of the data:

```
IR_SendBit:            ; carry = bit to send, hl = rRP
    jr   c, .one       ;  8 / 12
    ld   [hl], $C1     ; 12   <-- LED ON
    ld   a, $05        ;  8
    jr   @+2           ; 12
.d1 dec  a             ;  4     } 5 iterations = 76 T
    jr   nz, .d1       ; 12/8   }
    ld   [hl], $C0     ; 12   <-- LED OFF  (108 T after the ON write)
    ld   a, $0E        ;  8
    jr   @+2           ; 12
.d2 dec  a             ;  4     } 14 iterations = 220 T
    jr   nz, .d2       ; 12/8   }
    ret                ; 16          total = 384 T
.one
    ld   a, $15        ;  8
    jr   @+2           ; 12
.d3 dec  a             ;  4     } 21 iterations = 332 T
    jr   nz, .d3       ; 12/8   }
    nop                ;  4
    ret                ; 16          total = 384 T
```

### 2.2 Byte frame

`IR_SendByte` sends **one empty cell, one pulse cell (the start bit), then the
8 data bits LSB first**:

```
 cell:   S0      S1      b0      b1      b2      b3      b4      b5      b6      b7
        (idle) (pulse)
        |       |###    |###    |       |###    |       |       |###    |       |###
        +-------+-------+-------+-------+-------+-------+-------+-------+-------+-------
        |<-412->|<-440->|<-440->|                      ... 440 T each ...
        T                                                            example: $AA
```

* `S0` is 412 T of silence (not 440 — it is entered slightly earlier).
* `S1` is a normal `0` cell: the **start pulse**.
* `b0..b7` follow at exactly **440 T** intervals; `b7`'s cell is 436 T.
* Total from the start pulse to the end of `b7` = **8 × 440 = 3520 T ≈ 839 µs**.
* Whole call including prologue/epilogue ≈ **4516 T ≈ 1.08 ms**, i.e. about
  **870 bytes/s** gross.
* Between consecutive bytes of a block transfer the line is idle for
  **≈ 1132 T ≈ 270 µs** (measured from the last data cell's start to the next
  byte's start pulse).

After every byte `IR_SendByte` polls `rP1`; if **B** is held it returns
`a = $FF` with carry set and the whole transfer is aborted.

### 2.3 Reception

`IR_RecvByte`:

1. Poll `rRP` bit 1 in a 36 T loop, at most **256 times → 9216 T ≈ 2.198 ms**.
   No pulse in that window ⇒ return `a = $FF`, **carry set** (timeout).
2. On the first detected pulse: delay **252 T**, then sample 8 cells.
3. Each receive cell is **448 T** and is sampled **10 times**, at cell offsets
   `40, 64, 104, 144, 184, 224, 264, 304, 344, 384` T (i.e. every 40 T ≈ 9.5 µs
   over a 356 T window). If *any* sample sees light the bit is `0`, otherwise `1`.
4. Bits are shifted in LSB-first; returns the byte with carry clear.

Note the deliberate mismatch: the transmitter uses 440 T cells, the receiver
448 T cells. The receiver drifts ~8 T per bit (56 T over a byte) and still has
~100–200 T of margin at the last bit. **This is the tolerance budget you get.**

### 2.4 Timing budget for a foreign implementation

Solving the "pulse must fall inside the receiver's sampling window" constraint
for the worst bit (`b7`):

| Parameter | Nominal | Accepted range |
|---|---|---|
| Bit cell period | 104.90 µs (440 T) | **≈ 102.3 – 112.9 µs** |
| Pulse width (`0`) | 25.8 µs (108 T) | **≥ ~12 µs** (must cover one 9.5 µs sample), ≤ ~90 µs |
| Idle before start pulse | ≥ 98 µs | anything ≥ ~1 cell |
| Reply deadline after receiving a byte | ~160 µs typical | **< 2.19 ms** |

So: emit a ~25 µs pulse every 105 µs; jitter of ±5 µs per cell is harmless,
but do not let the *cumulative* period error exceed about −2.5 % / +7.5 %.

---

## 3. Link layer

### 3.1 Handshake

Every transaction starts with a two-byte ping-pong that also decides who talks
first:

```
sender   ->  $AA   (repeated until answered)
receiver ->  $33
```

* `IR_SyncAsSender` (`06:$5770`): loop { send `$AA`; delay; receive } until the
  answer is exactly `$33`. Aborts if **B** is held.
* `IR_SyncAsReceiver` (`06:$5789`): loop { receive } until `$AA` is seen, then
  send `$33`. Aborts if **B** is held.

`IR_TryMaster` (`06:$5888`) is the bounded variant: at most **4** attempts,
returns carry set on failure. `IR_TrySlave` (`06:$58AD`) waits for `$AA`
indefinitely but breaks out if **A** or **B** is pressed (returned in `a`:
bit 0 = A, bit 1 = B).

### 3.2 Command frame

`IR_SendCommand` (`06:$57A3`) = handshake, then the magic bytes
`$49 $52` (`"IR"`), then an 8-byte block with checksum.

`IR_RecvCommand` (`06:$57B9`) mirrors it and re-syncs if the magic does not
match, so a desynchronised link recovers on its own.

### 3.3 Block transfer and checksum

```
IR_SendBlock(hl = source, c = count):        ; count == 0 means 256
    b = 0
    for i in 0..c-1:  b += mem[hl+i];  send mem[hl+i]
    send (-b) & 0xFF                          ; two's complement checksum
```

```
IR_RecvBlock(hl = dest, c = count):
    b = 0
    for i in 0..c-1:  mem[hl+i] = recv();  b += that byte
    if ((recv() + b) & 0xFF) != 0:  abort
```

I.e. **the sum of the payload plus the trailing checksum byte is 0 mod 256.**
Any receive timeout or checksum mismatch aborts the whole session (there is no
retransmission at this level; the application layer retries the whole thing).

---

## 4. RPC layer

The 8-byte command block is literally a **snapshot of the master's CPU
registers**, packed by `06:$5922` and unpacked by `06:$593B` at `$CE8C`:

| Offset | `$CE8C` | Content |
|---|---|---|
| 0 | `$CE8C` | `F` |
| 1 | `$CE8D` | **`A` — the command number** |
| 2 | `$CE8E` | `L` |
| 3 | `$CE8F` | `H` |
| 4 | `$CE90` | `E` |
| 5 | `$CE91` | `D` |
| 6 | `$CE92` | `C` |
| 7 | `$CE93` | `B` |

`IR_ServeLoop` (`06:$5828`) is the slave side: receive a command block, reject
`A >= 5`, otherwise dispatch through the table at `06:$5853`.

| `A` | Handler | Meaning |
|---|---|---|
| 0 | `06:$585D` | **Disconnect** — leave `IR_ServeLoop`, return success |
| 1 | `06:$5863` | No-op (unused by the game) |
| 2 | `06:$5865` | **Read**: slave syncs as sender, then sends `C` bytes from `HL` |
| 3 | `06:$586F` | **Write**: slave receives `C` bytes into `DE`, then replies with one acknowledgement byte `(-checksum)` |
| 4 | `06:$587E` | **Call**: `jp hl` on the slave, then repack the registers into `$CE8C` (unused by the game) |

Master-side wrappers:

* `IR_RemoteRead` (`06:$58E1`) — `hl` = address **in the peer**, `de` = local
  destination, `c` = length (0 = 256). Sends command 2, then syncs as
  *receiver* and pulls the block.
* `IR_RemoteWrite` (`06:$58F4`) — `hl` = local source, `de` = address **in the
  peer**, `c` = length. Sends command 3, then pushes the block immediately (no
  second handshake) and waits for the 1-byte acknowledgement.
* `IR_Disconnect` (`06:$58D0`) — sets `$CE8D = 0` and sends the command block.

The game uses these three commands as its normal Card Pop! operation: each
cartridge reads the other's header and player name (command 2) and writes its
own name into the other (command 3), and the disconnect is command 0. Because
the address is a plain parameter of the command, a cooperating peer that speaks
this protocol can point the same read/write/call at any location — which is what
makes it a convenient entry point for our loader ([chapter 5](05-loader-protocol.md)), and also handy for
inspecting or patching memory while debugging.

Each wrapper calls `IR_Begin`/`IR_End` around itself, so the speed switch and
VBlank wait happen **per transaction** (see §1.2).

---

## 5. Session layer

`CP_InitBuffers` (`06:$5AC0`) prepares two WRAM areas:

```
$C5EA   session status : $FF = not finished, $00 = OK, $01 = mode/version mismatch
$C5EB   mode           : 1 = Card Pop!, 2 = card transfer, 3 = deck configuration
$C5EC   'P'  ($50)
$C5ED   'K'  ($4B)
$C5EE   '1'  ($31)   protocol version
$C5EF.. peer's copy of the 4 bytes above ($C5EF = peer mode, $C5F0 'P', $C5F1 'K', $C5F2 '1')
$C590   own player name   (16 bytes, copied from SRAM sPlayerName = $A010)
$C500   peer player name  (16 bytes, filled during the exchange)
$C5F3   Card Pop! duplicate flag ($00 = ok, $FF = already popped with this friend)
```

`CP_ExchangeIdent` (`06:$5B22`), run **by the master only**:

1. `IR_RemoteRead(peer $C5EB → local $C5EF, 4)` — grab the peer's header.
2. Check `$C5F0 == 'P'`, `$C5F1 == 'K'`.
3. Check `$C5EF == $C5EB` (same feature selected on both sides).
4. `IR_RemoteRead(peer $C590 → local $C500, 16)` — the peer's name.
5. `IR_RemoteWrite(local $C590 → peer $C500, 16)` — our name.
6. Caller then checks `$C5F2 == '1'` (version).

A mismatch makes the master write `$01` into the peer's `$C5EA` and disconnect;
both sides then report "wasn't successful".

---

## 6. Card Pop! session flow

`CP_Session` (`06:$5D1D`). Both consoles run identical code; who becomes master
is decided by who presses **A** first.

```
both:   copy SRAM $BB00..$BBFF (own pop history, 16 × 16 bytes) to WRAM $C000
        CP_InitBuffers(mode = 1)

loop:   IR_TrySlave()                     ; sit and listen for $AA
          success  -> SLAVE path
          B pressed-> quit ("Pop! wasn't successful")
          A pressed-> IR_TryMaster()      ; 4 attempts
                        fail -> loop
                        ok   -> MASTER path

MASTER: CP_ExchangeIdent()                            ; headers + names
        IR_RemoteRead (peer $C000 -> local $C200, 256); peer's pop history
        CP_CheckDuplicate()                           ; -> $C5F3
        IR_RemoteWrite(local $C5F3 -> peer $C5F3, 1)  ; tell the peer
        IR_RemoteWrite(local $C5EB -> peer $C5EA, 1)  ; status byte = $00 (OK)
        IR_Disconnect()
        IR_ServeLoop()                                ; now serve the peer's wind-down

SLAVE:  IR_ServeLoop()                                ; master drives everything
        check $C5EA == 0
        IR_Disconnect()

both:   if $C5F3 != 0 -> "You cannot Card Pop! with a friend you previously Popped! with."
        CP_PickCard()                                 ; §7
        AddCardToCollection(wLoadedCard1ID)
        SRAM: history[ sa005++ & $0F ] = peer name    ; ring buffer at $BB00
```

#### How long a partner stays blocked

The history at SRAM `$BB00` is a **16-entry ring of 16-byte names** with the
write index in `sa005` (`sa005++ & $0F`), and only a *successful* pop advances
it. There is no clock involved: a partner is blocked until **16 further
successful Card Pops with other people** have overwritten their slot. On a new
game the ring is reset (only byte 0 of each slot is actually cleared — the
rest is left as SRAM garbage).

The check is also **one-sided**: `CP_CheckDuplicate` runs on the master and
tests the peer's name against the *master's* history. Swapping who presses A
first does not help between two cartridges, because both record each other
after every pop — but anything that takes the master role can simply write
`$00` into the peer's `$C5F3` and the restriction is gone. The slave never
consults its own history at all.

Two quirks worth recording:

* The 256-byte history transfer is **functionally dead**. `CP_CheckDuplicate`
  compares the peer's name against *our own* history (first loop, `$C000`);
  the second loop, which compares the peer's history against our own name
  (`$C200` vs `$A010`), never tests the comparison result and throws it away.
  So the duplicate check is one-sided — only the **master's** history matters —
  yet 256 bytes (~280 ms) are still transferred every time.
* `IR_ServeLoop`'s `jr nz` after `IR_RecvCommand` is unreachable, because
  `IR_RecvBlock` always returns `Z` on success and jumps away on failure.

---

## 7. How the received card is computed

`CP_PickCard` (`06:$5DFD`). **The result is fully deterministic**: it depends
only on the two 16-byte player names, because the routine *seeds the global RNG
from them* before drawing.

### 7.1 Name digest

```
CP_SumXor(buf16) -> (sum, xor):
    sum = (b0 + b1 + ... + b15) mod 256
    xor =  b0 ^ b1 ^ ... ^ b15
```

Applied to your own name (SRAM `sPlayerName`, `$A010`) and to the peer's name
(`$C500`), then:

```
d = (xor_own - xor_peer) mod 256      -> wRNG1   (EU $CAC9, US $CACA)
e = (sum_own - sum_peer) mod 256      -> wRNG2   (EU $CACA, US $CACB)
0                                     -> wRNG3   (EU $CACB, US $CACC)
```

Because `d` and `e` are **differences**, the other console computes `-d` and
`-e`: the two players almost always get cards of *different* rarity from the
same meeting. That is by design.

### 7.2 Rarity selection

```
if e == $05:                     -> promo branch (see 7.4)
elif e <  $40:  rarity = STAR    (2), fanfare = MUSIC_MATCH_VICTORY ($18)
elif e <  $9A:  rarity = DIAMOND (1), fanfare = MUSIC_BOOSTER_PACK  ($1C)
else:           rarity = CIRCLE  (0), fanfare = MUSIC_BOOSTER_PACK  ($1C)
```

(The chosen music id is stashed at `$CEA7` and played on the result screen.)

Note `e = 0` (two names with the same byte sum) falls into the `< $40` branch,
so identical names always yield a **rare**.

### 7.3 Candidate pool and draw

`CP_BuildPool` (`06:$5E62`) walks card IDs `1, 2, 3, …` calling
`LoadCardDataToBuffer1_FromCardID` until it reports "out of range"
(`NUM_CARDS = 228`), and appends the ID to a list at `$C400` unless:

* `wLoadedCard1Type & $08` — i.e. `TYPE_ENERGY_*` ⇒ **energy cards excluded**
  (Trainer cards are `$10` and therefore **eligible**);
* `wLoadedCard1Rarity != rarity`;
* `(wLoadedCard1Set & $F0) == PROMOTIONAL ($40)` ⇒ **all promos excluded**.

Resulting pool sizes: **CIRCLE 68, DIAMOND 67, STAR 66** cards (of which
10 / 12 / 10 are Trainer cards). The 27 excluded cards are the 7 basic energies
and the 20 promotional cards.

The list is then shuffled in place (home `$11E4`, EU addressing) and the
**first entry is the card you get**:

```
for i in 0 .. n-1:
    j = Random(n)                    ; Random(n) = (n * next_rng_byte()) >> 8
    swap list[i], list[j]
card = list[0]
```

### 7.4 The Card Pop!-only cards

```
if e == $05:
    fanfare = MUSIC_MEDAL ($1D)
    card = VENUSAUR1 ($0A) if (d & 1) == 0 else MEW2 ($A1)
```

These two cards are in the `PROMOTIONAL | GB` set and are therefore excluded
from every normal pool — this branch is the **only** way to obtain them, which
is exactly why `GetCardAlbumProgress` (home `$1DA4`) special-cases
`VENUSAUR1` and `MEW2` when computing album completion.

Because `e` is a signed difference, only **one** of the two players can ever be
on the `e == $05` branch (the other sees `e = $FB`).

#### Venusaur Lv.64 can never be obtained

Bit 0 of a byte block's sum always equals bit 0 of its xor — the sum's least
significant bit carries nothing in, so it is exactly the xor of the low bits.
Both name fields obey it, so

```
d & 1 == (xor_own ^ xor_peer) & 1 == (sum_own ^ sum_peer) & 1 == e & 1
```

**`d` and `e` always share parity.** Half the (d, e) plane is therefore
unreachable by any pair of names — and the Venusaur branch needs `e = $05`
(odd) with `d` even. No two trainer names in existence produce it.

An exhaustive sweep of all 65,536 (d, e) pairs confirms it: 203 distinct cards
appear, 202 of them in parity-reachable cells. `VENUSAUR1` occupies 128 cells,
all of them unreachable; `MEW2` occupies 128 reachable ones. Card Pop!'s only
obtainable exclusive is **Mew Lv.15**.

Reachable outcomes are also far from uniform — the shuffle only ever reports
`list[0]`. Arbok Lv.27 takes 315 of the 32,768 reachable cells (0.96 %) while
Super Energy Removal takes 3 (0.009 %).

### 7.5 The RNG

`UpdateRNGSources` (home `$089B`), operating on the three bytes `wRNG1`,
`wRNG2`, `wRNG3`:

```
c0 = bit0( rol(wRNG2, 2) ^ wRNG1 )       ; note: XOR clears carry, so RRA just tests bit 0
d  = wRNG2 ^ wRNG1
e  = wRNG3 ^ wRNG1
c1 = bit7(e)
e  = ((e << 1) | c0) & 0xFF              ; rl e
d  = ((d << 1) | c1) & 0xFF              ; rl d
wRNG3 += 1
wRNG2  = d
wRNG1  = e
return d ^ e
```

`Random(n)` (home `$088F`) returns `(n * UpdateRNGSources()) >> 8`.

### 7.6 Reference implementation

```python
POOL = {  # card IDs by rarity, energy and promotional cards removed
 0: [0x08,0x0C,0x0D,0x0F,0x12,0x14,0x17,0x1A,0x1C,0x1F,0x21,0x23,0x26,0x28,0x2A,0x2C,
     0x2D,0x30,0x33,0x39,0x41,0x44,0x46,0x49,0x4D,0x4F,0x51,0x53,0x55,0x56,0x5C,0x60,
     0x61,0x69,0x6A,0x6D,0x77,0x79,0x7B,0x7D,0x80,0x83,0x84,0x89,0x8B,0x8E,0x92,0x94,
     0x99,0xA3,0xA7,0xA9,0xAE,0xAF,0xB1,0xB2,0xB5,0xBC,0xC5,0xCC,0xCF,0xD0,0xD2,0xD4,
     0xDB,0xDD,0xE3,0xE4],
 1: [0x09,0x0E,0x10,0x13,0x15,0x18,0x1B,0x1D,0x20,0x24,0x29,0x2B,0x31,0x36,0x38,0x3A,
     0x3B,0x3C,0x3D,0x42,0x45,0x47,0x4A,0x4B,0x4C,0x4E,0x50,0x52,0x54,0x57,0x5A,0x5D,
     0x72,0x78,0x7C,0x7E,0x81,0x82,0x85,0x86,0x8A,0x8F,0x93,0x95,0x97,0x9C,0xA8,0xAA,
     0xB3,0xB4,0xB6,0xB7,0xBA,0xBD,0xBF,0xC3,0xC6,0xCD,0xD3,0xD7,0xD8,0xD9,0xDE,0xDF,
     0xE0,0xE1,0xE2],
 2: [0x0B,0x11,0x16,0x19,0x1E,0x22,0x25,0x27,0x2E,0x2F,0x32,0x34,0x35,0x3E,0x3F,0x43,
     0x48,0x58,0x59,0x5B,0x5E,0x67,0x68,0x6B,0x6C,0x6E,0x6F,0x71,0x73,0x74,0x75,0x7A,
     0x7F,0x87,0x88,0x8C,0x8D,0x90,0x96,0x98,0x9A,0x9B,0x9D,0xA2,0xA4,0xA5,0xA6,0xAB,
     0xAC,0xB0,0xB8,0xB9,0xBB,0xBE,0xC0,0xC2,0xC4,0xC7,0xC9,0xCA,0xCB,0xD1,0xD5,0xD6,
     0xDA,0xDC],
}
VENUSAUR1, MEW2 = 0x0A, 0xA1

class RNG:
    def __init__(self, r1, r2, r3=0):
        self.r1, self.r2, self.r3 = r1 & 0xFF, r2 & 0xFF, r3 & 0xFF
    def next_byte(self):
        r1, r2, r3 = self.r1, self.r2, self.r3
        c0 = ((((r2 << 2) | (r2 >> 6)) & 0xFF) ^ r1) & 1
        d, e = r2 ^ r1, r3 ^ r1
        c1 = (e >> 7) & 1
        e = ((e << 1) | c0) & 0xFF
        d = ((d << 1) | c1) & 0xFF
        self.r3 = (r3 + 1) & 0xFF
        self.r2, self.r1 = d, e
        return d ^ e
    def random(self, n):
        return (n * self.next_byte()) >> 8

def sum_xor(name16):
    s = x = 0
    for b in name16:
        s = (s + b) & 0xFF
        x ^= b
    return s, x

def card_pop(own_name16, peer_name16):
    s_own, x_own = sum_xor(own_name16)
    s_oth, x_oth = sum_xor(peer_name16)
    d = (x_own - x_oth) & 0xFF
    e = (s_own - s_oth) & 0xFF
    rng = RNG(d, e, 0)
    if e == 0x05:
        return VENUSAUR1 if (d & 1) == 0 else MEW2
    rarity = 2 if e < 0x40 else (1 if e < 0x9A else 0)
    lst = list(POOL[rarity]); n = len(lst)
    for i in range(n):
        j = rng.random(n)
        lst[i], lst[j] = lst[j], lst[i]
    return lst[0]

def name(s):                       # sPlayerName: ASCII, NUL-terminated & padded
    b = s.encode('ascii')[:15]
    return b + b'\x00' * (16 - len(b))
```

Worked example — `"MARK"` meets `"JOHN"`:

```
MARK: sum=$2B xor=$15      JOHN: sum=$2F xor=$03
MARK's side: d = $15-$03 = $12,  e = $2B-$2F = $FC  -> $FC >= $9A -> CIRCLE -> card $E4 (Recycle)
JOHN's side: d = $03-$15 = $EE,  e = $2F-$2B = $04  -> $04 <  $40 -> STAR   -> card $8C
```

---

## 8. Building the other end (Raspberry Pi)

### 8.1 Optics and analogue front end

**Transmit.** A 940 nm IR LED (e.g. TSAL6200/SFH4545) driven from a GPIO
through an NPN transistor or a small MOSFET, with a series resistor for
~50–150 mA pulsed. Keep the pulse duty low — it is only ~25 % of the cell for
worst-case data, so a resistor sized for continuous 20–30 mA average is fine.
Aim the LED at the Game Boy's IR window from 2–10 cm; the GBC's receiver is
directional and not very sensitive.

**Receive.** You need an *unmodulated* light detector:

* a PIN photodiode (BPW34, SFH203FA, with a daylight filter if possible) into
  a transimpedance amplifier and a comparator, **or**
* a phototransistor with a load resistor into a Schmitt-trigger input
  (74HC14) — simplest, works fine at these speeds, and
* **not** a TSOP-style demodulator module.

The GBC's own LED pulses are ~26 µs, so the front end must settle well inside
~10 µs. Add an optical barrier or point the LED slightly away from your own
photodiode: your transmitter will otherwise blind your receiver (the Game Boy
has the same problem, which is why the protocol is strictly half-duplex with
long turnaround gaps).

Decide a convention and stick to it, e.g. GPIO **high = light present**
(invert whatever your comparator gives you).

### 8.2 GPIO timing

* Cell period **104.9 µs**, pulse **25 µs**. Both are far too tight for naive
  userspace `nanosleep` loops on a busy Linux.
* **Transmit:** use DMA-driven waveforms — `pigpio`'s
  `gpioWaveAddGeneric()` / `gpioWaveTxSend()` give 1 µs resolution and are
  immune to scheduler jitter. On a Pi 5 the RP1 PIO is an even better fit.
  Build the whole byte frame (10 cells) as one waveform and fire it in one go.
  Without DMA — a busy-wait loop against the BCM system timer at
  `PERI_BASE + $3000` — the clock is still exact but the loop is not; see the
  PREEMPT_RT note below.
* **Receive:** timestamp *edges*, do not poll. `pigpio`'s
  `gpioSetAlertFunc()` gives ~1–5 µs timestamps; `gpioSetWatchdog()` gives you
  the timeout. Then decode arithmetically:

  ```
  t0 = timestamp of the first rising edge (the start pulse)
  for each later rising edge at time t within the byte:
      k = round((t - t0) / 104.9 µs)          # 1..8
      bit[k-1] = 0
  every k not seen  ->  bit = 1
  ```

  This is much more robust than imitating the Game Boy's 10-samples-per-cell
  scheme, and it naturally absorbs the 440/448 T asymmetry.
* Give yourself a real-time scheduling class (`SCHED_FIFO`) and pin the
  process, or better, put the bit layer on a microcontroller / PIO block and
  talk bytes over SPI/UART.
* **A PREEMPT_RT kernel is not optional for a polling implementation.** The
  2.19 ms receive timeout is the only hard requirement the *protocol* states,
  but a 26 µs pulse has to be caught by a loop the kernel can preempt at any
  moment. On a stock Raspbian kernel ordinary network traffic is enough to
  lose bytes — that is measured on the Gen 2 Mystery Gift rig, not theoretical.
  Reading the clock is immune (the BCM system timer is one register load); the
  loop around it is not. DMA-driven transmission sidesteps the problem on the
  send side only.

### 8.3 What to implement, in order

1. `send_byte(b)` / `recv_byte(timeout=2.19 ms)` per §2.
2. `sync_as_sender()` / `sync_as_receiver()` per §3.1.
3. `send_block(bytes)` / `recv_block(n)` with the two's-complement checksum
   per §3.3.
4. `send_command(A, HL, DE, BC)` — handshake, `"IR"`, the 8-byte packet
   `F, A, L, H, E, D, C, B` plus checksum — and the slave-side
   `serve_loop()` implementing commands 0–4 per §4.
5. `remote_read(peer_addr, n)` and `remote_write(peer_addr, data)` per §4.
   With just these two you can already talk to a real cartridge.
6. The Card Pop! session per §6, plus `card_pop()` per §7 if you want to
   predict (or verify) the result.

### 8.4 Testing without a second console

Because command 2 reads back whatever address you name, the quickest smoke test
is:

1. Put the Game Boy into Card Pop! and let it sit in `IR_TrySlave` (do **not**
   press A on it).
2. From the Pi, run `sync_as_sender()`, then `send_command(A=2, HL=$C5EB,
   DE=<anything>, BC=4)` and read the 4-byte reply.
3. You should get back `01 'P' 'K' '1'` — mode 1, magic, version.

If that round-trips, your bit layer, checksums and RPC framing are all correct
and the rest is bookkeeping.

---

## 9. The other two IR features

The same stack carries two more modes, selected by the byte at `$C5EB`:

| Mode | Feature | Payload |
|---|---|---|
| 1 | Card Pop! | see §6 |
| 2 | Send / receive a **card** | 61 bytes from `$C510`, then a `'O'` ($4F) acknowledgement written into the peer's `$C5EC` |
| 3 | Send / receive a **deck configuration** | 84 bytes from `$C510` |

Both follow the same shape: `CP_InitBuffers(mode)`, master/slave arbitration,
`CP_ExchangeIdent`, one `IR_RemoteWrite` of the payload, status byte, disconnect.
Their driver routines are at `06:$5BAC` / `06:$5BF7` (mode 2, send / receive)
and `06:$5C30` / `06:$5C66` (mode 3), with the Card Pop! driver at `06:$5C8B`.

---

## 10. Reference implementation

`host/` in this repository is a Raspberry Pi implementation of
everything above: one GPIO out to an IR LED, one GPIO in from an unmodulated
detector. It covers the bit layer, framing, both RPC roles, the mode-2 card
transfer, Card Pop! as the master, SRAM access through the mapper registers,
and the card-selection algorithm — including forging a trainer name that
forces a chosen card onto the cartridge.

It also carries `payload.asm`, a 414-byte resident bridge uploaded into the
cartridge's work RAM and entered with RPC command 4. It brings its own copy of
the bit layer, so the cartridge can be swapped while the link stays up, and
`gen2save.py` then splices a Celebi into the Gen 2 save that comes back.

Three GPIO backends: a dependency-free `/dev/mem` one that builds on very old
Raspbian, wiringPi, and pigpio. See the PREEMPT_RT note in §8.2 — for a
polling backend an RT kernel is a requirement, not a nicety.
