# 8. History and lessons learned

This chapter is the story of how the launcher came to be, and the things that
only became clear by building it. It is written for a reader who wasn't there;
the working notes it is distilled from are not part of this repository.

## Where it started

The starting point was my earlier, unrelated effort to reverse-engineer Pokémon
Gold/Silver/Crystal's **Mystery Gift** over infrared (the same work behind the
IR board in chapter 4). For that, I had patched a build of VisualBoyAdvance
1.8.0 to give the emulator a real IR "cable":
one emulator's `$FF56` light output is carried, through a small shared-memory
ring on `/dev/shm`, to another program, with every LED edge timestamped by the
CPU's cycle counter. That patched emulator was an internal tool — it is not
shipped here; only the patch files that recreate the bridge on a stock VBA are
(`vba-ir-patch/`, chapter 3).

Having a way to watch and drive `$FF56` cycle-exactly is what made the TCG work
tractable. The TCG's "Card Pop!" turned out to be built on a tiny
remote-procedure-call layer: two cartridges **read, write and call** each
other's memory over IR, with the address as a parameter (chapter 2). That is a
general-purpose foothold — the same commands the game uses to exchange cards can
place a routine in the console's work RAM and run it. The rest of the project is
what it takes to turn that foothold into a reliable homebrew launcher.

## Two protocols, on purpose

The single most important design decision is the split between two IR protocols
(chapter 5).

The TCG's own Card Pop! bit layer is **timing-tight**: a bit cell is 440 T
(≈ 105 µs), a `0` is a ~26 µs pulse, and one bad byte aborts the transfer. That
is fine for a few hundred bytes of bootstrap with retries, but hopeless for
kilobytes over a hand-aimed IR link. So Stage 0 uses the game's bit layer for
**only** the ~0.5 KB bootstrap; once that runs, we own both ends of the wire and
switch to our own link — self-clocking, chunked, checksummed and acknowledged.
Everything above the bit layer is then independent of the host game, which is
why the loader, menu and payloads are reusable (chapter 9).

## The crux: clock the link on the game's cycles, not on wall time

The first version of the emulator link clocked the bit layer on wall-clock time.
It never decoded a single byte, and the reason is the lesson that shaped the
whole host side:

An emulator does not run the game in real time. VBA advances the Game Boy in
bursts of roughly a thousand cycles — about one IR byte — and only sleeps once
per frame. So it swallows a whole byte's worth of our 26 µs pulses inside one
game read and stamps them all at a single instant: the individual edges never
exist, and nothing decodes. No amount of wall-clock tuning survives a scheduling
granularity ~600× coarser than a bit cell.

The fix was to clock the **entire link on the Game Boy's own cycle counter**.
The emulator exports its live cycle count; the host schedules each edge at a
future cycle target and decodes received bytes from the cycle stamp on each of
the game's transitions, using wall time only for timeouts. A "440 T cell" is then
440 T no matter how the emulator paces wall time — which also means the PC needs
no real-time kernel for the emulator link. On real hardware this question does
not arise: an ATtiny clocks the wire directly.

This is worth internalizing: **on an emulator, time is the emulated CPU's clock,
not the host's.** Timeout windows sized in milliseconds of wall time shrank to a
fraction of that in game-time under load and had to be re-expressed in cycles.

## Emulator quirks are not hardware

Getting the link working on VBA meant absorbing two emulator artifacts, entirely
on the host side (the emulator patch stayed generic):

* **Inverted light polarity.** The ROM writes "bit 0 set" to turn the LED on;
  VBA 1.8.0 treats bit 0 set as off, so the game's pulses arrive inverted.
* **A short cell.** VBA 1.8.0's core runs the IR loops ~18 % fast, so a 440 T
  cell is ~90 emulated cycles and the pulse ~23, versus 110/27 on real hardware.

Both are properties of that emulator build, not of the console, and the ATtiny
uses the true hardware figures. The takeaway is to treat emulator timing and
polarity as things to be measured and compensated, and never to bake an
emulator's numbers into code meant for hardware.

One more class of bug came from the shared-memory bridge itself rather than the
protocol: a stale backlog left in the ring from a previous run was once replayed
as if it were live traffic. And a memorable "the game received the wrong trainer
name" turned out not to be a protocol fault at all but a character-**encoding**
mismatch — a reminder to rule out representation before suspecting the wire.

## Making the link reliable

With the link decoding, the remaining work was reliability over a real,
hand-aimed beam. This went through three generations:

