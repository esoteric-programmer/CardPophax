// gbIRsim — standalone IR bridge partner for VisualBoyAdvance.
//
// Attaches to the same /dev/shm ring buffer layout as src/gb/gbIR.cpp, so
// it can stand in for a second VBA process when you want to validate a
// reverse-engineered IR protocol without running another emulator.
//
// Usage:
//   gbIRsim [--local PATH] [--peer PATH] [--dump PATH] [--poll-us N]
//
// If --local / --peer are omitted, falls back to VBA_IR_LOCAL_SHM /
// VBA_IR_PEER_SHM env vars (so the same environment you use to launch
// VBA also works here).
//
// The clock (`cT`) is synthesized from CLOCK_MONOTONIC scaled to GB
// M-cycles (1.048576 MHz), so the numbers line up with VBA's own
// `cT` at 100% emulation speed.
//
// The PROTOCOL STUBS section near the bottom is where you plug in a
// state machine. By default the simulator is purely passive: it logs
// every peer transition and does not transmit anything.

#include "../gb/gbIrShm.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- clock

static struct timespec g_t_start;

static void clock_init()
{
    clock_gettime(CLOCK_MONOTONIC, &g_t_start);
}

// Wall-clock nanoseconds since start → GB M-cycles (1.048576 MHz).
//   ticks = ns * 1048576 / 1e9 = ns * 1048576 / 1000000000
static long long cT_now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long long ns = (long long)(t.tv_sec  - g_t_start.tv_sec)  * 1000000000LL
                 + (long long)(t.tv_nsec - g_t_start.tv_nsec);
    return ns * 1048576LL / 1000000000LL;
}

static double ticks_to_us(long long ticks)
{
    // Simulator runs at normal GBC speed; no double-speed concept here.
    return (double)ticks * (1.0 / 1048576.0) * 1e6;
}

// ---------------------------------------------------------------- logging

static FILE* g_dump = 0;

static void ir_log(const char* line)
{
    fputs(line, stdout);
    fflush(stdout);
    if (g_dump) { fputs(line, g_dump); fflush(g_dump); }
}

static void log_tx(int now_on, long long cT, long long prev_cT)
{
    long long dur = (prev_cT > 0) ? (cT - prev_cT) : 0;
    char buf[192];
    snprintf(buf, sizeof(buf),
             "[cT=%lld] TX %s after %lld ticks (~%.2f us) %s\n",
             cT, now_on ? "ON " : "OFF",
             dur, ticks_to_us(dur), now_on ? "off" : "on");
    ir_log(buf);
}

static void log_rx(int now_on, long long cT, long long prev_cT,
                   long long sender_cT)
{
    long long dur = (prev_cT > 0) ? (cT - prev_cT) : 0;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[cT=%lld] RX %s after %lld ticks (~%.2f us) %s  [peer cT=%lld]\n",
             cT, now_on ? "ON " : "OFF",
             dur, ticks_to_us(dur), now_on ? "off" : "on", sender_cT);
    ir_log(buf);
}

// ---------------------------------------------------------------- transport

static gbIrShm::Writer g_writer;
static gbIrShm::Reader g_reader;

static int       g_own_led_on       = 0;
static long long g_own_last_change  = 0;
static int       g_peer_led_on      = 0;
static long long g_peer_last_change = 0;

// Forward decl of protocol hook.
static void on_rx_edge(int led_on, long long local_cT, long long peer_cT,
                       long long prev_local_cT);

static void try_attach_peer(const char* peer_path)
{
    if (g_reader.shm) return;
    gbIrShm::open_peer(&g_reader, peer_path);
}

