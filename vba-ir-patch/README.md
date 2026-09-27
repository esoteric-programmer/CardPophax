# VisualBoyAdvance IR bridge (patch)

These files add an infrared "cable" to **VisualBoyAdvance 1.8.0** (SDL build):
the Game Boy Color's IR port `$FF56` of one emulator is connected, through a
shared-memory ring buffer on `/dev/shm`, to a second emulator — or to any other
program speaking the same ring protocol, such as `gbcpop` (`host/shmring.c`).
Every LED transition is logged with CPU-cycle timestamps. How it works and what
we learned from it: [`../docs/03-vba-ir-bridge.md`](../docs/03-vba-ir-bridge.md).

Only the files we wrote are here; VisualBoyAdvance itself is not. VBA is GPLv2,
and so are these files.

| File | Goes to | Purpose |
|---|---|---|
| `gbIrShm.h`, `gbIrShm.cpp` | `src/gb/` | the lock-free SPSC ring (transport) |
| `gbIR.h`, `gbIR.cpp` | `src/gb/` | the VBA side: `$FF56` hooks, logging, peer drain |
| `gbIRsim.cpp` | `src/sdl/` | optional stand-alone second endpoint / test harness |

## Integration into VBA 1.8.0

### `src/gb/GB.cpp`

```cpp
#include "gbIR.h"
// ...
long long cT = 0;   // CPU M-cycle counter; used by the IR bridge for timing
```

In the main loop, right after an instruction's `clockTicks` are known:

```cpp
cT += clockTicks;
```

`gbWriteMemory`, register `$FF56`:

```cpp
    // IR register (RP) -- LED out on bit 0, peer LED in on bit 1, read-enable
    // on bits 6-7. The bridge (src/gb/gbIR.cpp) relays the LED state to the
    // peer over the shared-memory ring and logs every transition.
    case 0x56: {
      if(gbCgbMode) {
        gbIrOnWrite(value, gbMemory[0xff56], cT);
        gbMemory[0xff56] = value;
      }
    }
```

`gbReadMemory`, register `$FF56`:

```cpp
    case 0x56:
      if (gbCgbMode) {
        // Keep only the LED-out bit and the read-enable bits as storage;
        // bit 1 (peer signal) is supplied live from the bridge.
        u8 stored = gbMemory[0xff56] & 0xC1;
        if ((stored & 0xC0) == 0xC0)
          return stored | gbIrPeerBit1(cT);
        return stored | 0x02;
      }
```

`gbReset()`:

```cpp
  cT = 0;
  gbIrReset(cT);
```

### Build files

`src/gb/Makefile.am` — add to the source list:

```
	gbIR.cpp	\
	gbIR.h		\
	gbIrShm.cpp	\
	gbIrShm.h	\
```

`src/sdl/Makefile.am` — optional simulator:

```
bin_PROGRAMS = VisualBoyAdvance gbIRsim
# gbIRsim: standalone IR-bridge partner / protocol test harness.
gbIRsim_SOURCES = gbIRsim.cpp
gbIRsim_LDADD   = ../gb/gbIrShm.o
gbIRsim_DEPENDENCIES = ../gb/gbIrShm.o
```

(The upstream tree was generated with automake 1.15; either regenerate, or add
the same entries to the generated `Makefile.in`.)

## Use

```sh
VBA_IR_LOCAL_SHM=/dev/shm/tcg_gb  VBA_IR_PEER_SHM=/dev/shm/tcg_pi \
VBA_IR_DUMP_FILE=/tmp/ir.log      ./src/sdl/VisualBoyAdvance game.gbc
```

The peer (e.g. `host/gbcpop` built with `make SHM=1`) uses the two paths the
other way round. With either variable unset the bridge is off (vanilla VBA).

## Known quirk: LED polarity

The bridge treats `$FF56` bit 0 = **0** as "LED on"; real hardware (and the TCG,
and our code) use bit 0 = **1** = on. Everything in this repository that talks
to VBA (`gbcpop`'s `SHM_RX_LIGHT`, `decode_txlog.py`) compensates, so it is
kept for compatibility — but a VBA log's `TX ON/OFF` is the inverse of the real
LED.
