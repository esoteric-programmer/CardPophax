# 9. Ideas and limits

Directions the design leaves open, and the limits it has today. None of this is
built; each is weighed against what the launcher can already do.

## Writing a save back over IR

The payloads that patch a save do so entirely on the Game Boy — they never send
data back. A payload that instead **read** a save out over IR (to back it up
before writing, or to dump a cartridge without the audio path) needs three
things the current link doesn't have:

* **A GB→host bulk stream.** Today the Game Boy only sends acknowledgements and
  the one-byte menu choice; a chunked GB→host direction would be new, and the
  host would need somewhere to put 32 KB (more than an ATtiny85 has — this is
  where a larger microcontroller with an SD card comes in).
* **Bigger streams.** The chunk index is 7 bits (16 KB per stream) and the
  staging buffer is 4 KB. A 32 KB save needs a longer index, or several streams,
  and writing chunks straight to save RAM instead of staging them.
* **A loader API.** The loader stays resident in WRAM bank 7 unless a payload
  overwrites it, and already has a fixed jump table at `$D000` (entry) and
  `$D003` (return to the menu, chapter 5 §7a). More entries (receive a stream,
  send a byte) would let payloads use the IR link without carrying their own
  copy of the receive layer.

## Another game as the entry point

Everything above Stage 0 is game-independent by design (chapter 5): the loader,
the IR link, the menu, the relocate-and-jump trampoline and the payloads talk
only to the IR register (`$FF56`, the same on every GBC) and to WRAM bank 7,
VRAM and HRAM. Once the bootstrap has taken over, the host game no longer
matters, so a different entry-point game would reuse Stages 1–3 unchanged.

Only two things are TCG-specific:

* **Stage 0** — how the ~0.5 KB bootstrap is placed in RAM and run. For the TCG
  this is Card Pop!'s own remote write/call (chapter 2); another game needs its
  own mechanism here.
* **The bootstrap's entry assumptions** — a verified-free RAM window to load
  into, CPU speed at hand-off (the TCG hands off in normal speed; a game in
  double speed must switch down), and the state left on the stack.

The one hard prerequisite is what made the TCG usable: the game must offer, over
IR, some way to write ~0.5 KB into RAM and run it. For the TCG that is an
intended feature. For a game without one, the same foothold would have to come
from a bug in its IR handling — a research question, not a given.

Other GBC games do a two-way exchange over the console's IR port and are worth a
look as alternative entry points:

* Pokémon Gold / Silver / Crystal
* Pokémon Pinball
* Super Mario Bros. Deluxe
* Donkey Kong Country (GBC)
* Mission: Impossible
* Mary-Kate & Ashley: Pocket Planner
* Bomberman Max Red / Blue

Whether any of their IR code can be steered into running our bytes is unknown
and would mean studying the game first; only the Pokémon titles are disassembled.

## Smaller ideas

* **Progress bar.** While a payload streams in, the menu could show a bar under
  it, advanced one tile per acknowledged chunk. Barely worth it for Snake (about
  a second); useful for the audio dumper; needed for a future 32 KB
  transfer.

## Known limits

* **No whole-payload integrity check.** Each chunk has a checksum and an
  acknowledgement, but there is no CRC over the finished payload.
* **A garbled menu choice can pick the wrong payload.** The one-byte choice is
  unacknowledged; a lost choice recovers (the loader asks once more after
  ~0.5 s, then returns to its menu), but a
  choice corrupted into another valid one launches the wrong payload. Rare.
* **The Raspberry Pi launcher is Pi 3 only.** It needs `gbcpop`'s default
  (direct `/dev/mem`) build and the RT kernel; the pigpio and wiringPi backends
  speak only the Card Pop! stage, and the Pi 4 and 5 are not supported
  (chapter 11).
