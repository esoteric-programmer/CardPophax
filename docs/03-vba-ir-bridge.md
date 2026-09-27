# 3. The VisualBoyAdvance IR bridge

This is the project's **local test bench**, not a launch path. The launch
itself is the ATtiny85 (chapters [4](04-attiny-hardware.md) and [6](06-attiny-firmware.md)). A changed bootstrap, loader or
payload is first run here, against the same GB code and the same wire frames
the ATtiny will send, so mistakes show up before a build is flashed. To do
that, we connect the IR port
(`$FF56`) of a patched VisualBoyAdvance to `gbcpop` (or a second emulator)
through a shared-memory ring buffer on `/dev/shm`. The patch files and how to
apply them are in [`../vba-ir-patch/`](../vba-ir-patch/) (with its own README);
the host end of the same ring is `host/shmring.c`.

This chapter describes what the bridge is; the integration details (which files,
which hooks in `GB.cpp`, the build-file entries, the environment variables and
the LED-polarity quirk) are all in `vba-ir-patch/README.md`, so they are not
repeated here.

## What it does

* Every write to `$FF56` bit 0 (the LED) is published, with the emulator's CPU
  M-cycle counter `cT`, to our own ring file (`VBA_IR_LOCAL_SHM`).
* Every read of `$FF56` bit 1 (the peer's light) is answered from the peer's
  ring file (`VBA_IR_PEER_SHM`).
* Transitions are logged (`VBA_IR_DUMP_FILE`, or stderr) as
  `[cT=…] TX/RX ON/OFF after N ticks (~X us)`, with the peer's own `cT` on RX
  lines so two logs line up.

The transport (`gbIrShm`) is a lock-free single-producer/single-consumer ring:
each side writes one file and reads the peer's read-only. `host/shmring.c` is a
dependency-free C99 port of the same wire format, which is how `gbcpop` speaks
to VBA.

## Clock caveats (they shaped the host code)

VBA's `cT` only advances while the CPU runs, and it can lag wall-clock time
under load. Two consequences the host (`gbcpop`) has to handle, and which do
**not** exist on real hardware:

* **The clock freezes once the GB stops touching `$FF56`** (e.g. after the
  payload has jumped). `gbcpop` uses "the clock stopped moving for 500 ms" to
  mean "the GB moved on", not a fixed wall-clock timeout.
* **Timing windows must be measured in `cT`, not wall time.** A 30 ms
  wall-clock ACK window shrank to ~3 ms of GB time under load; `gbcpop` times
  its ACK windows on the peer's `cT`.

Also note the LED-polarity inversion documented in `vba-ir-patch/README.md`:
a VBA log's `TX ON` is the *inverse* of the real LED. `decode_txlog.py` and
`gbcpop` compensate.

## `run_cal.sh` and `decode_txlog.py`

`host/run_cal.sh <rom> <gbcpop args…>` starts VBA headless with the IR bridge,
runs one `gbcpop` command against it, then prints a summary and the decoded GB
transmissions. `host/decode_txlog.py` turns a VBA IR log into the bytes the GB
sent (polarity-aware). Test hooks (`GBCPOP_TEST_*`) are listed in [chapter 5](05-loader-protocol.md).

> **simavr** is a different tool for a different question: it simulates the
> **ATtiny**, not the Game Boy, and was used to check the AVR's pin timing
> (`attiny/sim/pb0_trace.c`). See [chapter 8](08-history-and-lessons.md).
