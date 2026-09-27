# card-pop — a Card Pop! gift for the Pokémon Trading Card Game

A WRAM-resident payload that adds a **Mew** (A) or a **Venusaur** (B) to a TCG
save **exactly the way a successful Card Pop! records it**, so the save looks
like it had one more real Card Pop!. Mew Lv15 and Venusaur Lv64 are the two
Card Pop! exclusives.

**Legitimacy.**
* **Mew:** indistinguishable from one received through a real Card Pop!. The
  collection only stores a count, and the counter, history entry, backup copy
  and album progress are what the game itself writes. The partner name is one
  for which the game's own rule gives Mew for this player.
* **Venusaur:** can't be obtained the legit way. A parity bug in the game's
  card choice makes it unreachable through Card Pop!, so a save holding it
  can't have come from a real pop. Its history entry is the nearest possible:
  a partner name that leads into the game's rare-card branch.

It is in the default ATtiny set, on button B (`make PAYLOADS="snake card-pop
audio-dumper" flash` in `attiny/`). It shows as `CARD POP` in the menu.

## Use

1. Launch it from the loader menu. The TCG is still inserted (the loader runs
   from it), so its menu comes up right away: **A**: add Mew, **B**: add
   Venusaur. The button first checks the cartridge (you can also swap in
   another TCG cartridge before pressing it).
2. The result stays on screen for about 2.5 s, then the menu is back.
   `DONE. RESET THE GAME BOY TO PLAY.`: the TCG's own code in RAM was replaced
   by the loader, so the console has to be restarted to play it.
3. **SELECT** on the menu returns to the loader's menu (payload build only;
   not shown on screen).

| Screen | Meaning |
|---|---|
| `DONE.` (green) | recorded and read back |
| `99 CARDS ALREADY` (yellow) | collection + built decks already hold 99 of that card; nothing written |
| `NOT SUPPORTED` (red) | not a Western TCG cartridge (title `POKECARD`, MBC5, 32 KB SRAM, header checksum, not Japanese) |
| `NO SAVE FOUND` (red) | the general save data header or checksum is wrong, or there is no player name |
| `WRITE FAILED` (red) | the read-back did not match |

## What a Card Pop! writes, and so does this payload

Sources: pret/poketcg `engine/link/card_pop.asm` (`_DoCardPop`,
`HandleCardPopCommunications`, `DecideCardToReceiveFromCardPop`),
`engine/save.asm` (`_AddCardToCollectionAndUpdateAlbumProgress`),
`home/card_collection.asm`, `sram.asm`.

| | SRAM | Change |
|---|---|---|
| `sTotalCardPopsDone` | bank 0 `$A005` | + 1, 8-bit, so 255 wraps to 0 |
| `sCardPopNameList` | bank 0 `$BB00`, 16 × 16 bytes | the partner's name at slot `old count & 15`, so after 16 pops the oldest is overwritten |
| `sCardCollection` | `$A100 + id`, bank 2 (backup) then bank 0 | if collection + the four built decks < 99: count + 1, "not owned" bit 7 cleared |
| `sAlbumProgress` | `$B8FE`, bank 2 then bank 0 | owned ids, and 228 minus unowned Venusaur/Mew |

The order and the two banks follow the game: its first
`AddCardToCollection` call runs with SRAM bank 2 selected, the second in the
current bank. None of these bytes is covered by the general save checksum
(`$B804`, over `$B808`… up to `$B8BB`).

## The partner name

The partner is always **ATTINY**: six full-width letters (`$03 xx`, A = `$30`),
then `$00 $00`, then the two bytes that the name entry fills from the RNG
(`DisplayPlayerNamingScreen`: two `UpdateRNGSources` outputs). Simulating the
game's RNG from all 2^24 states shows that **all 65536 byte pairs are
reachable**, so any pair is a plausible name. The pair is chosen at run time:

* **Both cards use the same rule.** The game decides the card from both
  names' 16-byte hashes. When `sum(player) − sum(partner) ≡ 5 (mod 256)` it
  takes its rare branch: Venusaur Lv64 if bit 0 of the XOR difference is 0,
  else Mew Lv15. Bit 0 of an XOR of bytes always equals bit 0 of their sum, so
  with a difference of 5 it is always 1: always Mew. The pair is chosen to
  satisfy the sum rule for *your* player name, so:
  * **Mew:** a pop the real game would have made.
  * **Venusaur:** the nearest possible. The partner's name leads into the
    rare branch, and only the (unreachable) parity bit differs.

  There are 256 such pairs (any r1, then r2), and the list holds 16 names, so
  a fresh one always exists.
* Either way, the pair is changed until the whole 16-byte name is **not in
  the list yet**. So two ATTINY entries always differ in their last two
  bytes, like two real trainers with the same name.

This is stricter than the game requires. Only the **master's** list is
checked: the master looks up the slave's name in its own list and sends the
result to the slave. It also searches the slave's list for its own name, but a
bug discards that result. The slave checks nothing and, on success, always
writes the master's name and increments its counter. So the same two players
can pop again once the partner has rotated out of the master's 16 slots, and
the slave then holds a duplicate entry. That is a real game state, and the
same pair of names always gives the same card again. Keeping every generated
name new means each simulated pop is one the game allows whichever side was
master.

## Build and test

```sh
make card-pop.bin      # the $C000 payload (907 B)
make                   # card-pop.gbc: standalone ROM
make tests && ./test_card_pop.py [save.sav]   # needs PyBoy
```

`test_card_pop.py` runs on a real save (European TCG by default) and compares
every SRAM byte with a model of the game's Card Pop! writes. It also checks
that each name is new and, for Mew, meets the game's rule. It runs 13 cases,
and all pass:
* Mew, Venusaur, and three pops in a row
* SELECT ignored (the tests run the standalone build, which has no loader)
* 98 → 99 → limit, and the limit counting a card in a built deck
* bad save checksum, Japanese cart
* a full list at count 15 (slot 15), 16 (slot 0, the oldest is overwritten)
  and 255 (the counter wraps to 0)
* a generated name that is already listed, for both cards

A save patched this way also loads in the real game: pret/poketcg's US build,
which matches the original checksum. Its "Continue" screen shows the album going
from 226/226 to 227/227 (Mew owned), and the game continues normally. The
European save layout matches the US one (player name, decks, collection, name
list, general save data and album progress all at the same addresses).

**Status:** works on hardware (GBC + ATtiny, European TCG), as well as in
PyBoy and in the real TCG code. Back up the TCG save first.