static void drain_peer(const char* peer_path)
{
    try_attach_peer(peer_path);
    if (!g_reader.shm) return;

    gbIrShm::Slot slot;
    while (gbIrShm::read_next(&g_reader, &slot)) {
        int new_state = slot.led_on ? 1 : 0;
        if (new_state != g_peer_led_on) {
            long long cT = cT_now();
            log_rx(new_state, cT, g_peer_last_change, (long long)slot.cT);
            long long prev = g_peer_last_change;
            g_peer_led_on      = new_state;
            g_peer_last_change = cT;
            on_rx_edge(new_state, cT, (long long)slot.cT, prev);
        }
    }
}

// Emit one LED-state change on our ring and log it locally.
// No-ops if `led_on` matches the current state (nothing to transition).
static void send_edge(int led_on)
{
    led_on = led_on ? 1 : 0;
    if (led_on == g_own_led_on) return;
    long long cT = cT_now();
    log_tx(led_on, cT, g_own_last_change);
    g_own_led_on      = led_on;
    g_own_last_change = cT;
    gbIrShm::publish(&g_writer, cT, led_on);
}

// ---------------------------------------------------------------- scheduling
//
// Simple one-shot scheduler: the protocol stub asks for an edge at a
// future cT, and the main loop fires it when cT_now() catches up. Queue
// depth is small (8 entries) because IR protocols are bit-banged and
// usually only have one or two pending edges at a time.

struct ScheduledEdge {
    long long target_cT;
    int       led_on;
    int       active;
};

static const int SCHED_N = 16;
static ScheduledEdge g_sched[SCHED_N];

static void schedule_edge(long long target_cT, int led_on)
{
    for (int i = 0; i < SCHED_N; ++i) {
        if (!g_sched[i].active) {
            g_sched[i].target_cT = target_cT;
            g_sched[i].led_on    = led_on ? 1 : 0;
            g_sched[i].active    = 1;
            return;
        }
    }
    fprintf(stderr, "gbIRsim: scheduler full — dropping edge at cT=%lld\n",
            target_cT);
}

static void scheduler_pump(long long now)
{
    // Fire all due entries; order doesn't matter because send_edge
    // dedupes no-op transitions.
    for (int i = 0; i < SCHED_N; ++i) {
        if (g_sched[i].active && now >= g_sched[i].target_cT) {
            send_edge(g_sched[i].led_on);
            g_sched[i].active = 0;
        }
    }
}

// ---------------------------------------------------------------- lifecycle

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

static void usage(const char* argv0)
{
    fprintf(stderr,
        "Usage: %s [--local PATH] [--peer PATH] [--dump PATH] [--poll-us N]\n"
        "\n"
        "  --local PATH     Path to our own ring file (writer). Default:\n"
        "                   $VBA_IR_LOCAL_SHM.\n"
        "  --peer  PATH     Path to peer's ring file (reader). Default:\n"
        "                   $VBA_IR_PEER_SHM.\n"
        "  --dump  PATH     Append transitions to this file. Default:\n"
        "                   $VBA_IR_DUMP_FILE (or no dump).\n"
        "  --poll-us N      Main-loop poll interval in microseconds.\n"
        "                   Default 50.\n"
        "\n"
        "Ctrl-C to stop. Edit the PROTOCOL STUBS section in gbIRsim.cpp\n"
        "to plug in a state machine.\n",
        argv0);
}


// ============================================================
// PROTOCOL STUBS — fill these in as you learn the protocol
// ============================================================
//
// Two entry points:
//   on_rx_edge(...)     called once per peer LED transition
//   proto_tick(...)     called every main-loop iteration (~poll-us)
//
// Emit LED pulses via either:
//   send_edge(led_on)               — fire now
//   schedule_edge(target_cT, on)    — fire when cT_now() >= target_cT
//
// Timing reference (from IR_NOTES.md, Gen-2 Pokémon):
//   carrier pulse (LED on):  ~23 ticks  (~22 us)
//   SHORT  off (0-bit):      ~126 ticks (~120 us)
//   MEDIUM off (HELLO ack):  ~157 ticks (~150 us)
//   LONG   off (1-bit):      ~314 ticks (~300 us)
//   START_MSG:  on 132, off 762
//   END_MSG:    on 114, off 620

