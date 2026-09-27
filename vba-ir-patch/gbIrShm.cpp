// Shared-memory ring-buffer transport for the IR bridge. See gbIrShm.h.

#include "gbIrShm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace gbIrShm {

int open_local(Writer* w, const char* path)
{
    w->shm = 0;
    w->fd = -1;
    w->local_seq = 0;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("gbIrShm: open local"); return -1; }
    if (ftruncate(fd, sizeof(Shm)) < 0) {
        perror("gbIrShm: ftruncate local");
        close(fd);
        return -1;
    }
    void* p = mmap(0, sizeof(Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("gbIrShm: mmap local");
        close(fd);
        return -1;
    }
    // Zero first (wipe any stale state), then publish magic LAST so a
    // racing peer reader doesn't trust partial initialization.
    memset(p, 0, sizeof(Shm));
    Shm* s = (Shm*)p;
    s->header.version    = PROTO_VERSION;
    s->header.slot_count = SLOTS;
    __atomic_store_n(&s->header.seq, (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->header.magic, MAGIC, __ATOMIC_RELEASE);

    w->shm = s;
    w->fd  = fd;
    return 0;
}

int open_peer(Reader* r, const char* path)
{
    r->shm = 0;
    r->fd = -1;
    r->last_seq = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(Shm)) {
        close(fd);
        return -1;
    }
    void* p = mmap(0, sizeof(Shm), PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }

    Shm* s = (Shm*)p;
    uint32_t m = __atomic_load_n(&s->header.magic, __ATOMIC_ACQUIRE);
    if (m != MAGIC) {
        munmap(p, sizeof(Shm));
        close(fd);
        return -1;
    }
    r->shm = s;
    r->fd  = fd;
    /*
     * The ring file is a plain file at a fixed path that outlives its writer's
     * process -- nothing deletes /dev/shm/tcg_pi when gbcpop exits. A fresh
     * reader connecting to a path that still holds a PREVIOUS session's file
     * will find a valid magic and a header.seq left over from that dead
     * writer. Starting last_seq at 0 made the lapped-writer clamp below replay
     * up to SLOTS of that stale backlog as if it were live traffic just
     * arriving now on the very next read. A reader must only ever see edges
     * published AFTER it connects, so skip straight to the writer's current
     * position. (Mirrors the identical fix in the poketcg repo's C99 port,
     * host/shmring.c, found via `gbcpop serve` decoding garbage
     * with no VBA running at all.)
     */
    r->last_seq = __atomic_load_n(&s->header.seq, __ATOMIC_ACQUIRE);
    return 0;
}

void close_writer(Writer* w)
{
    if (w->shm) { munmap(w->shm, sizeof(Shm)); w->shm = 0; }
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
}

void close_reader(Reader* r)
{
    if (r->shm) { munmap(r->shm, sizeof(Shm)); r->shm = 0; }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
}

void publish(Writer* w, long long cT, int led_on)
{
    if (!w->shm) return;
    uint64_t new_seq = w->local_seq + 1;
    Slot& slot = w->shm->slots[(new_seq - 1) % SLOTS];
    slot.cT     = (uint64_t)cT;
    slot.led_on = (uint8_t)(led_on ? 1 : 0);
    slot.sched  = 0;                    // VBA's own LED edges are real-time
    __atomic_store_n(&slot.seq, new_seq, __ATOMIC_RELEASE);
    __atomic_store_n(&w->shm->header.seq, new_seq, __ATOMIC_RELEASE);
    w->local_seq = new_seq;
}

void set_live_cT(Writer* w, long long cT)
{
    if (!w->shm) return;
    __atomic_store_n(&w->shm->header.live_cT, (uint64_t)cT, __ATOMIC_RELEASE);
}

long long peer_live_cT(Reader* r)
{
    if (!r->shm) return -1;
    return (long long)__atomic_load_n(&r->shm->header.live_cT, __ATOMIC_ACQUIRE);
}

int read_next(Reader* r, Slot* out)
{
    if (!r->shm) return 0;
    for (;;) {
        uint64_t cur = __atomic_load_n(&r->shm->header.seq, __ATOMIC_ACQUIRE);

        // Peer restarted (seq went backwards). Treat as fresh start.
        if (cur < r->last_seq) r->last_seq = 0;
        if (cur == r->last_seq) return 0;

        // Writer lapped us — clamp to the oldest slot still intact.
        if (cur > SLOTS && r->last_seq < cur - SLOTS)
            r->last_seq = cur - SLOTS;

        uint64_t s = r->last_seq + 1;
        const Slot& slot = r->shm->slots[(s - 1) % SLOTS];
        uint64_t observed = __atomic_load_n(&slot.seq, __ATOMIC_ACQUIRE);
        r->last_seq = s;

        if (observed == s) {
            *out = slot;
            return 1;
        }
        // Slot was overwritten between the cur load and the slot load;
        // skip and try the next one.
    }
}

}  // namespace gbIrShm