1. **Per-packet ACK/NAK.** Numbered packets, the receiver acknowledges each; the
   sender retransmits on a bad or missing acknowledgement.
2. **Per-copy ACK with stream IDs.** Sending several copies of each stream and
   letting the receiver acknowledge the first good one, so a clean copy ends the
   stream early instead of waiting out a fixed repeat count. A frame header
   carries a stream ID (loader, font, menu, manifest, body) and the receiver
   re-acknowledges a repeat of the last stream without storing it.
3. **Chunked stop-and-wait.** On real IR, a whole ~900-byte loader copy rarely
   survived intact, so streams go in chunks of ≤128 bytes, each RLE-compressed,
   each sent up to eight times until acknowledged. A bad chunk costs a fraction
   of a second, not the whole stream. The receiver stores chunks strictly in
   order, re-acknowledges a repeat of the last one, and bounds every chunk to
   its buffer before writing.

A final wrinkle: right before the receiver stops listening — to draw the menu,
or to jump to the payload — a lost *final* acknowledgement can't be repaired the
normal way, so the receiver lingers, re-acknowledging repeats until the line has
been quiet for ~100 ms. With that, "no acknowledgement after eight tries" is a
true statement that the chunk never arrived, which the sender reports rather than
launching a half-received payload.

## Bringing it to hardware: the ATtiny

Porting the launch from the PC driver to a standalone ATtiny85 was
straightforward in structure — the firmware streams the same wire frames,
precomputed on the PC (`gbcpop attiny-inc`), so both hosts send byte-identical
data — but it surfaced a set of lessons that only hardware teaches:

* **RC-oscillator settling.** The very first Card Pop! write after power-up
  reproducibly came back one bit off and restarted the round. The internal
  RC oscillator hasn't settled that early, and Card Pop! is timing-tight. The
  fix is simply to retry each Stage-0 write a few times; the symptom then
  disappears.
* **Pick the right timer.** The ~33 ms window in which the firmware waits before
  acknowledging a chunk was first timed with one hardware timer that ended far
  too early on the real chip (a simulator missed it too, as it doesn't emulate
  that timer). Timing it on a different timer's overflows fixed it. General
  lesson: verify a timer's behaviour on the actual part, not only in
  simulation.
* **The status LEDs are infrared too.** The console's IR receiver is broadband
  enough to see the board's own status LEDs. They had to be switched off during
  every acknowledgement window, which is invisible to the eye but not to the
  receiver.
* **Cartridge swaps are an electrical event.** Swapping a cartridge into a
  running console rebooted the GBC when the audio dumper was active. The dumper
  now keeps the sound hardware off until the new cartridge has passed its header
  check. More broadly, a hot-swap draws an inrush current that dips the supply;
  on weak batteries this can reset the console or, more subtly, glitch a write
  (see the save-patcher notes in chapter 7). Swapping a cartridge while the
  loader menu is shown is safe because nothing is being written then.
* **Normal speed.** Our code runs at CGB normal speed, because the TCG hands off
  inside its IR transaction, which is in normal speed. A bootstrap for a
  different game that is in double speed at hand-off must switch down first.

The bootstrap stays under the 512-byte window that is provably free while the
game services IR, and the resident loader is about 1 KB. Both figures, and the
free-RAM reasoning behind them, are in chapter 5.

## The audio dumper, and running a ROM from RAM

Launching our own small game (Snake) was easy: it was written to run from the
staging address the loader jumps to. The harder case was FIX94's audio dumper,
a full ROM that assumes ROM-bank layout and fixed addresses. The question "how
do you run a ROM image from work RAM?" was answered with a linker trick — build
the code into a RAM section while it physically lives at a ROM offset the tools
extract — the same trick the loader itself uses. The details and the small
upstream patch are in chapter 7.

## Lessons, distilled

* A documented, intended remote read/write/call feature is a general foothold;
  the engineering is everything after it.
* Split a timing-tight, unforgiving transport from a forgiving one: use the
  strict one only for the minimum, then switch to your own.
* On an emulator, time is the emulated CPU's clock. Size every window in cycles,
  not wall time.
* Treat an emulator's polarity and timing as measured quantities to compensate,
  never as truth for hardware.
* Rule out encoding and stale buffers before blaming the protocol.
* Reliability over a hand-aimed beam wants small, individually acknowledged
  chunks and an explicit repair of the last acknowledgement.
* Hardware adds its own failure modes — oscillator settling, timer behaviour,
  stray IR from indicator LEDs, supply dips on cartridge insertion — that no
  amount of emulator testing predicts.
