/*
 * shmring — SPSC shared-memory ring for the GBC IR bridge.
 *
 * A dependency-free C99 port of VisualBoyAdvance-fork's gbIrShm
 * (src/gb/gbIrShm.{h,cpp}). Wire-compatible with PROTO_VERSION 1: the same
 * struct layout, the same MAGIC, the same 1024-slot ring, the same
 * __atomic_* release/acquire discipline. It lets gbcpop's shm backend be the
 * peer of a patched VBA process without pulling C++ or the emulator tree in.
 *
 * Each process owns one ring file (writer, mmap RW) and opens the peer's ring
 * file (reader, mmap RO). One LED transition == one slot.
 */

#ifndef SHMRING_H
#define SHMRING_H

#include <stdint.h>

#define SHM_MAGIC         0x42524956u   /* 'VIRB' little-endian */
#define SHM_PROTO_VERSION 2u             /* v2: header.live_cT + cT-scheduled edges */
#define SHM_SLOTS         1024u

struct ShmHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t _reserved;
    uint64_t seq;              /* writer's event counter, publish point */
    uint64_t live_cT;          /* v2: writer's current clock, refreshed live */
};

struct ShmSlot {
    uint64_t seq;              /* matches this slot's event number */
    uint64_t cT;               /* v2: clock at which the edge takes effect
                                  (a real-time "now", or a future target in the
                                  reader's clock when sched == 1) */
    uint8_t  led_on;           /* 1 = LED emitting, 0 = off */
    uint8_t  sched;            /* v2: 1 = cT is a target in the reader's clock */
    uint8_t  _pad[6];
};

struct Shm {
    struct ShmHeader header;
    struct ShmSlot   slots[SHM_SLOTS];
};

struct ShmWriter {
    struct Shm *shm;
    int         fd;
    uint64_t    local_seq;
};

struct ShmReader {
    struct Shm *shm;
    int         fd;
    uint64_t    last_seq;
};

/* Create/overwrite path, mmap RW, zero, publish magic LAST. 0 ok, -1 fail. */
int  shm_open_local(struct ShmWriter *w, const char *path);
/* Open path RO, verify magic. 0 ok, -1 if the peer hasn't started (retry). */
int  shm_open_peer(struct ShmReader *r, const char *path);
void shm_close_writer(struct ShmWriter *w);
void shm_close_reader(struct ShmReader *r);
/* Publish one LED transition. `cT` is the edge's effective clock; `sched` = 1
 * marks it a future target in the reader's clock, 0 a real-time edge. */
void shm_publish(struct ShmWriter *w, long long cT, int led_on, int sched);
/* Read the next unseen slot; 1 if produced, 0 when drained. Loop it. */
int  shm_read_next(struct ShmReader *r, struct ShmSlot *out);
/* v2: the peer's live clock, or -1 if the peer is not attached. */
long long shm_peer_live_cT(struct ShmReader *r);
/* v2: refresh our own live clock so a scheduling peer can pace against us. */
void shm_set_live_cT(struct ShmWriter *w, long long cT);

#endif /* SHMRING_H */
