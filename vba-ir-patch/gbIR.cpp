// GBC IR bridge — see gbIR.h and gbIrShm.h.
//
// Thin wrapper over gbIrShm: adds VBA-specific logging and the u8-based
// 0xFF56 API that GB.cpp expects.

#include "gbIR.h"
#include "gbIrShm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <climits>

extern int gbSpeed;  // 0 = normal, 1 = CGB double-speed

namespace {

gbIrShm::Writer writer;
gbIrShm::Reader reader;
char peer_path_buf[1024];
int  have_peer_path = 0;
FILE* dump          = 0;
int  enabled        = 0;

int       own_led_on       = 0;
long long own_last_change  = 0;
int       peer_led_on      = 0;
long long peer_last_change = 0;

double ticks_to_us(long long ticks)
{
    double per = gbSpeed ? (1.0 / 2097152.0) : (1.0 / 1048576.0);
    return (double)ticks * per * 1e6;
}

void ir_log(const char* line)
{
    fputs(line, stdout);
    fflush(stdout);
    if (dump) { fputs(line, dump); fflush(dump); }
}

void log_tx(int now_on, long long cT, long long prev_cT)
{
    long long dur = (prev_cT > 0) ? (cT - prev_cT) : 0;
    char buf[192];
    snprintf(buf, sizeof(buf),
             "[cT=%lld] TX %s after %lld ticks (~%.2f us) %s\n",
             cT, now_on ? "ON " : "OFF",
             dur, ticks_to_us(dur), now_on ? "off" : "on");
    ir_log(buf);
}

void log_rx(int now_on, long long cT, long long prev_cT, long long sender_cT)
{
    long long dur = (prev_cT > 0) ? (cT - prev_cT) : 0;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[cT=%lld] RX %s after %lld ticks (~%.2f us) %s  [peer cT=%lld]\n",
             cT, now_on ? "ON " : "OFF",
             dur, ticks_to_us(dur), now_on ? "off" : "on", sender_cT);
    ir_log(buf);
}

void try_attach_peer()
{
    if (reader.shm || !have_peer_path) return;
    gbIrShm::open_peer(&reader, peer_path_buf);  // silent retry on failure
}

// v2: a scheduling peer (e.g. the Raspberry-Pi tool over shm) hands us edges
// tagged with a *future* target in our own clock, so a 26 us pulse lands at
// the right cT no matter how coarsely the SDL loop batches emulation. We hold
// them here and apply each when cT catches up. Real-time edges from another
// emulator carry target LLONG_MIN and fire on arrival (old two-VBA behavior).
const int PEND_MAX = 512;
struct PendEdge { long long target_cT; long long sender_cT; int led_on; };
PendEdge pend[PEND_MAX];
int pend_head = 0, pend_tail = 0;      // ring, [head, tail)

void pend_reset() { pend_head = pend_tail = 0; }

void pend_push(long long target_cT, long long sender_cT, int led_on)
{
    int nxt = (pend_tail + 1) % PEND_MAX;
    if (nxt == pend_head)              // full: drop oldest, never wedge
        pend_head = (pend_head + 1) % PEND_MAX;
    pend[pend_tail].target_cT = target_cT;
    pend[pend_tail].sender_cT = sender_cT;
    pend[pend_tail].led_on    = led_on;
    pend_tail = nxt;
}

void apply_pending(long long cT)
{
    while (pend_head != pend_tail) {
        PendEdge& e = pend[pend_head];
        if (e.target_cT > cT) break;   // not due yet — and neither is any later one
        int ns = e.led_on ? 1 : 0;
        if (ns != peer_led_on) {
            log_rx(ns, cT, peer_last_change, e.sender_cT);
            peer_led_on      = ns;
            peer_last_change = cT;
        }
        pend_head = (pend_head + 1) % PEND_MAX;
    }
}

void drain_peer(long long cT)
{
    try_attach_peer();
    if (reader.shm) {
        gbIrShm::Slot slot;
        while (gbIrShm::read_next(&reader, &slot)) {
            long long target = slot.sched ? (long long)slot.cT : LLONG_MIN;
            pend_push(target, (long long)slot.cT, slot.led_on ? 1 : 0);
        }
    }
    apply_pending(cT);
}

}  // namespace

void gbIrInit()
{
    static int tried = 0;
    if (tried) return;
    tried = 1;

    const char* local_path = getenv("VBA_IR_LOCAL_SHM");
    const char* peer_path  = getenv("VBA_IR_PEER_SHM");
    const char* dump_path  = getenv("VBA_IR_DUMP_FILE");

    if (!local_path || !peer_path) {
        fprintf(stderr,
                "gbIR: VBA_IR_LOCAL_SHM / VBA_IR_PEER_SHM not set, "
                "IR bridge disabled (game will see no peer).\n");
        return;
    }
    if (strlen(peer_path) >= sizeof(peer_path_buf)) {
        fprintf(stderr, "gbIR: peer shm path too long\n");
        return;
    }
    strcpy(peer_path_buf, peer_path);
    have_peer_path = 1;

    if (gbIrShm::open_local(&writer, local_path) < 0) return;

    if (dump_path) {
        dump = fopen(dump_path, "a");
        if (!dump) perror("gbIR: dump fopen");
    }

    enabled = 1;
    fprintf(stderr,
            "gbIR: shm bridge up, local=%s peer=%s dump=%s\n",
            local_path, peer_path, dump_path ? dump_path : "(none)");
}

void gbIrOnWrite(u8 new_value, u8 old_value, long long cT)
{
    gbIrInit();
    gbIrShm::set_live_cT(&writer, cT);   // let the peer pace against our clock
    drain_peer(cT);

    int new_on = (new_value & 0x01) ? 0 : 1;
    int old_on = (old_value & 0x01) ? 0 : 1;
    if (new_on != old_on) {
        log_tx(new_on, cT, own_last_change);
        own_led_on      = new_on;
        own_last_change = cT;
        gbIrShm::publish(&writer, cT, new_on);
    }
}

u8 gbIrPeerBit1(long long cT)
{
    gbIrInit();
    gbIrShm::set_live_cT(&writer, cT);   // refreshed every poll during reception
    drain_peer(cT);
    return peer_led_on ? 0x00 : 0x02;
}

void gbIrReset(long long cT)
{
    if (!enabled && !dump) return;
    char buf[96];
    snprintf(buf, sizeof(buf), "[cT=%lld] --- RESET ---\n", cT);
    ir_log(buf);
    own_led_on       = 0;
    own_last_change  = 0;
    peer_led_on      = 0;
    peer_last_change = 0;
    pend_reset();
}