enum ProtoState {
    ST_IDLE,
    // TODO: add your states here
    // e.g. ST_WAITING_HELLO, ST_GOT_HELLO, ST_RECEIVING_MSG, ...
};

static ProtoState g_state = ST_IDLE;

static void proto_init()
{
    g_state = ST_IDLE;
    // TODO: set up any protocol-specific state, counters, timers.
    fprintf(stderr, "gbIRsim: protocol stub initialized (passive). "
                    "Fill in on_rx_edge / proto_tick in gbIRsim.cpp.\n");
}

static void on_rx_edge(int led_on, long long local_cT, long long peer_cT,
                       long long prev_local_cT)
{
    // Called once per peer LED transition. `led_on` is the NEW state of
    // the peer's LED. `local_cT - prev_local_cT` is the duration the peer
    // spent in the opposite state (on our clock). `peer_cT` is the peer's
    // own cT at the instant they emitted the edge.
    //
    // Default: do nothing. on_rx_edge transitions are already logged in
    // drain_peer() above.
    (void)led_on;
    (void)local_cT;
    (void)peer_cT;
    (void)prev_local_cT;

    // Example sketch — mirror-back a single ~23-tick pulse 762 ticks after
    // every RX rising edge. Uncomment to sanity-test scheduling:
    //
    //   if (led_on) {
    //       schedule_edge(local_cT + 762,       1);
    //       schedule_edge(local_cT + 762 + 23,  0);
    //   }
}

static void proto_tick(long long local_cT)
{
    // Called every poll iteration. Use for timeouts and periodic emits.
    (void)local_cT;
}

// ============================================================
// end PROTOCOL STUBS
// ============================================================


int main(int argc, char** argv)
{
    const char* local_path = getenv("VBA_IR_LOCAL_SHM");
    const char* peer_path  = getenv("VBA_IR_PEER_SHM");
    const char* dump_path  = getenv("VBA_IR_DUMP_FILE");
    int         poll_us    = 50;

    static struct option opts[] = {
        {"local",    required_argument, 0, 'l'},
        {"peer",     required_argument, 0, 'p'},
        {"dump",     required_argument, 0, 'd'},
        {"poll-us",  required_argument, 0, 'P'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "l:p:d:P:h", opts, 0)) != -1) {
        switch (c) {
            case 'l': local_path = optarg; break;
            case 'p': peer_path  = optarg; break;
            case 'd': dump_path  = optarg; break;
            case 'P': poll_us    = atoi(optarg); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 2;
        }
    }
    if (!local_path || !peer_path) {
        fprintf(stderr, "gbIRsim: --local / --peer (or VBA_IR_LOCAL_SHM / "
                        "VBA_IR_PEER_SHM) are required.\n\n");
        usage(argv[0]);
        return 2;
    }
    if (poll_us < 1) poll_us = 1;

    clock_init();

    if (gbIrShm::open_local(&g_writer, local_path) < 0) {
        fprintf(stderr, "gbIRsim: cannot open local ring at %s\n", local_path);
        return 1;
    }
    if (dump_path) {
        g_dump = fopen(dump_path, "a");
        if (!g_dump) perror("gbIRsim: dump fopen");
    }

    fprintf(stderr,
            "gbIRsim: running, local=%s peer=%s dump=%s poll=%d us\n"
            "         Ctrl-C to stop.\n",
            local_path, peer_path, dump_path ? dump_path : "(none)", poll_us);

    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    proto_init();

    while (!g_stop) {
        drain_peer(peer_path);
        long long now = cT_now();
        scheduler_pump(now);
        proto_tick(now);
        usleep(poll_us);
    }

    fprintf(stderr, "\ngbIRsim: shutting down.\n");
    gbIrShm::close_writer(&g_writer);
    gbIrShm::close_reader(&g_reader);
    if (g_dump) fclose(g_dump);
    return 0;
}
