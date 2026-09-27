/* shmring — see shmring.h. A C99 port of the VBA fork's gbIrShm transport. */

#define _POSIX_C_SOURCE 200809L    /* ftruncate, fstat, mmap under -std=c99 */

#include "shmring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int shm_open_local(struct ShmWriter *w, const char *path)
{
    int fd;
    void *p;
    struct Shm *s;

    w->shm = 0;
    w->fd = -1;
    w->local_seq = 0;

    fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("shmring: open local"); return -1; }
    if (ftruncate(fd, sizeof(struct Shm)) < 0) {
        perror("shmring: ftruncate local");
        close(fd);
        return -1;
    }
    p = mmap(0, sizeof(struct Shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        perror("shmring: mmap local");
        close(fd);
        return -1;
    }
    /* Zero first, then publish magic LAST so a racing reader skips partial
     * initialization. */
    memset(p, 0, sizeof(struct Shm));
    s = (struct Shm *)p;
    s->header.version    = SHM_PROTO_VERSION;
    s->header.slot_count = SHM_SLOTS;
    __atomic_store_n(&s->header.seq, (uint64_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->header.magic, (uint32_t)SHM_MAGIC, __ATOMIC_RELEASE);

    w->shm = s;
    w->fd  = fd;
    return 0;
}

int shm_open_peer(struct ShmReader *r, const char *path)
{
    int fd;
    struct stat st;
    void *p;
    struct Shm *s;
    uint32_t m;

    r->shm = 0;
    r->fd = -1;
    r->last_seq = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0 || (size_t)st.st_size < sizeof(struct Shm)) {
        close(fd);
        return -1;
    }
    p = mmap(0, sizeof(struct Shm), PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return -1; }

    s = (struct Shm *)p;
    m = __atomic_load_n(&s->header.magic, __ATOMIC_ACQUIRE);
    if (m != SHM_MAGIC) {
        munmap(p, sizeof(struct Shm));
        close(fd);
        return -1;
    }
    r->shm = s;
    r->fd  = fd;
    /*
     * The ring file is a plain file at a fixed path (/dev/shm/tcg_gb etc.) that
     * outlives its writer's process -- nothing deletes it when VBA or gbcpop
     * exits. A fresh reader connecting to a path that still holds a PREVIOUS
     * session's file will find a valid magic and a header.seq left over from
     * that dead writer. Starting last_seq at 0 made shm_read_next's catch-up
     * logic replay up to SHM_SLOTS of that stale backlog as if it were live
     * traffic just arriving now -- e.g. `gbcpop serve` run with no VBA up at
     * all decoded old edges from an earlier session's IR exchange. A reader
     * must only ever see edges published AFTER it connects, so skip straight
     * to the writer's current position.
     */
    r->last_seq = __atomic_load_n(&s->header.seq, __ATOMIC_ACQUIRE);
    return 0;
}

void shm_close_writer(struct ShmWriter *w)
{
    if (w->shm) { munmap(w->shm, sizeof(struct Shm)); w->shm = 0; }
    if (w->fd >= 0) { close(w->fd); w->fd = -1; }
}

void shm_close_reader(struct ShmReader *r)
{
    if (r->shm) { munmap(r->shm, sizeof(struct Shm)); r->shm = 0; }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
}

void shm_publish(struct ShmWriter *w, long long cT, int led_on, int sched)
{
    uint64_t new_seq;
    struct ShmSlot *slot;

    if (!w->shm) return;
    new_seq = w->local_seq + 1;
    slot = &w->shm->slots[(new_seq - 1) % SHM_SLOTS];
    slot->cT     = (uint64_t)cT;
    slot->led_on = (uint8_t)(led_on ? 1 : 0);
    slot->sched  = (uint8_t)(sched ? 1 : 0);
    __atomic_store_n(&slot->seq, new_seq, __ATOMIC_RELEASE);
    __atomic_store_n(&w->shm->header.seq, new_seq, __ATOMIC_RELEASE);
    w->local_seq = new_seq;
}

long long shm_peer_live_cT(struct ShmReader *r)
{
    if (!r->shm) return -1;
    return (long long)__atomic_load_n(&r->shm->header.live_cT, __ATOMIC_ACQUIRE);
}

void shm_set_live_cT(struct ShmWriter *w, long long cT)
{
    if (!w->shm) return;
    __atomic_store_n(&w->shm->header.live_cT, (uint64_t)cT, __ATOMIC_RELEASE);
}

int shm_read_next(struct ShmReader *r, struct ShmSlot *out)
{
    if (!r->shm) return 0;
    for (;;) {
        uint64_t cur = __atomic_load_n(&r->shm->header.seq, __ATOMIC_ACQUIRE);
        uint64_t s;
        const struct ShmSlot *slot;
        uint64_t observed;

        /* Peer restarted (seq went backwards). Treat as fresh start. */
        if (cur < r->last_seq) r->last_seq = 0;
        if (cur == r->last_seq) return 0;

        /* Writer lapped us — clamp to the oldest slot still intact. */
        if (cur > SHM_SLOTS && r->last_seq < cur - SHM_SLOTS)
            r->last_seq = cur - SHM_SLOTS;

        s = r->last_seq + 1;
        slot = &r->shm->slots[(s - 1) % SHM_SLOTS];
        observed = __atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE);
        r->last_seq = s;

        if (observed == s) {
            *out = *slot;
            return 1;
        }
        /* Slot overwritten between the two loads; skip and try the next. */
    }
}
