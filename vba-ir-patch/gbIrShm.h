// Shared-memory ring-buffer transport used by the IR bridge.
//
// Both VBA (src/gb/gbIR.cpp) and the standalone simulator
// (src/sdl/gbIRsim.cpp) speak this protocol. Wire layout must stay in
// sync with whatever is documented in INFRARED_REVERSING.md and the
// comment block at the top of gbIR.cpp.

#ifndef GBIRSHM_H
#define GBIRSHM_H

#include <stdint.h>

namespace gbIrShm {

const uint32_t MAGIC   = 0x42524956u;  // 'VIRB' little-endian
const uint32_t PROTO_VERSION = 2u;     // v2 adds header.live_cT + cT-scheduled edges
const uint32_t SLOTS   = 1024u;

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t _reserved;
    uint64_t seq;          // writer's event counter, publish point
    uint64_t live_cT;      // v2: writer's current clock, refreshed continuously
                           //     so the peer can pace edges in the writer's time
};

struct Slot {
    uint64_t seq;          // matches the event number of this slot
    uint64_t cT;           // v2: the clock at which this edge takes effect.
                           //     For a real-time edge (the writer's own LED)
                           //     that is "now"; a scheduling peer sets it to a
                           //     future target in the *reader's* clock so the
                           //     reader applies the edge when its cT reaches it.
    uint8_t  led_on;       // 1 = LED emitting, 0 = LED off
    uint8_t  sched;        // v2: 1 = cT is a target in the reader's clock
                           //     (apply when the reader's cT reaches it);
                           //     0 = real-time edge, apply on arrival (a peer
                           //     emulator's own LED — preserves two-VBA trades)
    uint8_t  _pad[6];
};

struct Shm {
    Header header;
    Slot   slots[SLOTS];
};

// Writer handle (one per process — this is our outgoing ring).
struct Writer {
    Shm*     shm;
    int      fd;
    uint64_t local_seq;    // last seq we published
};

// Reader handle (one per process — attached to the peer's outgoing ring).
struct Reader {
    Shm*     shm;
    int      fd;
    uint64_t last_seq;     // last seq we consumed
};

// Create/overwrite `path`, mmap RW, zero, publish magic LAST. Returns 0
// on success, -1 on failure (prints errno details).
int open_local(Writer* w, const char* path);

// Open `path` RO, verify magic. Returns 0 on success, -1 if the peer has
// not started yet (or file is incomplete). Safe to retry indefinitely.
int open_peer(Reader* r, const char* path);

// Close and unmap. Leaves the backing file on disk (tmpfs) so a peer that
// is still reading can finish; caller removes the file if desired.
void close_writer(Writer* w);
void close_reader(Reader* r);

// Publish one LED transition on the writer's ring. `cT` is the effective
// clock of the edge (see Slot::cT).
void publish(Writer* w, long long cT, int led_on);

// v2: refresh the writer's live clock so the peer can schedule against it.
// Cheap (one atomic store); call it whenever the writer's clock advances at
// a point the peer cares about.
void set_live_cT(Writer* w, long long cT);

// v2: read the peer's live clock. Returns -1 if the peer is not attached.
long long peer_live_cT(Reader* r);

// Read the next unseen slot from the reader's ring into `out`. Returns
// 1 if a slot was produced, 0 if the ring has been fully drained since
// the last call. Intended for a while-loop:
//
//     gbIrShm::Slot s;
//     while (gbIrShm::read_next(&reader, &s)) { handle(s); }
int read_next(Reader* r, Slot* out);

}  // namespace gbIrShm

#endif
