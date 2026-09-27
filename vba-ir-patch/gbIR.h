// GBC IR bridge — relays the 0xFF56 LED state between two VBA processes
// via an mmap'd shared-memory ring buffer (SPSC) so real IR protocols can
// be observed bidirectionally with sub-microsecond latency.
//
// Each side owns a ring file it only writes to, and mmaps the peer's ring
// read-only. Transport is lock-free: a monotonic sequence number in the
// header is the publication point, per-slot seq is a torn-read check.
//
// Config via environment variables:
//   VBA_IR_LOCAL_SHM   path to our own ring file (created, overwritten)
//   VBA_IR_PEER_SHM    path to peer's ring file  (opened read-only)
//   VBA_IR_DUMP_FILE   append-mode log file (optional)
//
// Both shm paths are required; if either is unset the bridge is disabled
// and the game sees no peer (vanilla behavior). Put the files on tmpfs
// (e.g. /dev/shm/vba_ir_A) — they must not hit a real disk.

#ifndef GBIR_H
#define GBIR_H

#include "../System.h"   // for u8

// Call once at startup (safe to call repeatedly; only first call initializes).
void gbIrInit();

// Called from the 0xFF56 write handler *before* gbMemory[0xff56] is updated,
// so the bridge can detect bit-0 transitions. `cT` is the current CPU
// M-cycle counter.
void gbIrOnWrite(u8 new_value, u8 old_value, long long cT);

// Called from the 0xFF56 read handler. Drains new peer slots, logs any new
// RX transitions, and returns the peer LED state as the value of register
// bit 1: 0 = light received, 2 = no light.
u8 gbIrPeerBit1(long long cT);

// Called on gbReset() so the log gets a clean boundary.
void gbIrReset(long long cT);

#endif
