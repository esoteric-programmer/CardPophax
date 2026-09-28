/*
 * gbcpop — speak the Pokémon Trading Card Game (GBC) infrared protocol
 *          from a Raspberry Pi.
 *
 * Protocol reference: docs/02-tcg-cardpop-protocol.md
 *
 * Wiring (defaults, BCM numbering -- the Gen 2 Mystery Gift rig):
 *   GPIO 17  ->  IR LED driver, lit when high                   [TX]
 *   GPIO 18  <-  IR detector output, unmodulated, low when lit  [RX]
 *
 * The Game Boy Color drives its IR LED with plain DC and reads a bare
 * photodiode, so the RX side must NOT be a 38 kHz demodulator module
 * (TSOP17xx/TSOP382xx): use a photodiode/phototransistor front end into
 * a comparator or a 74HC14 Schmitt inverter.
 *
 * Build:  make            (direct /dev/mem backend, no libraries; see Makefile
 *                          for WIRINGPI=1 / PIGPIO=1 / SHM=1)
 * Run:    sudo ./gbcpop <command> [options]     (GPIO access needs root)
 *
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>

/*
 * Two GPIO backends.
 *
 *   default                         direct /dev/mem, no dependencies at all.
 *                                   Compiles on any Raspbian back to wheezy.
 *                                   Wants SCHED_FIFO; a PREEMPT_RT kernel makes
 *                                   it reliable, because an ordinary kernel lets
 *                                   network softirqs preempt the polling loop in
 *                                   the middle of a byte.
 *   make WIRINGPI=1                 wiringPi: same technique, but through the
 *                                   library the existing Mystery Gift tool is
 *                                   already proven against on this image.
 *   make PIGPIO=1                   pigpio: DMA waveforms out, DMA-sampled edge
 *                                   timestamps in. Better if you have it.
 *
 * Everything above tx_bytes()/recv_byte() is identical either way.
 */
#include <time.h>
#ifdef USE_PIGPIO
#include <pigpio.h>
#endif
#ifdef USE_WIRINGPI
#include <wiringPi.h>
#endif

#ifdef USE_SHM
#include "shmring.h"
#endif

#include "cardtable.h"

/* ------------------------------------------------------------------ */
/* Timing — all figures are single-speed Game Boy T-cycles / 4.194304 MHz */
/* ------------------------------------------------------------------ */

#define CELL_US        104.904   /* 440 T  — one bit cell                 */
#define PULSE_US       26        /* 108 T  — LED on, marks a logic 0      */
#define BYTE_GAP_US    300       /* idle before each start pulse          */
#define RX_BYTE_US     2190      /* the GBC's own receive timeout         */
#define RX_LONG_US     20000000  /* "wait for the user to press A"        */
#define RX_TURN_US     60000     /* between RPC transactions the GBC
                                    re-enables IRQs, waits a VBlank and
                                    switches CPU speed twice: be patient  */

#define MAX_TX_BYTES   300       /* one pigpio wave: 300 * 19 pulses max   */

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/*
 * Defaults match the rig the Gen 2 Mystery Gift tool already runs on:
 *   BCM 17  IR LED, lit when the GPIO is HIGH
 *   BCM 18  detector, reads LOW when light is present, pull-up enabled
 * Override for anything else.
 */
static int  cfg_tx_gpio   = 17;
static int  cfg_rx_gpio   = 18;
static int  cfg_tx_invert = 0;   /* 1: LED is on when the GPIO is low    */
static int  cfg_rx_invert = 1;   /* 1: detector reads low when lit       */
static int  cfg_rx_pull   = 2;   /* 0 off, 1 down, 2 up                  */
static int  cfg_verbose   = 0;

#ifdef USE_SHM
/* Shared-memory backend: peer of a patched VBA over /dev/shm rings. Ours is
 * the writer (VBA reads it), the peer ring is VBA's writer (we read it). */
static const char *cfg_shm_local = "/dev/shm/tcg_pi";
static const char *cfg_shm_peer  = "/dev/shm/tcg_gb";
#endif

static const char *cfg_name = "RASPI";

/* AddCardToCollection in the home bank. $1CCE is the European English ROM
 * (POKECARD / AXQP); the US build (AXQE) has it at $1D6E. Override with
 * --addcard if your cartridge differs. */
static uint16_t cfg_addcard = 0x1CCE;
static uint16_t cfg_stub_at = 0xC300;

#define LOG(...)   do { if (cfg_verbose) fprintf(stderr, "  " __VA_ARGS__); } while (0)
#define ERR(...)   fprintf(stderr, "error: " __VA_ARGS__)


/* ------------------------------------------------------------------ */
/* Debug recording                                                     */
/* ------------------------------------------------------------------ */
/*
 * Nothing is printed while a byte is in flight: printf inside the sampling
 * loop would destroy the very timing we are trying to measure, and a stall
 * longer than the peer's 2.19 ms deadline loses the transfer. Every byte is
 * recorded into a small ring instead and dumped at a safe point.
 *
 *   -v     protocol steps
 *   -vv    per-byte timing: every pulse, the cell it landed in, and the error
 *   -vvv   the above, printed immediately (breaks block transfers; fine for
 *          probe, selftest and monitor)
 */

#define DBG_EDGES 10
#define DBG_RING  512

struct dbg_rec {
    char     dir;                 /* 'T' transmitted, 'R' received */
    uint8_t  byte;
    int      ok;                  /* 0 = timed out waiting for a start pulse */
    uint32_t t0;                  /* tick of the start pulse                 */
    int      nedge;               /* pulses recorded after the start pulse   */
    int      lost;                /* pulses seen but not recorded (overflow) */
    uint16_t dt[DBG_EDGES];       /* microseconds after t0                   */
    int8_t   cell[DBG_EDGES];     /* which bit cell it was binned to, 1..8   */
    int16_t  err[DBG_EDGES];      /* tenths of a us from the ideal position  */
    uint16_t span;                /* t0 to the last pulse                    */
    uint16_t overruns;            /* transmit deadlines already past on entry */
    uint16_t worst_late;          /* worst transmit overshoot, microseconds  */
};

static struct dbg_rec dbg_ring[DBG_RING];
static int dbg_head = 0, dbg_count = 0;

static struct {
    unsigned long tx, rx, timeouts, badck, resync, overruns;
    double worst_err;             /* microseconds, receive side */
    unsigned long worst_late;     /* microseconds, transmit side */
} g_stats;

/*
 * The µs length of one bit cell *in the frame the decoder actually bins in*,
 * used only for diagnostics (dbg err/drift, worst_err). The GPIO backends bin
 * on the hardware 440 T cell (CELL_US); the shm backend bins on VBA's emulated
 * cell (MC_PER_CELL M-cycles), which is ~17 µs shorter — so hw_init overrides
 * this. Displaying err against the wrong cell inflated every figure by ~17 µs
 * per cell and made a healthy link look badly out of tolerance.
 */
static double g_cell_us = CELL_US;

static void dbg_print_rec(const struct dbg_rec *r);

static struct dbg_rec *dbg_push(char dir)
{
    struct dbg_rec *r;
    if (cfg_verbose < 2) return NULL;
    r = &dbg_ring[dbg_head];
    memset(r, 0, sizeof *r);
    r->dir = dir;
    dbg_head = (dbg_head + 1) % DBG_RING;
    if (dbg_count < DBG_RING) dbg_count++;
    return r;
}

/* level 3 prints as it goes; that is only safe when nothing is waiting on us */
static void dbg_maybe_now(const struct dbg_rec *r)
{
    if (r && cfg_verbose >= 3) dbg_print_rec(r);
}

static void dbg_print_rec(const struct dbg_rec *r)
{
    int i;
    double drift;

    if (r->dir == 'R' && !r->ok) {
        fprintf(stderr, "  RX  ---- timeout, no start pulse\n");
        return;
    }
    if (r->dir == 'T') {
        fprintf(stderr, "  TX  $%02X  %d cells", r->byte, 9);
        if (r->overruns)
            fprintf(stderr, "   LATE on %u deadline%s, worst %u us"
                            "  <-- preemption, check SCHED_FIFO / RT kernel",
                    r->overruns, r->overruns == 1 ? "" : "s", r->worst_late);
        fputc('\n', stderr);
        return;
    }

    fprintf(stderr, "  RX  $%02X  t0=%u  bits(LSB first)", r->byte, r->t0);
    for (i = 0; i < 8; i++) fprintf(stderr, " %d", (r->byte >> i) & 1);
    fputc('\n', stderr);
    for (i = 0; i < r->nedge; i++) {
        fprintf(stderr, "        pulse %2d  +%6.1f us", i + 1, (double)r->dt[i]);
        if (r->cell[i] >= 1 && r->cell[i] <= 8)
            fprintf(stderr, "  cell %d  err %+6.1f us\n",
                    r->cell[i], r->err[i] / 10.0);
        else
            fprintf(stderr, "  OUT OF RANGE (cell %d) -- ignored\n", r->cell[i]);
    }
    if (r->lost) fprintf(stderr, "        (+%d more pulses not recorded)\n", r->lost);
    if (r->nedge && r->cell[r->nedge - 1] >= 1 && r->cell[r->nedge - 1] <= 8) {
        drift = (double)r->err[r->nedge - 1] / 10.0;
        fprintf(stderr, "        span %u us over %d cells, drift %+.1f us (%+.2f %%)\n",
                r->span, r->cell[r->nedge - 1], drift,
                100.0 * drift / (g_cell_us * r->cell[r->nedge - 1]));
    }
}

static void dbg_flush(void)
{
    int i, start;
    if (cfg_verbose < 2 || dbg_count == 0) return;
    if (cfg_verbose >= 3) { dbg_count = 0; return; }   /* already printed live */
    start = (dbg_head - dbg_count + DBG_RING) % DBG_RING;
    fprintf(stderr, "\n--- last %d byte%s on the wire ---\n",
            dbg_count, dbg_count == 1 ? "" : "s");
    for (i = 0; i < dbg_count; i++)
        dbg_print_rec(&dbg_ring[(start + i) % DBG_RING]);
    dbg_count = 0;
}

static void dbg_stats(void)
{
    if (cfg_verbose < 1) return;
    fprintf(stderr,
        "\n--- link statistics ---\n"
        "  bytes out            %lu\n"
        "  bytes in             %lu\n"
        "  receive timeouts     %lu\n"
        "  bad checksums        %lu\n"
        "  handshake retries    %lu\n"
        "  transmit deadlines missed %lu (worst %lu us late)\n"
        "  worst pulse position error %.1f us  (tolerance is about +-40 us)\n",
        g_stats.tx, g_stats.rx, g_stats.timeouts, g_stats.badck,
        g_stats.resync, g_stats.overruns, g_stats.worst_late, g_stats.worst_err);
    if (g_stats.overruns)
        fprintf(stderr,
        "  NOTE: missed transmit deadlines mean the process was preempted\n"
        "        mid-byte. Raise --rt-prio, or boot the PREEMPT_RT kernel.\n");
}

/* record one received pulse; called from inside the sampling loops */
static void dbg_edge(struct dbg_rec *r, uint32_t t, double cell_us)
{
    double dt, ideal;
    int k;
    if (!r) return;
    dt = (double)(int32_t)(t - r->t0);
    k  = (int)(dt / cell_us + 0.5);
    ideal = cell_us * k;
    if (r->nedge < DBG_EDGES) {
        r->dt[r->nedge]   = (uint16_t)(dt < 0 ? 0 : dt);
        r->cell[r->nedge] = (int8_t)k;
        r->err[r->nedge]  = (int16_t)((dt - ideal) * 10.0);
        r->nedge++;
    } else {
        r->lost++;
    }
    if (k >= 1 && k <= 8) {
        double e = dt - ideal;
        if (e < 0) e = -e;
        if (e > g_stats.worst_err) g_stats.worst_err = e;
    }
    r->span = (uint16_t)(dt < 0 ? 0 : dt);
}

/* ------------------------------------------------------------------ */
/* Backend: real-time scheduling                                       */
/* ------------------------------------------------------------------ */

#ifdef USE_SHM
static int cfg_rt_prio = 0;    /* no console deadline: RT off vs. an emulator */
#else
static int cfg_rt_prio = 80;
#endif

static void go_realtime(void)
{
    struct sched_param sp;
    if (cfg_rt_prio <= 0) return;
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        fprintf(stderr, "warning: mlockall failed (%s)\n", strerror(errno));
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = cfg_rt_prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        fprintf(stderr, "warning: SCHED_FIFO %d failed (%s) -- expect dropped bytes\n",
                cfg_rt_prio, strerror(errno));
}

#ifdef USE_PIGPIO
/* ------------------------------------------------------------------ */
/* Backend: pigpio — DMA waves out, alert-callback timestamps in       */
/* ------------------------------------------------------------------ */

#define RXQ 8192
static volatile uint32_t rq_tick[RXQ];
static volatile int      rq_head = 0;
static          int      rq_tail = 0;
static volatile int      rx_muted = 0;

static void rx_cb(int gpio, int level, uint32_t tick, void *user)
{
    (void)gpio; (void)user;
    if (rx_muted) return;
    if (level > 1) return;                       /* watchdog timeout event */
    if (level != (cfg_rx_invert ? 0 : 1)) return;/* keep "light on" edges  */
    {
        int h = rq_head;
        rq_tick[h] = tick;
        rq_head = (h + 1) & (RXQ - 1);
    }
}

static void rx_flush(void) { rq_tail = rq_head; }

static int rx_pop(uint32_t *tick)
{
    if (rq_tail == rq_head) return 0;
    *tick = rq_tick[rq_tail];
    rq_tail = (rq_tail + 1) & (RXQ - 1);
    return 1;
}

static int32_t tick_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static gpioPulse_t txbuf[MAX_TX_BYTES * 20 + 8];

static void push_pulse(int *np, int on, int us)
{
    uint32_t mask = 1u << cfg_tx_gpio;
    int lit = cfg_tx_invert ? !on : on;
    txbuf[*np].gpioOn  = lit ? mask : 0;
    txbuf[*np].gpioOff = lit ? 0 : mask;
    txbuf[*np].usDelay = (uint32_t)us;
    (*np)++;
}

static int tx_bytes(const uint8_t *buf, int n)
{
    int np = 0, i, k, id;

    if (n > MAX_TX_BYTES) { ERR("tx_bytes: %d bytes exceeds one wave\n", n); return -1; }

    for (i = 0; i < n; i++) {
        push_pulse(&np, 0, BYTE_GAP_US);
        for (k = 0; k < 9; k++) {                 /* cell 0 = start bit */
            int cell = (int)(CELL_US * (k + 1) + 0.5) - (int)(CELL_US * k + 0.5);
            int bit  = (k == 0) ? 0 : ((buf[i] >> (k - 1)) & 1);
            if (bit) {
                push_pulse(&np, 0, cell);
            } else {
                push_pulse(&np, 1, PULSE_US);
                push_pulse(&np, 0, cell - PULSE_US);
            }
        }
    }
    push_pulse(&np, 0, 20);

    gpioWaveAddNew();
    if (gpioWaveAddGeneric(np, txbuf) < 0) { ERR("gpioWaveAddGeneric failed\n"); return -1; }
    id = gpioWaveCreate();
    if (id < 0) { ERR("gpioWaveCreate failed (%d pulses)\n", np); return -1; }

    rx_muted = 1;                                 /* ignore our own light */
    gpioWaveTxSend(id, PI_WAVE_MODE_ONE_SHOT);
    while (gpioWaveTxBusy()) gpioDelay(100);
    gpioDelay(60);
    rx_flush();
    rx_muted = 0;
    gpioWaveDelete(id);
    for (i = 0; i < n; i++) {                     /* DMA: no deadline to miss */
        struct dbg_rec *r = dbg_push('T');
        if (r) r->byte = buf[i];
        g_stats.tx++;
        dbg_maybe_now(r);
    }
    return 0;
}

static int recv_byte(uint32_t timeout_us)
{
    uint32_t t0, t, start = gpioTick();
    struct dbg_rec *r = dbg_push('R');
    uint8_t b;
    uint32_t end;

    for (;;) {
        if (rx_pop(&t0)) break;
        if ((uint32_t)tick_diff(gpioTick(), start) > timeout_us) {
            g_stats.timeouts++;
            dbg_maybe_now(r);
            return -1;
        }
        gpioDelay(5);
    }
    if (r) { r->ok = 1; r->t0 = t0; }

    b = 0xFF;
    end = t0 + (uint32_t)(CELL_US * 8.6);
    for (;;) {
        if (rx_pop(&t)) {
            int32_t dt = tick_diff(t, t0);
            if (dt > 0) {
                int k = (int)(dt / CELL_US + 0.5);
                if (k >= 1 && k <= 8) b &= (uint8_t)~(1u << (k - 1));
                dbg_edge(r, t, CELL_US);
            }
            continue;
        }
        if (tick_diff(gpioTick(), end) > 0) break;
        gpioDelay(5);
    }
    if (r) r->byte = b;
    g_stats.rx++;
    dbg_maybe_now(r);
    return b;
}

static void gp_write(int on)
{
    gpioWrite(cfg_tx_gpio, (cfg_tx_invert ? !on : on) ? 1 : 0);
}

static int gp_read(void)
{
    int v = gpioRead(cfg_rx_gpio) ? 1 : 0;
    return cfg_rx_invert ? !v : v;
}

static int hw_init(void)
{
    gpioCfgClock(1, PI_CLOCK_PCM, 0);             /* 1 us sampling, PWM left free */
    if (gpioInitialise() < 0) { ERR("gpioInitialise failed (run as root?)\n"); return -1; }
    gpioSetMode(cfg_tx_gpio, PI_OUTPUT);
    gpioWrite(cfg_tx_gpio, cfg_tx_invert ? 1 : 0);
    gpioSetMode(cfg_rx_gpio, PI_INPUT);
    gpioSetPullUpDown(cfg_rx_gpio, cfg_rx_pull == 2 ? PI_PUD_UP :
                                   cfg_rx_pull == 1 ? PI_PUD_DOWN : PI_PUD_OFF);
    gpioSetAlertFuncEx(cfg_rx_gpio, rx_cb, NULL);
    rx_flush();
    go_realtime();
    return 0;
}

static uint32_t hw_now(void) { return gpioTick(); }
static void hw_close(void) { gpioTerminate(); }

#elif defined(USE_WIRINGPI)
/* ------------------------------------------------------------------ */
/* Backend: wiringPi — the library the Mystery Gift tool already uses  */
/* ------------------------------------------------------------------ */
/*
 * wiringPiSetupGpio() means the pin numbers here are BCM numbers, the same as
 * for the other two backends. Timing is busy-wait on micros(), exactly as in
 * the proven Gen 2 code.
 */

static void gp_write(int on)
{
    digitalWrite(cfg_tx_gpio, (cfg_tx_invert ? !on : on) ? HIGH : LOW);
}

static int gp_read(void)
{
    int v = digitalRead(cfg_rx_gpio) == HIGH ? 1 : 0;
    return cfg_rx_invert ? !v : v;
}

static uint32_t now_us(void) { return (uint32_t)micros(); }

/* Returns how many microseconds late we arrived. Non-zero means the scheduler
 * took the CPU away while a bit cell was running -- the preemption smoking gun. */
static int wait_until(uint32_t t)
{
    int32_t late = (int32_t)(now_us() - t);
    if (late > 0) { g_stats.overruns++; return (int)late; }
    while ((int32_t)(now_us() - t) < 0) { }
    return 0;
}

static int tx_bytes(const uint8_t *buf, int n)
{
    int i, k, late;
    uint32_t t0;

    for (i = 0; i < n; i++) {
        struct dbg_rec *r = dbg_push('T');
        if (r) r->byte = buf[i];
        gp_write(0);
        t0 = now_us() + BYTE_GAP_US;
        wait_until(t0);
        for (k = 0; k < 9; k++) {                 /* cell 0 = start bit */
            uint32_t cs = t0 + (uint32_t)(CELL_US * k + 0.5);
            int bit = (k == 0) ? 0 : ((buf[i] >> (k - 1)) & 1);
            late = wait_until(cs);
            if (late && r) {
                r->overruns++;
                if (late > (int)r->worst_late) r->worst_late = (uint16_t)late;
                if ((unsigned long)late > g_stats.worst_late)
                    g_stats.worst_late = (unsigned long)late;
            }
            if (!bit) {
                gp_write(1);
                wait_until(cs + PULSE_US);
                gp_write(0);
            }
        }
        wait_until(t0 + (uint32_t)(CELL_US * 9 + 0.5));
        g_stats.tx++;
        dbg_maybe_now(r);
    }
    gp_write(0);
    return 0;
}

static int recv_byte(uint32_t timeout_us)
{
    uint32_t start = now_us(), t0, t;
    struct dbg_rec *r = dbg_push('R');
    uint8_t b;
    int prev;

    /* the line must be dark first, so we catch an edge and not a leftover */
    while (gp_read())
        if ((uint32_t)(now_us() - start) > timeout_us) goto timeout;
    while (!gp_read())
        if ((uint32_t)(now_us() - start) > timeout_us) goto timeout;

    t0 = now_us();
    if (r) { r->ok = 1; r->t0 = t0; }
    b = 0xFF;
    prev = 1;
    for (;;) {
        int lvl = gp_read();
        t = now_us();
        if (lvl && !prev) {                       /* rising edge = a pulse */
            int32_t dt = (int32_t)(t - t0);
            int k = (int)(dt / CELL_US + 0.5);
            if (k >= 1 && k <= 8) b &= (uint8_t)~(1u << (k - 1));
            dbg_edge(r, t, CELL_US);
        }
        prev = lvl;
        if ((int32_t)(t - t0) > (int32_t)(CELL_US * 8.6 + 0.5)) break;
    }
    if (r) r->byte = b;
    g_stats.rx++;
    dbg_maybe_now(r);
    return b;

timeout:
    g_stats.timeouts++;
    dbg_maybe_now(r);
    return -1;
}

static int hw_init(void)
{
    if (wiringPiSetupGpio() == -1) { ERR("wiringPiSetupGpio failed (run as root?)\n"); return -1; }
    pinMode(cfg_tx_gpio, OUTPUT);
    gp_write(0);
    pinMode(cfg_rx_gpio, INPUT);
    pullUpDnControl(cfg_rx_gpio,
                    cfg_rx_pull == 2 ? PUD_UP : cfg_rx_pull == 1 ? PUD_DOWN : PUD_OFF);
    go_realtime();
    return 0;
}

static uint32_t hw_now(void) { return now_us(); }
static void hw_close(void) { gp_write(0); }

#elif defined(USE_SHM)
/* ------------------------------------------------------------------ */
/* Backend: shared memory — talk to a patched VBA instead of an LED    */
/* ------------------------------------------------------------------ */
/*
 * No hardware, no LED, no photodiode: our "line" is the SPSC ring in
 * shmring.c, wire-compatible with the VBA fork's gbIrShm. gp_write publishes
 * an LED transition; gp_read drains the peer's transitions and returns its
 * current LED. The clock is CLOCK_MONOTONIC in us, exactly the trick gbIRsim
 * uses, so a 26 us pulse we emit reads back as ~27 M-cycles inside the game.
 *
 * Polarity here is direct: the peer's led_on == 1 means it is emitting light,
 * which is "light present" == 1 for the bit layer, so cfg_rx_invert / the pull
 * settings are irrelevant on this backend.
 */

static struct ShmWriter g_shm_wr;
static struct ShmReader g_shm_rd;
static int g_shm_peer_on = 0;      /* last known peer LED state (1 = light) */
static int g_shm_own_on  = 0;      /* last state we published (dedupe)      */
static struct ShmSlot g_peek;      /* one-slot RX pushback (see recv_byte)  */
static int g_have_peek = 0;
static long long g_tx_lead = -1;   /* one-shot tx_bytes lead override; -1 = use MC_LEAD (see REPLY_LEAD) */

/* The protocol runs on the game's own clock, not ours. VBA batches emulation
 * ~1 byte at a time and only sleeps once per frame, so wall-clock pulse timing
 * collapses. Instead we read the game's live M-cycle counter from the ring
 * header and schedule every edge at a future target in *that* clock; VBA holds
 * each edge until its cT reaches the target. A 440 T cell is then 440 T no
 * matter how VBA paces wall time. Received bytes are decoded the same way, from
 * the cT stamp on each of the game's own transitions. */
#define MC2US        (1000000.0 / 1048576.0)   /* one M-cycle in microseconds */
/*
 * These are VBA-1.8.0's *emulated* IR timings, which differ from the hardware
 * figures the GPIO backends use (110 / 27). Measured from the game's own Card
 * Pop! transmission in the shm bridge, VBA runs the ROM's IR loops ~18% short:
 * a 440 T bit cell comes out at ~90 M-cycles and the 108 T pulse at ~23. We
 * both transmit and decode on these so the two ends agree with the emulator.
 */
#define MC_PER_CELL  92.0                       /* VBA's emulated 440 T cell (measured) */
/*
 * On transmit we deliberately drive a *wider* pulse than the game's own 23:
 * VBA applies our scheduled edges at the game's rP1-poll instants, adding up to
 * ~a poll of jitter, and the game reads a cell as 0 if *any* of its 10 samples
 * sees light. A fat pulse (~half the cell) survives that jitter and the ~90-vs-
 * 110 timing slop, which is what keeps the game from mis-decoding a byte and
 * aborting its IR_ServeLoop. Decode ignores width (it bins pulse *edges*), so
 * this only affects what we send.
 */
#define MC_PER_PULSE 44                         /* fat TX pulse (~half a cell) */
#define MC_LEAD      1000                       /* schedule this far ahead: >1
                                                   byte, so after a role reversal
                                                   the peer is already listening
                                                   before our start pulse        */
/*
 * MC_LEAD's ">1 byte" margin exists for a peer that might not yet be watching
 * for us (a genuine role reversal). An immediate reply to something we JUST
 * received (the $33 sync ack, a write's checksum ack) has a different budget:
 * the game starts IR_RecvByteOr0 right after its own byte and polls for
 * ~2200 mc, so our start pulse must land within ~t0+850 .. ~t0+2900 (t0 = the
 * game's start pulse; measured: $AA retry cadence 3058 mc with no answer).
 * We read live_cT at ~t0+870, so the lead must stay below ~1900.
 *
 * It must ALSO cover gbcpop's own publish latency measured in the GAME's
 * clock: VBA emulates in bursts far faster than real time, and any edge whose
 * target cT has already passed when VBA drains the ring fires immediately --
 * a late byte collapses into zero-width pulses at one instant and the game
 * misdecodes it. Real-VBA log (serve, REPLY_LEAD was 200): the $33 edges
 * arrived 544 and 152 mc late, the start pulse vanished, and the game retried
 * $AA early (after decoding garbage) until IR_TryMaster's 4 attempts ran out.
 * 1200 sits roughly midway: ~650 mc above the worst lateness seen, ~800 mc
 * below the game's receive deadline. (An earlier theory that MC_LEAD 1000 was
 * "too late" for the game was wrong -- 1000 is well inside the window.)
 */
#define REPLY_LEAD   1200
/* Arm REPLY_LEAD for the next tx_bytes call only (an immediate reply). */
static void tx_reply_next(void) { g_tx_lead = REPLY_LEAD; }
/*
 * VBA maps rP1 bit 0 the opposite way to real hardware / the GPIO backends:
 * the ROM writes $C1 (bit0=1) to light the LED for a pulse, but VBA treats
 * bit0=1 as LED-off, so it publishes the game's real pulses as led_on=0 and the
 * idle gaps as led_on=1. On receive we therefore read a pulse as led_on==0.
 * (Transmit is unaffected: VBA's *read* path is correct, so our led_on=1 pulses
 * are seen by the game as light.)
 */
#define SHM_RX_LIGHT(led_on)  (!(led_on))

static void shm_try_peer(void)
{
    if (g_shm_rd.shm) return;
    shm_open_peer(&g_shm_rd, cfg_shm_peer);   /* silent, retry every access */
}

static uint32_t now_us(void)                  /* wall clock, only for timeouts */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL +
                      (uint64_t)ts.tv_nsec / 1000);
}

/* gp_write / gp_read stay real-time (sched = 0, applied on arrival) and exist
 * only for the hardware-diagnostic commands (selftest, trace); the protocol
 * path uses the cT-scheduled tx_bytes / recv_byte below. */
static void gp_write(int on)
{
    if (on == g_shm_own_on) return;
    g_shm_own_on = on;
    shm_publish(&g_shm_wr, (long long)now_us(), on, 0);
}

static int gp_read(void)
{
    struct ShmSlot s;
    shm_try_peer();
    while (shm_read_next(&g_shm_rd, &s))
        g_shm_peer_on = SHM_RX_LIGHT(s.led_on) ? 1 : 0;
    return g_shm_peer_on;
}

/*
 * One byte = 9 cells (start + 8 data). Bytes MUST be transmitted back-to-back
 * at a continuous 92-cycle cadence: the game's receiver (and our own recv_byte,
 * whose byte window is 8.7 cells) expects the next byte's start pulse ~9 cells
 * after the last. So we anchor the whole block ONCE, apply MC_LEAD only to the
 * first byte (to clear a role reversal), and schedule every following byte at a
 * fixed 9-cell stride from that anchor. The earlier code re-read the game's
 * live cT and re-added MC_LEAD per byte, inserting a ~1000-cycle (~11 empty
 * cell) idle gap between bytes; single-byte syncs survived it but the first
 * multi-byte block (the "IR" magic / an 8-byte command) overran the game's
 * inter-byte timeout and it aborted IR_ServeLoop without turning around.
 */
/*
 * Byte stride = start-pulse to start-pulse. Per protocol §2.2 the frame is
 * 8 cells from the start pulse to b7's start (8 x 440 T) PLUS a 1132 T
 * inter-byte idle before the next start pulse (= 4652 T total). The receiver
 * REQUIRES that idle: IR_RecvByte waits for a start pulse and needs >= ~1 cell
 * of silence before it (§2 "Idle before start pulse >= 98 us"). A gapless
 * 9-cell stride left b7 butting straight into the next start pulse, so every
 * byte after the first misframed and the game aborted IR_ServeLoop — which is
 * why single-byte handshakes worked but the first multi-byte block never did.
 * (vbastub doesn't model this idle requirement, hence it passed while VBA failed.)
 */
#define T_PER_CELL   440.0
#define T_PER_BYTE   4652.0                                /* §2.2: 8 cells + 1132 T idle */
#define MC_PER_BYTE  (long long)(MC_PER_CELL * (T_PER_BYTE / T_PER_CELL) + 0.5)  /* ~973 */

#define TX_AHEAD_BYTES  8          /* max bytes published ahead of the game's clock */
#define TX_STALL_US     100000u    /* no live_cT progress this long = peer gone */

static int tx_bytes(const uint8_t *buf, int n)
{
    int i, k;
    long long base, end;
    uint32_t wstart;

    /*
     * g_peek is sourced from the PEER's ring (g_shm_rd); publishing our own
     * edges to g_shm_wr never touches it, so transmitting does not make it
     * stale, and it must not be cleared here. Clearing it would be wrong
     * right after sync_as_receiver: it decodes the peer's $AA via recv_byte,
     * which -- if the peer sends its next byte densely, with no gap (e.g. the
     * game's $AA immediately followed by "IR" + a command, as opposed to the
     * master-read turnaround's brief pause before the peer starts sending
     * data) -- may already have seen and pushed back that next byte's real
     * start pulse before we get here. sync_as_receiver's send_byte($33) reply
     * calls tx_bytes immediately after, and clearing g_have_peek here would
     * throw that legitimate start pulse away: the following recv_byte (magic
     * byte 1) would lock onto random noise instead, and the whole command
     * would shift one byte late (`recv_command: bad magic $C9 $49` -- $49 is
     * exactly the real first magic byte, landing in the *second* slot).
     * Timing-dependent: probe's turnaround never shows it (nothing pending
     * yet there), every serve command does (the game sends $AA+magic+packet
     * as one dense burst). The only real staleness risk is the peer's writer
     * restarting mid-session between the push-back and the next use -- rare
     * (each gbcpop invocation is a fresh process against a freshly-launched
     * peer).
     */

    {
        long long lead = (g_tx_lead >= 0) ? g_tx_lead : MC_LEAD;
        g_tx_lead = -1;                    /* one-shot: back to MC_LEAD next call */

        shm_try_peer();
        base = shm_peer_live_cT(&g_shm_rd);   /* the game's clock, once */
        LOG("tx_bytes n=%d live_cT=%lld base=%lld (lead %lld)\n",
            n, base, base < 0 ? 0 : base + lead, lead);
        if (base < 0) base = 0;               /* peer not up: retries recover */
        base += lead;                         /* lead the first byte only */
    }

    /*
     * Stream, don't dump: publish byte i only once the game's clock is within
     * TX_AHEAD_BYTES of it. Each byte is up to 18 edges ($00 = 9 pulses), and
     * both the shm ring (1024 slots) and VBA's pending-edge queue (512) drop
     * the OLDEST entries when full -- publishing a 257-byte read response in
     * one go (~4600 edges) lost the start of the block and the game aborted
     * after ~17 bytes. 8 bytes ahead = <= 144 edges outstanding, while still
     * ~7800 mc of lead, far above the ~550 mc publish lateness seen on VBA.
     *
     * Stall detection is progress-based: give up only if the game's clock
     * hasn't moved for TX_STALL_US of wall time. (A fixed wall budget fired
     * spuriously whenever VBA slept between frame bursts.)
     */
    wstart = now_us();
    {
        long long last_live = shm_peer_live_cT(&g_shm_rd);
        i = 0;
        end = base + (long long)n * MC_PER_BYTE;
        for (;;) {
            long long live = shm_peer_live_cT(&g_shm_rd);
            if (live != last_live) { last_live = live; wstart = now_us(); }

            while (i < n && live >= base + (long long)(i - TX_AHEAD_BYTES) * MC_PER_BYTE) {
                struct dbg_rec *r = dbg_push('T');
                long long bbase = base + (long long)i * MC_PER_BYTE;

                if (r) r->byte = buf[i];

                for (k = 0; k < 9; k++) {             /* cell 0 = start bit */
                    long long cs = bbase + (long long)(MC_PER_CELL * k + 0.5);
                    int bit = (k == 0) ? 0 : ((buf[i] >> (k - 1)) & 1);
                    if (!bit) {                       /* a pulse marks a logic 0 */
                        shm_publish(&g_shm_wr, cs, 1, 1);
                        shm_publish(&g_shm_wr, cs + MC_PER_PULSE, 0, 1);
                    }
                }
                g_stats.tx++;
                dbg_maybe_now(r);
                i++;
            }
            /* pace: wait until the game's clock has run past the whole block,
             * so the caller's role reversal lines up with the game's. */
            if (i >= n && live >= end) break;
            if ((uint32_t)(now_us() - wstart) > TX_STALL_US) {
                g_stats.overruns++;
                LOG("tx_bytes: peer clock STALLED at byte %d/%d, live_cT=%lld end=%lld (short %lld)\n",
                    i, n, live, end, end - live);
                break;
            }
        }
    }
    g_shm_own_on = 0;
    return 0;
}

/* One-slot pushback so a byte's decode loop can peek the *next* byte's start
 * pulse without eating it — otherwise every byte after the first loses its
 * start edge and the whole block cascades out of frame. (g_peek/g_have_peek
 * are declared up with the other shm statics so tx_bytes can clear them.) */
static int shm_next(struct ShmSlot *s)
{
    if (g_have_peek) { *s = g_peek; g_have_peek = 0; return 1; }
    return shm_read_next(&g_shm_rd, s);
}
static void shm_unread(const struct ShmSlot *s) { g_peek = *s; g_have_peek = 1; }

/*
 * Timeouts are measured on the GAME's clock (live cT), not wall time. VBA
 * emulates a frame in a fast burst and then sleeps for the rest of the ~16 ms,
 * so a byte's edges can straddle a sleep: the start pulse arrives, then nothing
 * for many wall-milliseconds, then the rest. A 2190 us wall timeout expired
 * mid-byte there (real VBA serve: the game sent a perfect `80 02 EB C5 EF ..`
 * packet; gbcpop cut `$EF` short to `$FF`, took its data pulse for the next
 * start and fell out of frame -> `recv_block: timeout at byte 5/8`). Wall time
 * remains only as a generous fallback for a peer that stops advancing live_cT
 * (paused, crashed, or the game not polling IR at all).
 */
#define RX_WALL_MIN_US  100000u

static int recv_byte(uint32_t timeout_us)
{
    uint32_t start = now_us();
    uint32_t wall_limit = timeout_us > RX_WALL_MIN_US ? timeout_us : RX_WALL_MIN_US;
    struct dbg_rec *r = dbg_push('R');
    struct ShmSlot s;
    long long t0 = -1, win = (long long)(MC_PER_CELL * 8.7 + 0.5);
    long long live0, tmo_mc = (long long)(timeout_us / MC2US + 0.5);
    uint8_t b = 0xFF;

    shm_try_peer();
    live0 = shm_peer_live_cT(&g_shm_rd);

    /* wait for the start pulse: the game's first rising (light-on) edge */
    for (;;) {
        long long live = shm_peer_live_cT(&g_shm_rd);   /* read BEFORE draining: VBA
                                                          sets live before publishing */
        if (shm_next(&s)) {
            if (SHM_RX_LIGHT(s.led_on)) { t0 = (long long)s.cT; break; }
            continue;
        }
        if ((live0 >= 0 && live - live0 > tmo_mc) ||
            (uint32_t)(now_us() - start) > wall_limit) {
            g_stats.timeouts++;
            dbg_maybe_now(r);
            return -1;
        }
    }
    if (r) { r->ok = 1; r->t0 = (uint32_t)(t0 * MC2US + 0.5); }

    for (;;) {
        if (shm_next(&s)) {
            long long dt = (long long)s.cT - t0;
            if (SHM_RX_LIGHT(s.led_on)) {              /* a pulse = a 0 in that cell */
                int kk = (int)((double)dt / MC_PER_CELL + 0.5);
                if (kk >= 1 && kk <= 8) {
                    b &= (uint8_t)~(1u << (kk - 1));
                    dbg_edge(r, (uint32_t)((long long)s.cT * MC2US + 0.5), g_cell_us);
                    continue;
                }
                /*
                 * Not one of this byte's 8 data cells: it's the next byte's
                 * real start pulse (or, if kk<=0, a spurious edge either way
                 * not ours to keep) -- push it back and end this byte now.
                 * Pushing back only when dt > win (8.7 cells) is not enough:
                 * the game's measured byte period varies (866-1038 mc), so a
                 * next-byte start landing in the ~782-800 mc "dead zone"
                 * rounds to kk==9 but still has dt <= win. Dropped as noise,
                 * it would eat the next byte's start pulse and cause an
                 * unrecoverable timeout one byte later (`recv_block: timeout
                 * at byte N`, no checksum error, since the lost byte was
                 * never even attempted).
                 */
                shm_unread(&s);
                break;
            }
            if (dt > win) break;                       /* stale idle edge past the byte: done */
            continue;
        }
        {
            long long live = shm_peer_live_cT(&g_shm_rd);
            if (live >= 0 && live - t0 > win) break;   /* byte window elapsed */
        }
        if ((uint32_t)(now_us() - start) > wall_limit) break;   /* peer stalled */
    }
    if (r) r->byte = b;
    g_stats.rx++;
    dbg_maybe_now(r);
    return b;
}

static int hw_init(void)
{
    if (shm_open_local(&g_shm_wr, cfg_shm_local) < 0) {
        ERR("cannot create local ring %s\n", cfg_shm_local);
        return -1;
    }
    g_shm_own_on = 0;
    g_have_peek = 0;
    g_cell_us = MC_PER_CELL * MC2US;      /* diagnostics bin on VBA's emulated cell */
    shm_try_peer();
    go_realtime();                       /* no-op unless --rt-prio was given */
    if (cfg_verbose)
        fprintf(stderr, "  shm: local=%s peer=%s%s\n", cfg_shm_local,
                cfg_shm_peer, g_shm_rd.shm ? "" : " (peer not up yet)");
    return 0;
}

static uint32_t hw_now(void) { return now_us(); }

static void hw_close(void)
{
    shm_close_reader(&g_shm_rd);
    shm_close_writer(&g_shm_wr);
}
#else
/* ------------------------------------------------------------------ */
/* Backend: direct /dev/mem — no libraries, works on ancient Raspbian  */
/* ------------------------------------------------------------------ */
/*
 * Timing comes from the BCM283x free-running system timer at
 * PERI_BASE + $3000: a 1 MHz counter the CPU cannot skew. Reading it is a
 * single load, so both the transmit deadlines and the receive timestamps are
 * exact even though the loop itself is plain userspace polling.
 *
 * What is NOT immune is preemption: if the scheduler takes the CPU away in the
 * middle of a byte, that byte is lost. SCHED_FIFO plus a PREEMPT_RT kernel is
 * what makes this reliable -- on a stock kernel, network traffic alone is
 * enough to corrupt bytes.
 *
 * Pi 1/2/3/Zero only. The Pi 5's RP1 has a different GPIO block; build with
 * PIGPIO=1 there, or use the Pi 5 PIO.
 */

#define GPFSEL0  0
#define GPSET0   7
#define GPCLR0   10
#define GPLEV0   13
#define GPPUD    37
#define GPPUDCLK0 38
#define ST_CLO   1

static volatile uint32_t *g_gpio;
static volatile uint32_t *g_st;
static int   g_memfd = -1;
static unsigned long cfg_peri_base = 0;           /* 0 = autodetect */

/* ARM physical base of the peripherals, from the device tree when possible */
static unsigned long detect_peri_base(void)
{
    unsigned char r[16];
    int fd, n;
    unsigned long base;

    if (cfg_peri_base) return cfg_peri_base;

    fd = open("/proc/device-tree/soc/ranges", O_RDONLY);
    if (fd >= 0) {
        n = (int)read(fd, r, sizeof r);
        close(fd);
        if (n >= 8) {
            base = ((unsigned long)r[4] << 24) | ((unsigned long)r[5] << 16) |
                   ((unsigned long)r[6] << 8)  |  (unsigned long)r[7];
            if (base == 0 && n >= 12)             /* Pi 4 style entry */
                base = ((unsigned long)r[8] << 24) | ((unsigned long)r[9] << 16) |
                       ((unsigned long)r[10] << 8) | (unsigned long)r[11];
            if (base) return base;
        }
    }
    /* No device tree (very old kernels): read the SoC out of /proc/cpuinfo. */
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        char line[256];
        if (f) {
            while (fgets(line, sizeof line, f)) {
                if (strstr(line, "BCM2711")) { fclose(f); return 0xFE000000UL; }
                if (strstr(line, "BCM2709") || strstr(line, "BCM2710") ||
                    strstr(line, "BCM2836") || strstr(line, "BCM2837")) {
                    fclose(f); return 0x3F000000UL;
                }
                if (strstr(line, "BCM2708") || strstr(line, "BCM2835")) {
                    fclose(f); return 0x20000000UL;
                }
            }
            fclose(f);
        }
    }
    return 0x3F000000UL;            /* Pi 2/3 is the likeliest unknown */
}

static volatile uint32_t *map_peri(unsigned long addr)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED,
                   g_memfd, (off_t)addr);
    return p == MAP_FAILED ? NULL : (volatile uint32_t *)p;
}

static void gp_mode_out(int pin)
{
    int reg = pin / 10, sh = (pin % 10) * 3;
    g_gpio[reg] = (g_gpio[reg] & ~(7u << sh)) | (1u << sh);
}

static void gp_mode_in(int pin)
{
    int reg = pin / 10, sh = (pin % 10) * 3;
    g_gpio[reg] &= ~(7u << sh);
}

/* BCM2835/6/7 pull sequence: value, 150 cycles, clock the pin, 150 cycles. */
static void gp_set_pull(int pin, int mode)
{
    volatile int i;
    g_gpio[GPPUD] = (uint32_t)mode;
    for (i = 0; i < 300; i++) { }
    g_gpio[GPPUDCLK0 + pin / 32] = 1u << (pin & 31);
    for (i = 0; i < 300; i++) { }
    g_gpio[GPPUD] = 0;
    g_gpio[GPPUDCLK0 + pin / 32] = 0;
}

static void gp_write(int on)
{
    int lit = cfg_tx_invert ? !on : on;
    uint32_t mask = 1u << (cfg_tx_gpio & 31);
    g_gpio[(lit ? GPSET0 : GPCLR0) + cfg_tx_gpio / 32] = mask;
}

static int gp_read(void)
{
    int v = (int)((g_gpio[GPLEV0 + cfg_rx_gpio / 32] >> (cfg_rx_gpio & 31)) & 1);
    return cfg_rx_invert ? !v : v;
}

/*
 * The BCM free-running 1 MHz system timer is a single load and cannot be
 * skewed by the scheduler. --clock mono falls back to CLOCK_MONOTONIC if the
 * peripheral mapping ever looks wrong.
 */
static int cfg_clock_st = 1;

static uint32_t now_us(void)
{
    if (cfg_clock_st) return g_st[ST_CLO];
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000);
    }
}

/* Returns how many microseconds late we arrived. Non-zero means the scheduler
 * took the CPU away while a bit cell was running -- the preemption smoking gun. */
static int wait_until(uint32_t t)
{
    int32_t late = (int32_t)(now_us() - t);
    if (late > 0) { g_stats.overruns++; return (int)late; }
    while ((int32_t)(now_us() - t) < 0) { }
    return 0;
}

static int tx_bytes(const uint8_t *buf, int n)
{
    int i, k, late;
    uint32_t t0;

    for (i = 0; i < n; i++) {
        struct dbg_rec *r = dbg_push('T');
        if (r) r->byte = buf[i];
        gp_write(0);
        t0 = now_us() + BYTE_GAP_US;
        wait_until(t0);
        for (k = 0; k < 9; k++) {                 /* cell 0 = start bit */
            uint32_t cs = t0 + (uint32_t)(CELL_US * k + 0.5);
            int bit = (k == 0) ? 0 : ((buf[i] >> (k - 1)) & 1);
            late = wait_until(cs);
            if (late && r) {
                r->overruns++;
                if (late > (int)r->worst_late) r->worst_late = (uint16_t)late;
                if ((unsigned long)late > g_stats.worst_late)
                    g_stats.worst_late = (unsigned long)late;
            }
            if (!bit) {
                gp_write(1);
                wait_until(cs + PULSE_US);
                gp_write(0);
            }
        }
        wait_until(t0 + (uint32_t)(CELL_US * 9 + 0.5));
        g_stats.tx++;
        dbg_maybe_now(r);
    }
    gp_write(0);
    return 0;
}

static int recv_byte(uint32_t timeout_us)
{
    uint32_t start = now_us(), t0, t;
    struct dbg_rec *r = dbg_push('R');
    uint8_t b;
    int prev;

    /* the line must be dark first, so we catch an edge and not a leftover */
    while (gp_read())
        if ((uint32_t)(now_us() - start) > timeout_us) goto timeout;
    while (!gp_read())
        if ((uint32_t)(now_us() - start) > timeout_us) goto timeout;

    t0 = now_us();
    if (r) { r->ok = 1; r->t0 = t0; }
    b = 0xFF;
    prev = 1;
    for (;;) {
        int lvl = gp_read();
        t = now_us();
        if (lvl && !prev) {                       /* rising edge = a pulse */
            int32_t dt = (int32_t)(t - t0);
            int k = (int)(dt / CELL_US + 0.5);
            if (k >= 1 && k <= 8) b &= (uint8_t)~(1u << (k - 1));
            dbg_edge(r, t, CELL_US);
        }
        prev = lvl;
        if ((int32_t)(t - t0) > (int32_t)(CELL_US * 8.6 + 0.5)) break;
    }
    if (r) r->byte = b;
    g_stats.rx++;
    dbg_maybe_now(r);
    return b;

timeout:
    g_stats.timeouts++;
    dbg_maybe_now(r);
    return -1;
}

static int hw_init(void)
{
    unsigned long base = detect_peri_base();

    g_memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_memfd < 0) {
        ERR("cannot open /dev/mem (%s) -- run as root\n", strerror(errno));
        return -1;
    }
    g_gpio = map_peri(base + 0x200000);
    g_st   = map_peri(base + 0x003000);
    if (!g_gpio || !g_st) {
        ERR("mmap of the peripherals at 0x%lx failed (%s); try --peri-base\n",
            base, strerror(errno));
        return -1;
    }
    if (cfg_verbose) fprintf(stderr, "  peripherals at 0x%lx\n", base);

    gp_mode_out(cfg_tx_gpio);
    gp_write(0);
    gp_mode_in(cfg_rx_gpio);
    gp_set_pull(cfg_rx_gpio, cfg_rx_pull);
    go_realtime();
    return 0;
}

static uint32_t hw_now(void) { return now_us(); }

static void hw_close(void)
{
    if (g_gpio) gp_write(0);
    if (g_memfd >= 0) close(g_memfd);
}

#endif /* USE_PIGPIO */

#ifndef USE_SHM
/* GPIO backends start every byte BYTE_GAP_US after the call, well inside the
 * peer's 2.19 ms reply window, so an immediate reply needs no special lead. */
static void tx_reply_next(void) { }
#endif

/* ------------------------------------------------------------------ */
/* Link layer                                                          */
/* ------------------------------------------------------------------ */

static int send_byte(uint8_t b) { return tx_bytes(&b, 1); }

/* Send $AA until $33 comes back. The peer must be in a receive loop. */
static int sync_as_sender(int attempts, uint32_t timeout_us)
{
    int i;
    for (i = 0; i < attempts; i++) {
        if (send_byte(0xAA) < 0) return -1;
        int r = recv_byte(timeout_us);
        if (r == 0x33) return 0;
        g_stats.resync++;
        LOG("sync_as_sender: attempt %d got %d\n", i + 1, r);
    }
    return -1;
}

/*
 * Wait for $AA, answer $33. The wait is chopped into 50 ms slices: under
 * SCHED_FIFO a single multi-second busy-poll would hold a core hostage.
 */
static int sync_as_receiver(uint32_t timeout_us)
{
    uint32_t left = timeout_us;
    for (;;) {
        uint32_t slice = left > 50000 ? 50000 : left;
        int r = recv_byte(slice);
        if (r == 0xAA) { tx_reply_next(); return send_byte(0x33); }
        if (r >= 0) { LOG("sync_as_receiver: saw $%02X\n", r); continue; }
        if (left <= slice) return -1;
        left -= slice;
    }
}

/* n bytes plus a byte that makes the whole thing sum to 0 mod 256. */
static int send_block(const uint8_t *buf, int n)
{
    uint8_t out[MAX_TX_BYTES];
    int i, sum = 0;
    if (n + 1 > MAX_TX_BYTES) { ERR("send_block: %d bytes too long\n", n); return -1; }
    for (i = 0; i < n; i++) { out[i] = buf[i]; sum += buf[i]; }
    out[n] = (uint8_t)(-sum);
    return tx_bytes(out, n + 1);
}

static int recv_block(uint8_t *buf, int n, uint32_t first_timeout_us)
{
    int i, sum = 0;
    for (i = 0; i < n; i++) {
        int r = recv_byte(i ? RX_BYTE_US : first_timeout_us);
        if (r < 0) { ERR("recv_block: timeout at byte %d/%d\n", i, n); return -1; }
        buf[i] = (uint8_t)r;
        sum += r;
    }
    int ck = recv_byte(RX_BYTE_US);
    if (ck < 0) { ERR("recv_block: checksum timeout\n"); return -1; }
    if (((sum + ck) & 0xFF) != 0) {
        g_stats.badck++;
        ERR("recv_block: bad checksum (payload sums to $%02X, trailer $%02X)\n",
            sum & 0xFF, ck);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* RPC layer — the 8-byte packet is the master's register file          */
/* ------------------------------------------------------------------ */

struct pkt { uint8_t f, a; uint16_t hl, de, bc; };

/*
 * The command is "IR" + an 8-byte register packet + a checksum byte (the
 * checksum covers only the 8 packet bytes, matching IR_RecvCommand, which reads
 * the magic then IR_RecvBlock's 8+1). These 11 bytes MUST go out as one
 * contiguous stream: sending the magic and the block as two tx_bytes calls
 * re-anchored each call at live_cT + MC_LEAD, opening a ~13-cell gap mid-command
 * that overran the game's inter-byte window. So assemble the whole stream and
 * transmit it once.
 *
 * A write (cmd 3) goes further: its data block (+ checksum) is appended to the
 * SAME stream, exactly like the game's own IR_RemoteWrite pushes the block
 * straight after the command. Sending it as a second tx_bytes call re-anchored
 * the data at live_cT + MC_LEAD; on real VBA gbcpop once took ~2100 mc to get
 * from reading live_cT to publishing, so the block's first byte arrived 1114 mc
 * late, collapsed, and the game aborted mid-write (no acknowledgement). Inside
 * one stream every byte after the first is published ~8 bytes ahead.
 */
static int send_command_data(uint8_t a, uint16_t hl, uint16_t de, uint16_t bc,
                             const uint8_t *data, int n)
{
    uint8_t s[11 + 256 + 1];                /* magic[2] + packet[8] + ck [+ data + ck] */
    int i, sum = 0, len = 11;
    if (n > 256) { ERR("send_command: %d data bytes too long\n", n); return -1; }
    if (sync_as_sender(8, RX_TURN_US) < 0) { ERR("send_command: no sync\n"); return -1; }
    s[0] = 0x49; s[1] = 0x52;                       /* "IR" */
    s[2] = 0;    s[3] = a;
    s[4] = hl & 0xFF; s[5] = hl >> 8;
    s[6] = de & 0xFF; s[7] = de >> 8;
    s[8] = bc & 0xFF; s[9] = bc >> 8;
    for (i = 2; i < 10; i++) sum += s[i];           /* checksum: packet bytes only */
    s[10] = (uint8_t)(-sum);
    if (n > 0) {
        for (i = 0, sum = 0; i < n; i++) { s[len++] = data[i]; sum += data[i]; }
        s[len++] = (uint8_t)(-sum);
    }
    LOG("cmd A=%u HL=$%04X DE=$%04X BC=$%04X (%d-byte stream)\n", a, hl, de, bc, len);
    return tx_bytes(s, len);
}

static int send_command(uint8_t a, uint16_t hl, uint16_t de, uint16_t bc)
{
    return send_command_data(a, hl, de, bc, NULL, 0);
}

static int recv_command(struct pkt *out, uint32_t timeout_us)
{
    uint8_t p[8];
    for (;;) {
        if (sync_as_receiver(timeout_us) < 0) return -1;
        int m1 = recv_byte(RX_BYTE_US), m2;
        /*
         * The peer re-sends $AA when it didn't accept our $33 (IR_TryMaster
         * does so up to 4 times). Answer that retry right here: treating it as
         * a bad magic byte and going back to sync_as_receiver would wait for
         * the *next* $AA and burn two of the peer's four attempts.
         */
        while (m1 == 0xAA) {
            LOG("recv_command: peer re-sent $AA, answering again\n");
            g_stats.resync++;
            tx_reply_next();
            if (send_byte(0x33) < 0) return -1;
            m1 = recv_byte(RX_BYTE_US);
        }
        m2 = recv_byte(RX_BYTE_US);
        if (m1 != 0x49 || m2 != 0x52) { LOG("recv_command: bad magic %d %d\n", m1, m2); continue; }
        if (recv_block(p, 8, RX_BYTE_US) < 0) continue;
        out->f  = p[0]; out->a = p[1];
        out->hl = (uint16_t)(p[2] | (p[3] << 8));
        out->de = (uint16_t)(p[4] | (p[5] << 8));
        out->bc = (uint16_t)(p[6] | (p[7] << 8));
        return 0;
    }
}

/* Read n bytes (n == 0 means 256) out of the peer's address space. */
static int remote_read(uint16_t peer_addr, uint8_t *dst, int n)
{
    int c = n & 0xFF;
    if (send_command(2, peer_addr, 0, (uint16_t)c) < 0) return -1;
#ifdef USE_SHM
    LOG("remote_read: awaiting turnaround $AA, live_cT=%lld\n",
        shm_peer_live_cT(&g_shm_rd));
#endif
    if (sync_as_receiver(RX_TURN_US) < 0) { ERR("remote_read: no sync\n"); return -1; }
    return recv_block(dst, n ? n : 256, RX_BYTE_US);
}

/* Write n bytes into the peer's address space and check its acknowledgement. */
static int remote_write(const uint8_t *src, uint16_t peer_addr, int n)
{
    int i, sum = 0;
    if (send_command_data(3, 0, peer_addr, (uint16_t)(n & 0xFF), src, n) < 0) return -1;
    for (i = 0; i < n; i++) sum += src[i];
    int ack = recv_byte(RX_BYTE_US);
    if (ack < 0) { ERR("remote_write: no acknowledgement\n"); return -1; }
    if (((ack + sum) & 0xFF) != 0) { ERR("remote_write: acknowledgement mismatch\n"); return -1; }
    return 0;
}

/* jp hl on the peer. A is forced to 4 by the command number itself. */
static int remote_call(uint16_t addr, uint16_t de, uint16_t bc)
{
    return send_command(4, addr, de, bc);
}

static int rpc_disconnect(void) { return send_command(0, 0, 0, 0); }

/* ------------------------------------------------------------------ */
/* Slave side — our own 64 KiB "address space" the peer may read/write  */
/* ------------------------------------------------------------------ */

static uint8_t mem[0x10000];

static void mem_init(uint8_t mode, const char *name)
{
    memset(mem, 0, sizeof mem);
    mem[0xC5EA] = 0xFF;                 /* session status: unfinished */
    mem[0xC5EB] = mode;                 /* 1 Card Pop!, 2 card, 3 deck */
    mem[0xC5EC] = 'P';
    mem[0xC5ED] = 'K';
    mem[0xC5EE] = '1';
    memset(mem + 0xC590, 0, 16);
    strncpy((char *)mem + 0xC590, name, 15);
}

/* Serve RPCs until the peer disconnects. Returns 0 on a clean disconnect. */
static int serve_loop(uint32_t first_timeout_us)
{
    struct pkt p;
    uint32_t to = first_timeout_us;
    for (;;) {
        if (recv_command(&p, to) < 0) { ERR("serve_loop: link lost\n"); return -1; }
        to = RX_TURN_US;
        switch (p.a) {
        case 0:
            LOG("served: disconnect\n");
            return 0;
        case 1:
            LOG("served: no-op\n");
            break;
        case 2: {                                  /* peer reads our memory */
            int n = p.bc & 0xFF; if (!n) n = 256;
            LOG("served: read $%04X x %d\n", p.hl, n);
            if (sync_as_sender(8, RX_TURN_US) < 0) return -1;
            if (send_block(mem + p.hl, n) < 0) return -1;
            break;
        }
        case 3: {                                  /* peer writes our memory */
            int n = p.bc & 0xFF; if (!n) n = 256;
            int i, sum = 0;
            LOG("served: write $%04X x %d\n", p.de, n);
            if (recv_block(mem + p.de, n, RX_BYTE_US) < 0) return -1;
            for (i = 0; i < n; i++) sum += mem[p.de + i];
            tx_reply_next();
            if (send_byte((uint8_t)(-sum)) < 0) return -1;
            break;
        }
        case 4:
            LOG("served: remote call $%04X (ignored, we are not a Game Boy)\n", p.hl);
            break;
        default:
            ERR("serve_loop: unknown command %u\n", p.a);
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Card Pop! card selection — 06:$5DFD, 06:$5E62 and home $089B/$088F   */
/* ------------------------------------------------------------------ */

struct rng { uint8_t r1, r2, r3; };

static uint8_t rng_next(struct rng *s)
{
    uint8_t r1 = s->r1, r2 = s->r2, r3 = s->r3;
    uint8_t c0 = (uint8_t)(((((r2 << 2) | (r2 >> 6)) & 0xFF) ^ r1) & 1);
    uint8_t d  = (uint8_t)(r2 ^ r1);
    uint8_t e  = (uint8_t)(r3 ^ r1);
    uint8_t c1 = (uint8_t)((e >> 7) & 1);
    e = (uint8_t)((e << 1) | c0);
    d = (uint8_t)((d << 1) | c1);
    s->r3 = (uint8_t)(r3 + 1);
    s->r2 = d;
    s->r1 = e;
    return (uint8_t)(d ^ e);
}

static int rng_random(struct rng *s, int n) { return (n * rng_next(s)) >> 8; }

#define VENUSAUR1 0x0A
#define MEW2      0xA1

static int pick_card(uint8_t d, uint8_t e)
{
    uint8_t list[80];
    int rar, n, i;

    if (e == 0x05) return (d & 1) ? MEW2 : VENUSAUR1;
    rar = (e < 0x40) ? 2 : (e < 0x9A ? 1 : 0);
    n = pool_len[rar];
    memcpy(list, pools[rar], (size_t)n);

    struct rng s = { d, e, 0 };
    for (i = 0; i < n; i++) {
        int j = rng_random(&s, n);
        uint8_t t = list[i]; list[i] = list[j]; list[j] = t;
    }
    return list[0];
}

static void name_digest(const uint8_t *b16, uint8_t *sum, uint8_t *xor_)
{
    int i; uint8_t s = 0, x = 0;
    for (i = 0; i < 16; i++) { s = (uint8_t)(s + b16[i]); x ^= b16[i]; }
    *sum = s; *xor_ = x;
}

/*
 * Build a 16-byte name field whose digest is exactly (want_sum, want_xor).
 * The game hashes all 16 bytes, terminator included, so the three bytes
 * after the printable prefix are free tuning space.
 *
 * Note the invariant every byte block obeys: bit 0 of the sum always equals
 * bit 0 of the xor, because a sum's bit 0 carries nothing in. So only targets
 * with (want_sum ^ want_xor) even are solvable at all.
 */
static int forge_name(uint8_t *out16, const char *prefix, uint8_t want_sum, uint8_t want_xor)
{
    int len = (int)strlen(prefix);
    int a, b;
    if (len > 12) len = 12;
    memset(out16, 0, 16);
    memcpy(out16, prefix, (size_t)len);

    for (a = 0; a < 256; a++) {
        for (b = 0; b < 256; b++) {
            uint8_t s, x, c;
            out16[13] = (uint8_t)a;
            out16[14] = (uint8_t)b;
            out16[15] = 0;
            name_digest(out16, &s, &x);
            c = (uint8_t)(want_sum - s);
            out16[15] = c;
            name_digest(out16, &s, &x);
            if (s == want_sum && x == want_xor) return 0;
        }
    }
    out16[13] = out16[14] = out16[15] = 0;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void hexdump(uint16_t base, const uint8_t *p, int n)
{
    int i, j;
    for (i = 0; i < n; i += 16) {
        printf("  %04X  ", base + i);
        for (j = 0; j < 16; j++) printf(j < n - i ? "%02X " : "   ", p[i + j]);
        printf(" |");
        for (j = 0; j < 16 && j < n - i; j++) {
            uint8_t c = p[i + j];
            putchar(c >= 0x20 && c < 0x7F ? c : '.');
        }
        printf("|\n");
    }
}

static const char *cardname(int id)
{
    return (id >= 1 && id <= NUM_CARDS) ? card_name[id] : "?";
}

static int check_header(const uint8_t *h, uint8_t want_mode)
{
    if (h[1] != 'P' || h[2] != 'K') { ERR("peer magic is not \"PK\" (%02X %02X)\n", h[1], h[2]); return -1; }
    if (h[3] != '1') { ERR("peer protocol version is $%02X, expected '1'\n", h[3]); return -1; }
    if (h[0] != want_mode) {
        ERR("peer is in mode %u, this command needs mode %u\n", h[0], want_mode);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static volatile int stop_flag = 0;
static long parse_num(const char *s);

/*
 * Decode a TCG name field into ASCII. Names are NOT ASCII: the game stores each
 * glyph as two bytes for its wide (full-width) font — a $03 font/high byte then
 * a code byte based at 'A' = $30 (so T=$43, I=$38, A=$30, S=$42), $00-terminated
 * (see src/constants/text_constants.asm + the text engine's a>=$10 full-width
 * path). We map the uppercase run we've confirmed; anything else is shown raw as
 * \xNN so an unmapped glyph is visible rather than silently wrong.
 */
static void tcg_decode_name(const uint8_t *nm, int len, char *out, int outsz)
{
    int i, o = 0;
    for (i = 0; i + 1 < len && o < outsz - 4; i += 2) {
        uint8_t hi = nm[i], lo = nm[i + 1];
        if (hi == 0 && lo == 0) break;                 /* $00 terminator */
        if (lo >= 0x30 && lo <= 0x49)                  /* A-Z */
            out[o++] = (char)('A' + (lo - 0x30));
        else
            o += sprintf(out + o, "\\x%02X", lo);
    }
    out[o] = 0;
}

static int cmd_probe(void)
{
    uint8_t h[4], nm[17];
    printf("Waiting for the Game Boy (it must be sitting in an IR screen)...\n");
    if (sync_as_sender(200, RX_LONG_US / 200) < 0) { ERR("no answer to $AA\n"); return 1; }
    printf("Handshake OK.\n");
    if (remote_read(0xC5EB, h, 4) < 0) return 1;
    printf("  header      %02X '%c' '%c' '%c'  -> mode %u (%s)\n",
           h[0], h[1], h[2], h[3], h[0],
           h[0] == 1 ? "Card Pop!" : h[0] == 2 ? "card transfer" :
           h[0] == 3 ? "deck configuration" : "unknown");
    if (remote_read(0xC590, nm, 16) < 0) return 1;
    nm[16] = 0;
    {
        char name[64];
        tcg_decode_name(nm, 16, name, sizeof name);
        printf("  player name  \"%s\"\n", name);
    }
    hexdump(0xC590, nm, 16);
    rpc_disconnect();
    return 0;
}

static int cmd_peek(uint16_t addr, int n)
{
    uint8_t buf[256];
    if (n < 1 || n > 256) { ERR("length must be 1..256\n"); return 1; }
    if (sync_as_sender(200, RX_LONG_US / 200) < 0) { ERR("no answer to $AA\n"); return 1; }
    if (remote_read(addr, buf, n) < 0) return 1;
    hexdump(addr, buf, n);
    rpc_disconnect();
    return 0;
}

static int cmd_poke(uint16_t addr, const uint8_t *data, int n)
{
    if (sync_as_sender(200, RX_LONG_US / 200) < 0) { ERR("no answer to $AA\n"); return 1; }
    if (remote_write(data, addr, n) < 0) return 1;
    printf("Wrote %d byte%s to $%04X.\n", n, n == 1 ? "" : "s", addr);
    rpc_disconnect();
    return 0;
}

/*
 * Hand the game a list of cards using its own "Receive a card" feature
 * (mode 2). On the Game Boy: Gift Center -> receive a card.
 * The payload at $C510 is a NUL-terminated list of up to 60 card IDs.
 */
static int cmd_give(const uint8_t *ids, int n)
{
    uint8_t h[4], peer[17], me[16], list[61], zero = 0;
    int i;

    if (n < 1 || n > 60) { ERR("give: 1..60 card ids\n"); return 1; }
    memset(list, 0, sizeof list);
    memcpy(list, ids, (size_t)n);

    memset(me, 0, sizeof me);
    strncpy((char *)me, cfg_name, 15);
    mem_init(2, cfg_name);

    printf("Waiting for the Game Boy in \"Receiving a card\"...\n");
    if (sync_as_sender(400, RX_LONG_US / 400) < 0) { ERR("no answer to $AA\n"); return 1; }
    printf("Handshake OK.\n");

    if (remote_read(0xC5EB, h, 4) < 0) return 1;
    if (check_header(h, 2) < 0) return 1;
    if (remote_read(0xC590, peer, 16) < 0) return 1;
    peer[16] = 0;
    printf("Peer is \"%s\".\n", peer);

    if (remote_write(me, 0xC500, 16) < 0) return 1;
    if (remote_write(list, 0xC510, 61) < 0) return 1;
    if (remote_write(&zero, 0xC5EA, 1) < 0) return 1;      /* status = OK */
    if (rpc_disconnect() < 0) return 1;

    /* The Game Boy now takes the master role to acknowledge with 'O'. */
    if (serve_loop(RX_TURN_US) < 0) return 1;
    if (mem[0xC5EC] != 0x4F) {
        ERR("no 'O' acknowledgement from the game (got $%02X)\n", mem[0xC5EC]);
        return 1;
    }

    printf("Accepted. Cards added to the collection:\n");
    for (i = 0; i < n; i++) printf("  $%02X  %s\n", ids[i], cardname(ids[i]));
    return 0;
}

/* Receive a card list from the game's "Send a card" (mode 2), as the slave. */
static int cmd_take(void)
{
    int i;
    mem_init(2, cfg_name);
    printf("Waiting for the Game Boy in \"Sending a card\" (press A there)...\n");
    if (sync_as_receiver(RX_LONG_US) < 0) { ERR("no $AA from the game\n"); return 1; }
    if (serve_loop(RX_TURN_US) < 0) return 1;
    if (mem[0xC5EA] != 0) { ERR("game reported status $%02X\n", mem[0xC5EA]); return 1; }

    printf("Received from \"%.16s\":\n", mem + 0xC500);
    for (i = 0; i < 60 && mem[0xC510 + i]; i++)
        printf("  $%02X  %s\n", mem[0xC510 + i], cardname(mem[0xC510 + i]));
    if (i == 0) printf("  (empty list)\n");

    /* Mirror 06:$5BF7: acknowledge with 'O' and disconnect. */
    mem[0xC5EC] = 0x4F;
    if (remote_write(mem + 0xC5EB, 0xC5EB, 4) < 0) return 1;
    rpc_disconnect();
    return 0;
}

/*
 * Card Pop! as the master. With want >= 0 we choose our own 16-byte name
 * so that the digest differences steer the game to that exact card.
 */
static int cmd_pop(int want)
{
    uint8_t h[4], peer[17], me[16], hist[256], zero = 0, dup = 0;
    uint8_t sum_g, xor_g, sum_m, xor_m, d, e;
    int found = 0;

    mem_init(1, cfg_name);
    memset(me, 0, sizeof me);
    strncpy((char *)me, cfg_name, 15);

    printf("Waiting for the Game Boy in Card Pop! ...\n");
    if (sync_as_sender(400, RX_LONG_US / 400) < 0) { ERR("no answer to $AA\n"); return 1; }
    printf("Handshake OK.\n");

    if (remote_read(0xC5EB, h, 4) < 0) return 1;
    if (check_header(h, 1) < 0) return 1;
    if (remote_read(0xC590, peer, 16) < 0) return 1;
    peer[16] = 0;
    name_digest(peer, &sum_g, &xor_g);
    printf("Peer is \"%s\"  (sum $%02X, xor $%02X)\n", peer, sum_g, xor_g);

    if (want >= 0) {
        int dd, ee;
        for (ee = 0; ee < 256 && !found; ee++)
            for (dd = (ee & 1); dd < 256; dd += 2)     /* parity-reachable only */
                if (pick_card((uint8_t)dd, (uint8_t)ee) == want) {
                    d = (uint8_t)dd; e = (uint8_t)ee; found = 1; break;
                }
        if (!found) { ERR("card $%02X (%s) is not reachable by Card Pop!\n", want, cardname(want)); return 1; }
        /* The game computes e = sum(its name) - sum(our name). */
        if (forge_name(me, cfg_name, (uint8_t)(sum_g - e), (uint8_t)(xor_g - d)) < 0) {
            ERR("could not forge a name for d=$%02X e=$%02X\n", d, e);
            return 1;
        }
        printf("Using a forged name to force card $%02X (%s).\n", want, cardname(want));
        hexdump(0xC590, me, 16);
    }

    name_digest(me, &sum_m, &xor_m);
    d = (uint8_t)(xor_g - xor_m);
    e = (uint8_t)(sum_g - sum_m);
    printf("The game will get d=$%02X e=$%02X -> card $%02X (%s)\n",
           d, e, pick_card(d, e), cardname(pick_card(d, e)));

    if (remote_write(me, 0xC500, 16) < 0) return 1;
    if (remote_read(0xC000, hist, 256) < 0) return 1;      /* the game ignores it too */
    if (remote_write(&dup, 0xC5F3, 1) < 0) return 1;       /* never "already popped" */
    if (remote_write(&zero, 0xC5EA, 1) < 0) return 1;
    if (rpc_disconnect() < 0) return 1;
    if (serve_loop(RX_TURN_US) < 0) return 1;

    printf("Done.\n");
    return 0;
}

static int cmd_serve(void)
{
    mem_init(1, cfg_name);
    if (cfg_verbose < 1) cfg_verbose = 1;   /* was an unconditional =1, clobbering -vv/-vvv */
    printf("Acting as a slave; logging everything the Game Boy asks for.\n");
    if (sync_as_receiver(RX_LONG_US) < 0) { ERR("no $AA\n"); return 1; }
    if (serve_loop(RX_TURN_US) < 0) return 1;
    printf("Peer disconnected. Notable buffers:\n");
    hexdump(0xC500, mem + 0xC500, 16);
    hexdump(0xC5EA, mem + 0xC5EA, 16);
    hexdump(0xC510, mem + 0xC510, 64);
    return 0;
}

static int cmd_monitor(void)
{
    printf("Raw byte monitor. Ctrl-C to stop.\n");
    while (!stop_flag) {
        int r = recv_byte(1000000);
        if (r >= 0) {
            printf("  $%02X  %c\n", r, (r >= 0x20 && r < 0x7F) ? r : '.');
            fflush(stdout);
        }
    }
    return 0;
}

/*
 * Arbitrary code execution demo, and a card injection route that works in
 * any of the three IR modes: write a six-byte stub into the peer's work RAM
 * and jump to it with RPC command 4.
 *
 *     3E xx        ld a, <card id>
 *     CD lo hi     call AddCardToCollection
 *     C9           ret            -> back into the peer's RPC dispatcher
 *
 * $C300 is scratch during an IR session in every mode; AddCardToCollection
 * lives in the home bank, so it is mapped whatever the current ROM bank is.
 */
static int cmd_homebrew(int card, uint16_t addcard, uint16_t stub_at)
{
    uint8_t stub[6];

    if (card < 1 || card > NUM_CARDS) { ERR("card id out of range\n"); return 1; }
    stub[0] = 0x3E; stub[1] = (uint8_t)card;
    stub[2] = 0xCD; stub[3] = addcard & 0xFF; stub[4] = addcard >> 8;
    stub[5] = 0xC9;

    printf("Waiting for the Game Boy in any IR screen...\n");
    if (sync_as_sender(400, RX_LONG_US / 400) < 0) { ERR("no answer to $AA\n"); return 1; }
    if (remote_write(stub, stub_at, 6) < 0) return 1;
    printf("Stub written to $%04X: ", stub_at);
    { int i; for (i = 0; i < 6; i++) printf("%02X ", stub[i]); putchar('\n'); }
    if (remote_call(stub_at, 0, 0) < 0) return 1;
    printf("Executed. $%02X (%s) should now be in the collection.\n",
           card, cardname(card));
    rpc_disconnect();
    return 0;
}

static int cmd_predict(const char *a, const char *b)
{
    uint8_t na[16], nb[16], sa, xa, sb, xb, d, e;
    memset(na, 0, 16); memset(nb, 0, 16);
    strncpy((char *)na, a, 15); strncpy((char *)nb, b, 15);
    name_digest(na, &sa, &xa);
    name_digest(nb, &sb, &xb);
    d = (uint8_t)(xa - xb); e = (uint8_t)(sa - sb);
    printf("%-16s sum $%02X xor $%02X   d $%02X e $%02X  ->  $%02X  %s\n",
           a, sa, xa, d, e, pick_card(d, e), cardname(pick_card(d, e)));
    d = (uint8_t)(xb - xa); e = (uint8_t)(sb - sa);
    printf("%-16s sum $%02X xor $%02X   d $%02X e $%02X  ->  $%02X  %s\n",
           b, sb, xb, d, e, pick_card(d, e), cardname(pick_card(d, e)));
    return 0;
}


/* ------------------------------------------------------------------ */
/* TCG save access — no code execution, just the mapper registers      */
/* ------------------------------------------------------------------ */
/*
 * A cmd-3 write below $8000 does not land in memory: it lands in the
 * cartridge mapper. $0000 is RAM enable, $4000 is the RAM bank. So SRAM
 * becomes readable and writable without executing anything.
 *
 * $2000 is the ROM bank and would swap out the code the game is currently
 * running, so it is never written here.
 *
 * The enable does not survive our disconnect — the game unwinds and turns
 * SRAM back off — so every command below does its whole job in one session.
 */

#define SAVE_SIZE   0x8000       /* 4 banks x 8 KiB */
#define SAVE_BANKS  4
#define SRAM_BASE   0xA000

static int sram_enable(void)  { uint8_t v = 0x0A; return remote_write(&v, 0x0000, 1); }
static int sram_bank(int b)   { uint8_t v = (uint8_t)b; return remote_write(&v, 0x4000, 1); }

static int tcg_session_open(void)
{
    printf("Waiting for the Game Boy in any IR screen...\n");
    fflush(stdout);
    if (sync_as_sender(400, RX_LONG_US / 400) < 0) { ERR("no answer to $AA\n"); return -1; }
    if (sram_enable() < 0) { ERR("could not unlock SRAM\n"); return -1; }
    return 0;
}

static int cmd_dump_save(const char *path)
{
    static uint8_t img[SAVE_SIZE];
    int bank, off;
    FILE *f;

    if (tcg_session_open() < 0) return 1;
    for (bank = 0; bank < SAVE_BANKS; bank++) {
        if (sram_bank(bank) < 0) return 1;
        for (off = 0; off < 0x2000; off += 256) {
            if (remote_read((uint16_t)(SRAM_BASE + off), img + bank * 0x2000 + off, 256) < 0)
                return 1;
            printf("\r  reading  %5d / %d bytes", bank * 0x2000 + off + 256, SAVE_SIZE);
            fflush(stdout);
        }
    }
    rpc_disconnect();
    putchar('\n');

    f = fopen(path, "wb");
    if (!f) { ERR("cannot write %s\n", path); return 1; }
    fwrite(img, 1, SAVE_SIZE, f);
    fclose(f);
    printf("Saved %d bytes to %s\n", SAVE_SIZE, path);
    return 0;
}

static int cmd_push_save(const char *path)
{
    static uint8_t img[SAVE_SIZE];
    int bank, off;
    FILE *f = fopen(path, "rb");

    if (!f) { ERR("cannot read %s\n", path); return 1; }
    if (fread(img, 1, SAVE_SIZE, f) != SAVE_SIZE) {
        ERR("%s is not %d bytes\n", path, SAVE_SIZE);
        fclose(f); return 1;
    }
    fclose(f);

    if (tcg_session_open() < 0) return 1;
    for (bank = 0; bank < SAVE_BANKS; bank++) {
        if (sram_bank(bank) < 0) return 1;
        for (off = 0; off < 0x2000; off += 128) {
            if (remote_write(img + bank * 0x2000 + off, (uint16_t)(SRAM_BASE + off), 128) < 0)
                return 1;
            printf("\r  writing  %5d / %d bytes", bank * 0x2000 + off + 128, SAVE_SIZE);
            fflush(stdout);
        }
    }
    rpc_disconnect();
    printf("\nRestored %s. Power-cycle the Game Boy.\n", path);
    return 0;
}

static int cmd_set_name(const char *name)
{
    uint8_t buf[16], back[16];
    int i;

    memset(buf, 0, sizeof buf);
    for (i = 0; i < 15 && name[i]; i++) buf[i] = (uint8_t)name[i];

    if (tcg_session_open() < 0) return 1;
    if (sram_bank(0) < 0) return 1;
    if (remote_read(0xA010, back, 16) < 0) return 1;
    printf("  old name  \"%.16s\"\n", back);
    if (remote_write(buf, 0xA010, 16) < 0) return 1;
    if (remote_read(0xA010, back, 16) < 0) return 1;
    rpc_disconnect();
    printf("  new name  \"%.16s\"\n", back);
    return memcmp(buf, back, 16) == 0 ? 0 : (ERR("read-back mismatch\n"), 1);
}

static int cmd_wipe_save(int confirmed)
{
    uint8_t zero[3] = { 0, 0, 0 };

    if (!confirmed) {
        char line[16];
        printf("This destroys the save marker at $A000. The next boot reinitialises\n"
               "the cartridge and everything on it is gone. Type YES to continue: ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "YES", 3) != 0) {
            printf("Aborted.\n");
            return 1;
        }
    }
    if (tcg_session_open() < 0) return 1;
    if (sram_bank(0) < 0) return 1;
    if (remote_write(zero, 0xA000, 3) < 0) return 1;
    rpc_disconnect();
    printf("Marker cleared. The next boot will reinitialise the save.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* The resident bridge — payload.asm, entered with RPC command 4       */
/* ------------------------------------------------------------------ */

#define BR_PING    0
#define BR_READ    1
#define BR_WRITE   2
#define BR_MAPPER  3
#define BR_HALT    4
#define BR_NOBANK  0xFF          /* leave the mapper's bank register alone */
#define RX_BRIDGE_US 250000      /* the payload waits ~1.7 ms before answering */

static uint16_t cfg_payload_at = 0xC700;
static const char *cfg_payload  = "payload.bin";

static int bridge_cmd(uint8_t op, uint8_t bank, uint16_t addr, uint8_t len, int tries)
{
    uint8_t p[7];
    int i, sum = 0;
    if (sync_as_sender(tries, 60000) < 0) return -1;
    p[0] = op; p[1] = bank;
    p[2] = addr & 0xFF; p[3] = addr >> 8;
    p[4] = len; p[5] = 0;
    for (i = 0; i < 6; i++) sum += p[i];
    p[6] = (uint8_t)(-sum);
    return tx_bytes(p, 7);
}

static int bridge_ping(int tries)
{
    uint8_t r;
    if (bridge_cmd(BR_PING, 0, 0, 0, tries) < 0) return -1;
    if (recv_block(&r, 1, RX_BRIDGE_US) < 0) return -1;
    return r == 0x4B ? 0 : -1;
}

/* len 0 means 256 */
static int bridge_read(uint8_t bank, uint16_t addr, uint8_t len, uint8_t *dst)
{
    if (bridge_cmd(BR_READ, bank, addr, len, 8) < 0) return -1;
    return recv_block(dst, len ? len : 256, RX_BRIDGE_US);
}

static int bridge_write(uint8_t bank, uint16_t addr, const uint8_t *src, uint8_t len)
{
    uint8_t ack;
    if (bridge_cmd(BR_WRITE, bank, addr, len, 8) < 0) return -1;
    if (send_block(src, len ? len : 256) < 0) return -1;
    return recv_block(&ack, 1, RX_BRIDGE_US);
}

static int bridge_mapper(uint16_t addr, uint8_t value)
{
    uint8_t r;
    if (bridge_cmd(BR_MAPPER, value, addr, 0, 8) < 0) return -1;
    return recv_block(&r, 1, RX_BRIDGE_US);
}

static int bridge_wait(int seconds)
{
    int i;
    for (i = 0; i < seconds; i++) {
        if (bridge_ping(6) == 0) return 0;
        printf("\r  waiting for the bridge... %ds ", i + 1);
        fflush(stdout);
    }
    putchar('\n');
    return -1;
}

static int cmd_upload(void)
{
    static uint8_t blob[4096];
    size_t n;
    int off;
    FILE *f = fopen(cfg_payload, "rb");

    if (!f) { ERR("cannot read %s (run make payload.bin)\n", cfg_payload); return 1; }
    n = fread(blob, 1, sizeof blob, f);
    fclose(f);
    if (n < 16) { ERR("%s is too small\n", cfg_payload); return 1; }

    printf("Waiting for the Game Boy in any IR screen...\n");
    fflush(stdout);
    if (sync_as_sender(400, RX_LONG_US / 400) < 0) { ERR("no answer to $AA\n"); return 1; }

    for (off = 0; off < (int)n; off += 128) {
        int chunk = (int)n - off < 128 ? (int)n - off : 128;
        if (remote_write(blob + off, (uint16_t)(cfg_payload_at + off), chunk) < 0) return 1;
        printf("\r  uploading %4d / %d bytes to $%04X", off + chunk, (int)n, cfg_payload_at);
        fflush(stdout);
    }
    putchar('\n');

    printf("Jumping to $%04X...\n", cfg_payload_at);
    if (remote_call(cfg_payload_at, 0, 0) < 0) return 1;   /* no reply: it took over */

    if (bridge_wait(10) < 0) { ERR("the bridge never answered\n"); return 1; }
    printf("Bridge is live. The cartridge can now be removed.\n");
    return 0;
}

/*
 * Run a raw WRAM image: upload it, read it back to verify, then jump to it.
 * For self-contained homebrew that never returns (e.g. snake-gbc, whose
 * 386-byte game is linked to $C000). Command 4 enters it from inside
 * IR_ServeLoop, i.e. after IR_Begin: IME = 0, single speed. The image must not
 * overlap the TCG's own IR packet buffer at $CE8C (it is still in use until
 * the jump) or the stack below $E000.
 */

/* ================================================================== */
/* Homebrew loader host side (docs/05-loader-protocol.md)        */
/* Stage 0 reuses the Card Pop! RPC upload; Stages 1-2 use a robust    */
/* self-clocking mark/space link the loader defines once it is resident.*/
/* shm (VBA) backend only; timing constants co-tuned with              */
/* gb/hardware.inc and attiny/main-tcg-loader.asm (docs/05 §2).         */
/* ================================================================== */
/* The loader's mark/space link (docs/05) exists in the SHM build (VBA, GB
 * clock) and in the direct GPIO backend (real time, the Pi's system timer).  */
#if defined(USE_SHM) || (!defined(USE_PIGPIO) && !defined(USE_WIRINGPI))
#define HAVE_MS_LINK 1
#endif

#ifdef HAVE_MS_LINK
/* mark/space durations (microseconds) — must match gb/hardware.inc     */
#define MS_MARK_US    50.0
#define MS_SPACE0_US  64.0
#define MS_SPACE1_US  224.0
#define MS_HELLO_US   130.0
#define MS_LEAD_US    800.0      /* idle before each byte (GB frame sync)  */
#define MS_RX_TMO_US  30000.0
#endif

#ifdef USE_SHM
#define MS_THRESH_US  240.0     /* GB->host split: ~190 / ~490 us marks   */

static long long ms_mc(double us) { return (long long)(us / MC2US + 0.5); }

/* schedule one mark (LED on MS_MARK) then a gap; advance *base (M-cyc). */
static void ms_mark(long long *base, double gap_us)
{
    shm_publish(&g_shm_wr, *base, 1, 1);
    shm_publish(&g_shm_wr, *base + ms_mc(MS_MARK_US), 0, 1);
    *base += ms_mc(MS_MARK_US) + ms_mc(gap_us);
}

/* Wait until the GB clock reaches `end`. VBA only refreshes its live clock
 * while the GB touches the IR port, so a clock that does not move at all for
 * 500 ms (wall) means the GB stopped listening (it took a copy and moved on:
 * menu, payload running). Returns -1 then. A merely slow emulator (it lags
 * under load) keeps moving and is waited for. */
static int ms_pace(long long end)
{
    uint32_t t_moved = now_us();
    long long last = shm_peer_live_cT(&g_shm_rd);
    for (;;) {
        long long live = shm_peer_live_cT(&g_shm_rd);
        if (live >= end) return 0;
        if (live != last) { last = live; t_moved = now_us(); }
        else if ((uint32_t)(now_us() - t_moved) > 500000u) return -1;   /* clock stalled */
    }
}

static int ms_tx_byte(uint8_t b)
{
    long long base = shm_peer_live_cT(&g_shm_rd);
    int i;
    if (base < 0) base = 0;
    base += ms_mc(MS_LEAD_US);
    for (i = 7; i >= 0; i--)
        ms_mark(&base, (b >> i) & 1 ? MS_SPACE1_US : MS_SPACE0_US);
    ms_mark(&base, MS_SPACE0_US);          /* trailing mark bounds bit 0 */
    g_shm_own_on = 0;
    return ms_pace(base);
}

/* Stay dark for `us` of GB time. Before the loader feed: the bootstrap first
 * waits for vblank (up to ~17 ms) to colour the screen, deaf to IR, and would
 * miss the first copy (the ATtiny waits the same 20 ms). VBA only publishes
 * the GB clock while the GB polls the IR port, so wait for it to move first
 * (at most 2 s wall): only then is "now" in GB time known. */
static void ms_idle(double us)
{
    long long base = shm_peer_live_cT(&g_shm_rd), live = base;
    uint32_t t0 = now_us();
    while (live == base && (uint32_t)(now_us() - t0) < 2000000u)
        live = shm_peer_live_cT(&g_shm_rd);
    if (live < 0) live = 0;
    ms_pace(live + ms_mc(us));
}

static void ms_send_hello(void)
{
    long long base = shm_peer_live_cT(&g_shm_rd);
    if (base < 0) base = 0;
    base += ms_mc(MS_LEAD_US);
    /* hello = long mark, long space, long mark */
    shm_publish(&g_shm_wr, base, 1, 1);
    shm_publish(&g_shm_wr, base + ms_mc(MS_HELLO_US), 0, 1);
    base += ms_mc(MS_HELLO_US) + ms_mc(MS_SPACE1_US);
    shm_publish(&g_shm_wr, base, 1, 1);
    shm_publish(&g_shm_wr, base + ms_mc(MS_HELLO_US), 0, 1);
    base += ms_mc(MS_HELLO_US);
    ms_pace(base);
    g_shm_own_on = 0;
}

/* wait for the next light-ON edge, return its cT (or -1 on timeout) */
static long long ms_next_light(double tmo_us)
{
    uint32_t t0 = now_us();
    struct ShmSlot s;
    shm_try_peer();
    for (;;) {
        if (shm_next(&s)) {
            if (SHM_RX_LIGHT(s.led_on)) return (long long)s.cT;
            continue;
        }
        if ((uint32_t)(now_us() - t0) > (uint32_t)tmo_us) return -1;
    }
}

/* receive one mark/space byte; returns 0..255 or -1 on timeout */
/* next light edge within `max_gap_us` of *GB* time after `prev` (the GB's own
 * clock, so a lagging VBA cannot cut a byte in half); -1 on timeout, or after
 * 2 s wall if the GB clock froze */
static long long ms_next_light_gb(long long prev, double max_gap_us)
{
    long long deadline = prev + ms_mc(max_gap_us);
    uint32_t t0 = now_us();
    struct ShmSlot sl;
    shm_try_peer();
    for (;;) {
        if (shm_next(&sl)) {
            if (SHM_RX_LIGHT(sl.led_on)) return (long long)sl.cT;
            continue;
        }
        if (shm_peer_live_cT(&g_shm_rd) >= deadline) return -1;
        if ((uint32_t)(now_us() - t0) > 2000000u) return -1;
    }
}

/* decode the 8 mark-to-mark gaps after the first mark (at `prev`); the GB's
 * gaps are <= ~0.5 ms, so 3 ms of GB time without a mark ends the byte */
static int ms_rx_rest(long long prev)
{
    long long t;
    int i, b = 0;
    for (i = 0; i < 8; i++) {
        t = ms_next_light_gb(prev, 3000.0);
        if (t < 0) return -1;
        double gap_us = (double)(t - prev) * MC2US;
        b = (b << 1) | (gap_us >= MS_THRESH_US ? 1 : 0);   /* MSB first */
        prev = t;
    }
    return b & 0xFF;
}

static int ms_rx_byte(void)
{
    long long prev = ms_next_light(MS_RX_TMO_US);
    return prev < 0 ? -1 : ms_rx_rest(prev);
}

/* Listen for one byte whose first mark comes within `window_us` of *GB* time.
 * An ACK window must be measured on the GB's clock: VBA can run well behind
 * wall time, and a 30 ms wall-clock window shrank to ~3 ms of GB time -- less
 * than the GB's pre-ACK idle wait (docs/08). The wall-clock cap only catches a
 * GB that stopped polling IR (VBA's clock then freezes). */
static int ms_rx_byte_gbtime(double window_us)
{
    long long start = shm_peer_live_cT(&g_shm_rd), deadline = start + ms_mc(window_us);
    uint32_t t0 = now_us();
    struct ShmSlot sl;
    shm_try_peer();
    for (;;) {
        if (shm_next(&sl)) {
            if (SHM_RX_LIGHT(sl.led_on)) return ms_rx_rest((long long)sl.cT);
            continue;
        }
        if (shm_peer_live_cT(&g_shm_rd) >= deadline || (uint32_t)(now_us() - t0) > 2000000u) {
            if (getenv("GBCPOP_DEBUG_ACK"))
                fprintf(stderr, "\n  [ack window: start cT %lld, now %lld (+%.1f ms GB), wall %.1f ms]\n",
                        start, shm_peer_live_cT(&g_shm_rd),
                        (shm_peer_live_cT(&g_shm_rd) - start) * MC2US / 1000.0,
                        (now_us() - t0) / 1000.0);
            return -1;
        }
    }
}

static int ms_wait_hello(double tmo_us)
{
    if (ms_next_light(tmo_us) < 0) return -1;   /* first mark */
    if (ms_next_light(tmo_us) < 0) return -1;   /* second mark after long gap */
    return 0;
}

#elif defined(HAVE_MS_LINK)
/*
 * The same link on real hardware (direct backend): host time is real time,
 * read from the BCM system timer, so no pacing against a peer clock. Values
 * as proven on the ATtiny (attiny/main-tcg-loader.asm): the GB's marks come
 * ~190 ('0') / ~490 ('1') us apart, split at 336 us. Receiving times the
 * light-ON edges only, so a detector that stretches its marks is harmless.
 */
#define MS_THRESH_US  336.0

/* LED on for a mark at `*t`, off after MS_MARK; advance *t by mark + gap */
static void ms_mark(uint32_t *t, double gap_us)
{
    wait_until(*t);
    gp_write(1);
    wait_until(*t + (uint32_t)MS_MARK_US);
    gp_write(0);
    *t += (uint32_t)(MS_MARK_US + gap_us);
}

static int ms_tx_byte(uint8_t b)
{
    uint32_t t;
    int i;
    gp_write(0);
    t = now_us() + (uint32_t)MS_LEAD_US;
    for (i = 7; i >= 0; i--)
        ms_mark(&t, (b >> i) & 1 ? MS_SPACE1_US : MS_SPACE0_US);
    ms_mark(&t, MS_SPACE0_US);             /* trailing mark bounds bit 0 */
    wait_until(t);
    g_stats.tx++;
    return 0;
}

static void ms_idle(double us)
{
    gp_write(0);
    wait_until(now_us() + (uint32_t)us);
}

static void ms_send_hello(void)
{
    uint32_t t;
    gp_write(0);
    t = now_us() + (uint32_t)MS_LEAD_US;
    wait_until(t);
    gp_write(1);
    wait_until(t + (uint32_t)MS_HELLO_US);
    gp_write(0);
    t += (uint32_t)(MS_HELLO_US + MS_SPACE1_US);
    wait_until(t);
    gp_write(1);
    wait_until(t + (uint32_t)MS_HELLO_US);
    gp_write(0);
}

/* next light-ON edge before `deadline`; a mark already lit when we start is
 * not an edge. Returns its time, or -1. */
static long long ms_next_light_until(uint32_t deadline)
{
    int prev = gp_read(), lvl;
    uint32_t t;
    for (;;) {
        lvl = gp_read();
        t = now_us();
        if (lvl && !prev) return (long long)t;
        prev = lvl;
        if ((int32_t)(t - deadline) >= 0) return -1;
    }
}

static long long ms_next_light(double tmo_us)
{
    return ms_next_light_until(now_us() + (uint32_t)tmo_us);
}

/* decode the 8 mark-to-mark gaps after the first mark (at `prev`); the GB's
 * gaps are <= ~0.5 ms, so 3 ms without a mark ends the byte */
static int ms_rx_rest(long long prev)
{
    long long t;
    int i, b = 0;
    for (i = 0; i < 8; i++) {
        t = ms_next_light_until((uint32_t)prev + 3000u);
        if (t < 0) return -1;
        b = (b << 1) | ((uint32_t)(t - prev) >= (uint32_t)MS_THRESH_US ? 1 : 0);   /* MSB first */
        prev = t;
    }
    g_stats.rx++;
    return b & 0xFF;
}

static int ms_rx_byte(void)
{
    long long prev = ms_next_light(MS_RX_TMO_US);
    return prev < 0 ? -1 : ms_rx_rest(prev);
}

/* one byte whose first mark comes within `window_us` (an ACK window) */
static int ms_rx_byte_gbtime(double window_us)
{
    long long prev = ms_next_light(window_us);
    return prev < 0 ? -1 : ms_rx_rest(prev);
}

static int ms_wait_hello(double tmo_us)
{
    if (ms_next_light(tmo_us) < 0) return -1;   /* first mark */
    if (ms_next_light(tmo_us) < 0) return -1;   /* second mark after long gap */
    return 0;
}
#endif /* USE_SHM / HAVE_MS_LINK */

#ifdef HAVE_MS_LINK
/* Stage 0: upload a raw blob to $C000 via the Card Pop! RPC, then jump. */
static int loader_stage0(const uint8_t *blob, int n)
{
    int off;
    if (sync_as_sender(400, RX_LONG_US / 400) < 0) { ERR("stage0: no $AA\n"); return -1; }
    for (off = 0; off < n; off += 128) {
        int c = n - off < 128 ? n - off : 128;
        if (remote_write(blob + off, (uint16_t)(0xC000 + off), c) < 0) return -1;
    }
    if (remote_call(0xC000, 0, 0) < 0) return -1;   /* jp $C000 -> bootstrap */
    return 0;
}

static int read_file(const char *path, uint8_t *buf, int max)
{
    FILE *f = fopen(path, "rb");
    int n;
    if (!f) { ERR("cannot read %s\n", path); return -1; }
    n = (int)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

/* A parsed payload: up to 6 segments + concatenated bodies, entry, flags. */
#define PL_MAXSEG 6
struct Payload {
    int nseg;
    uint16_t dest[PL_MAXSEG];
    uint16_t len[PL_MAXSEG];
    uint16_t entry;
    uint8_t  flags;
    uint8_t  body[8192];
    int      bodylen;
};

/* dir of a path, into buf (keeps trailing '/'); "" if no slash. */
static void path_dir(const char *path, char *buf, size_t n)
{
    const char *slash = strrchr(path, '/');
    if (!slash) { buf[0] = 0; return; }
    size_t k = (size_t)(slash - path + 1);
    if (k >= n) k = n - 1;
    memcpy(buf, path, k); buf[k] = 0;
}

/* Parse a payload spec: a manifest.txt (multi-segment) or a bare .bin (one seg
 * at $C000, entry $C000). Returns 0 on success. */
static int parse_payload(const char *path, struct Payload *p)
{
    size_t l = strlen(path);
    memset(p, 0, sizeof *p);
    if (l > 4 && !strcmp(path + l - 4, ".bin")) {
        int n = read_file(path, p->body, sizeof p->body);
        if (n <= 0) return -1;
        p->nseg = 1; p->dest[0] = 0xC000; p->len[0] = (uint16_t)n;
        p->entry = 0xC000; p->flags = 0; p->bodylen = n;
        return 0;
    }
    /* manifest.txt */
    FILE *f = fopen(path, "r");
    char line[256], dir[200];
    if (!f) { ERR("cannot open manifest %s\n", path); return -1; }
    path_dir(path, dir, sizeof dir);
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        int v; char fname[128];
        if (sscanf(line, " entry = %i", &v) == 1) { p->entry = (uint16_t)v; }
        else if (strstr(line, "flags") && strstr(line, "LCD_OFF")) { p->flags |= 1; }
        else if (sscanf(line, " segment %i %127s", &v, fname) == 2) {
            char full[400]; uint8_t seg[8192]; int sn;
            if (p->nseg >= PL_MAXSEG) { ERR("too many segments\n"); fclose(f); return -1; }
            snprintf(full, sizeof full, "%s%s", dir, fname);
            sn = read_file(full, seg, sizeof seg);
            if (sn <= 0) { fclose(f); return -1; }
            if (p->bodylen + sn > (int)sizeof p->body) { ERR("payload too big\n"); fclose(f); return -1; }
            p->dest[p->nseg] = (uint16_t)v; p->len[p->nseg] = (uint16_t)sn;
            memcpy(p->body + p->bodylen, seg, (size_t)sn);
            p->bodylen += sn; p->nseg++;
        }
    }
    fclose(f);
    if (p->nseg == 0) { ERR("manifest has no segments\n"); return -1; }
    return 0;
}

/* stream IDs: frame header byte after START (build_frame, recvstream.inc) */
#define ID_LOADER   'L'
#define ID_FONT     'F'
#define ID_MENU     'M'
#define ID_MANIFEST 'N'
#define ID_BODY     'B'
#define TRIES 8                              /* copies per chunk before giving up */
static int send_stream(const uint8_t *data, int n, int tries, int rle, uint8_t id, int req_max); /* below */

/* Send one payload: the manifest stream (nseg, table, entry, flags) and then
 * the body stream, each chunked and ACKed (send_stream). */
/* Manifest body: nseg, nseg*(dest16, len16), entry16, flags. Returns length. */
static int build_manifest(const struct Payload *p, uint8_t *man)
{
    int mi = 0, i;
    man[mi++] = (uint8_t)p->nseg;
    for (i = 0; i < p->nseg; i++) {
        man[mi++] = (uint8_t)(p->dest[i] & 0xFF); man[mi++] = (uint8_t)(p->dest[i] >> 8);
        man[mi++] = (uint8_t)(p->len[i] & 0xFF);  man[mi++] = (uint8_t)(p->len[i] >> 8);
    }
    man[mi++] = (uint8_t)(p->entry & 0xFF); man[mi++] = (uint8_t)(p->entry >> 8);
    man[mi++] = p->flags;
    return mi;
}

static int send_payload_frame(const struct Payload *p)
{
    uint8_t man[4 * PL_MAXSEG + 4];
    int mi = build_manifest(p, man);
    if (send_stream(man, mi, TRIES, 1, ID_MANIFEST, 0) < 0 ||
        send_stream(p->body, p->bodylen, TRIES, 1, ID_BODY, 0) < 0) {
        ERR("payload transfer failed (a chunk was never acknowledged)\n");
        return -1;
    }
    return 0;
}

/* Wait up to tmo_us for the loader's REQ byte (the menu choice); -1 if none. */
static int wait_req(uint32_t tmo_us)
{
    uint32_t t0 = now_us();
    int r = -1;
    while ((uint32_t)(now_us() - t0) < tmo_us && r < 0) r = ms_rx_byte();
    return r;
}

/* test: act as if the first REQ was lost (GBCPOP_TEST_DROPREQ) -- the loader
 * must give up waiting (~0.5 s) and send the REQ once more by itself */
static int test_drop_req(int req)
{
    static int dropped;
    if (dropped || !getenv("GBCPOP_TEST_DROPREQ")) return 0;
    dropped = 1;
    fprintf(stderr, "  TEST_DROPREQ: ignoring REQ %d\n", req);
    return 1;
}

/* gbcpop payloadcheck <manifest-or-bin> : parse + print (no IR). */
static int cmd_payloadcheck(const char *path)
{
    static struct Payload pl;
    int i;
    if (!path) { ERR("payloadcheck <manifest.txt|file.bin>\n"); return 1; }
    if (parse_payload(path, &pl) < 0) return 1;
    printf("payload '%s': %d seg(s), entry $%04X, flags $%02X, body %d bytes\n",
           path, pl.nseg, pl.entry, pl.flags, pl.bodylen);
    for (i = 0; i < pl.nseg; i++)
        printf("  seg %d -> $%04X, %d bytes\n", i, pl.dest[i], pl.len[i]);
    return 0;
}

/* RLE for the checked streams (decoder: rle_put in loader/recvstream.inc).
 * Control byte $00-$7F = n+1 literals follow; $80-$FF = next byte repeated
 * (n&$7F)+1 times. The GB writes a whole run between two IR bytes, inside the
 * ~0.8 ms MS_LEAD gap that must also leave room for its FRAME_IDLE sync (~0.4 ms);
 * at ~5.7 us per written byte, 32 keeps the run under ~0.2 ms. Re-check this if
 * MS_LEAD shrinks further. */
#define RLE_MAX_RUN 32
#define RLE_MIN_RUN 3           /* shorter runs cost more than literals       */

static int rle_encode(const uint8_t *in, int n, uint8_t *out)
{
    int i = 0, o = 0, lit = -1;             /* lit = index of the open literal ctrl */
    while (i < n) {
        int run = 1;
        while (i + run < n && in[i + run] == in[i] && run < RLE_MAX_RUN) run++;
        if (run >= RLE_MIN_RUN) {
            out[o++] = (uint8_t)(0x80 | (run - 1));
            out[o++] = in[i];
            i += run;
            lit = -1;
        } else {
            if (lit < 0 || out[lit] == 0x7F) { lit = o; out[o++] = 0xFF; }
            out[lit] = (uint8_t)(out[lit] + 1);  /* 0xFF+1 wraps to 0 = 1 literal */
            out[o++] = in[i++];
        }
    }
    return o;
}

static int rle_decode(const uint8_t *in, int n, uint8_t *out, int max)
{
    int i = 0, o = 0;
    while (i < n) {
        int c = in[i++], k;
        if (c & 0x80) {
            if (i >= n) return -1;
            for (k = 0; k <= (c & 0x7F); k++) { if (o >= max) return -1; out[o++] = in[i]; }
            i++;
        } else {
            for (k = 0; k <= c; k++) { if (i >= n || o >= max) return -1; out[o++] = in[i++]; }
        }
    }
    return o;
}

/* One wire frame = one chunk: $55 $55 START ID SEQ len16 body CK16, CK = 16-bit
 * sum of ID, SEQ and the wire body. START $3C = raw; $3D = RLE body (per chunk),
 * used when `rle` and it is shorter. SEQ = chunk index | $80 on the last chunk.
 * The GB stores chunks in order, re-ACKs a repeat of the last one without
 * storing it, and ignores anything else (recvstream.inc). Returns the frame
 * length; *packed_from (if set) gets the raw length when RLE'd, else 0. */
#define FRAME_EXTRA 9                        /* 55 55 START ID SEQ len16 ... CK16 */
#define CHUNK_RAW   128                      /* = CHUNK_RAW_MAX in recvstream.inc */
#define FRAME_MAX   (CHUNK_RAW + CHUNK_RAW / 128 + 1 + FRAME_EXTRA)
static int build_frame(const uint8_t *data, int n, int rle, uint8_t id, uint8_t seq,
                       uint8_t *out, int *packed_from)
{
    uint8_t *body = out + 7;
    uint8_t start = 0x3C;
    int i, wn = n;
    unsigned sum;
    if (packed_from) *packed_from = 0;
    memcpy(body, data, (size_t)n);
    if (rle) {
        uint8_t packed[CHUNK_RAW + CHUNK_RAW / 128 + 1], check[CHUNK_RAW];
        int pn = rle_encode(data, n, packed);
        if (pn < n) {
            if (rle_decode(packed, pn, check, (int)sizeof check) != n || memcmp(check, data, (size_t)n)) {
                ERR("internal: RLE round-trip mismatch, sending raw\n");
            } else {
                memcpy(body, packed, (size_t)pn); wn = pn; start = 0x3D;
                if (packed_from) *packed_from = n;
            }
        }
    }
    sum = (unsigned)id + seq;
    for (i = 0; i < wn; i++) sum += body[i];
    out[0] = 0x55; out[1] = 0x55;              /* prime the GB's frame sync */
    out[2] = start;
    out[3] = id;
    out[4] = seq;
    out[5] = (uint8_t)(wn & 0xFF); out[6] = (uint8_t)(wn >> 8);
    out[7 + wn] = (uint8_t)(sum & 0xFF); out[8 + wn] = (uint8_t)((sum >> 8) & 0xFF);
    return wn + FRAME_EXTRA;
}

/* All chunk frames of a stream, back to back (for the ATtiny tables). Returns
 * the total length. */
static int build_stream_frames(const uint8_t *data, int n, int rle, uint8_t id, uint8_t *out)
{
    int off = 0, seq = 0, o = 0;
    do {
        int m = n - off < CHUNK_RAW ? n - off : CHUNK_RAW;
        o += build_frame(data + off, m, rle, id,
                         (uint8_t)(seq | (off + m >= n ? 0x80 : 0)), out + o, NULL);
        off += m; seq++;
    } while (off < n);
    return o;
}

/* Send a stream chunk by chunk (docs/05-loader-protocol.md §3-§4): each chunk frame (build_frame)
 * goes out up to `tries` times until the GB ACKs it with the frame's CK_lo; the
 * GB answers once the line has been idle ~4 ms (recvstream.inc), so after each
 * copy we listen 30 ms of GB time. Stop-and-wait per chunk: a bad copy costs one
 * chunk, not the stream. The bootstrap has no RLE decoder: its loader feed
 * passes rle=0.
 * req_max > 0 (the menu, the last stream before the GB waits for a button):
 * the GB re-ACKs a repeated last menu chunk only while it lingers (~100 ms,
 * recv_linger), then draws; should its REQ arrive while we still listen for
 * that ACK, a reply in 1..req_max is taken as the REQ (it implies the GB has
 * the menu).
 * Returns 0 (all chunks ACKed), the REQ id, or -1 (a chunk never got an ACK). */
static int send_stream(const uint8_t *data, int n, int tries, int rle, uint8_t id, int req_max)
{
    uint8_t frame[FRAME_MAX];
    int off = 0, seq = 0, sent = 0;
    while (off < n) {
        int m = n - off < CHUNK_RAW ? n - off : CHUNK_RAW, last = off + m >= n;
        int fn = build_frame(data + off, m, rle, id, (uint8_t)(seq | (last ? 0x80 : 0)), frame, NULL);
        uint8_t ack_want = frame[fn - 2];        /* CK_lo */
        int c, i, acked = 0;
        if (last && id == ID_BODY && getenv("GBCPOP_TEST_BADLAST"))
            frame[fn - 1] ^= 0xFF;   /* test: the last body chunk never arrives
                                      * intact -> must be reported as a failure */
        if (id == ID_LOADER && seq == 3 && getenv("GBCPOP_TEST_STALL")) {
            /* test: 300 ms of silence mid-stream (bootstrap: screen red after
             * ~63 ms, teal again with the next good chunk) */
            uint32_t t0 = now_us();
            while ((uint32_t)(now_us() - t0) < 300000u) { }
        }
        for (c = 0; c < tries && !acked; c++) {
            int st = 0, ack;
            fprintf(stderr, "\r  stream '%c': chunk %d, copy %d   ", id, seq, c + 1);
            for (i = 0; i < fn && !st; i++) st |= ms_tx_byte(frame[i]);
            sent++;
            if (st) {   /* VBA: the GB stopped polling IR (it moved on / jumped) */
                fprintf(stderr, "\r  stream '%c': receiver stopped listening at chunk %d\n", id, seq);
                return last ? 0 : -1;
            }
            ack = ms_rx_byte_gbtime(30000.0);    /* 30 ms of GB time */
            if (ack == ack_want && c == 0 && getenv("GBCPOP_TEST_REPEAT"))
                continue;   /* test: act as if this ACK was lost -> the GB must
                             * re-ACK the repeat without storing it */
            if (ack == ack_want) { acked = 1; break; }
            if (last && ack >= 1 && ack <= req_max) {
                fprintf(stderr, "\r  stream '%c': REQ %d arrived instead of the last ACK\n", id, ack);
                return ack;
            }
            if (ack >= 0)
                fprintf(stderr, "\r  stream '%c' chunk %d copy %d: reply $%02X is not the ACK $%02X\n",
                        id, seq, c + 1, ack, ack_want);
        }
        if (!acked) {   /* the GB lingers ~100 ms after a stream's last chunk
                         * re-ACKing repeats (recv_linger), so this means the
                         * chunk really did not arrive */
            fprintf(stderr, "\r  stream '%c': chunk %d not acknowledged after %d copies\n", id, seq, tries);
            return -1;
        }
        off += m; seq++;
    }
    fprintf(stderr, "\r  stream '%c': %d bytes in %d chunks, %d frames sent (%d resent)\n",
            id, n, seq, sent, sent - seq);
    return 0;
}

/* Menu font: 5x7 glyphs for ASCII $2C..$5F (",-./0-9:;<=>?@A-Z[\\]^_"), one
 * 5-bit row value per line. The loader puts glyph c into tile c, so the menu
 * text is plain ASCII and space ($20) is simply a blank tile. */
#define FONT_FIRST 0x2C
static const uint8_t font5x7[][7] = {
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, /* , */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, /* / */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, /* ; */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /* @ */
    {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* [ */
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, /* \ */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ] */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
};
#define FONT_COUNT ((int)(sizeof font5x7 / sizeof font5x7[0]))

/* Font stream (ID 'F'): first, count, count*8 1bpp rows (8x8 cell: blank top
 * row, glyph in columns 1-5). */
static int build_font_stream(uint8_t *out)
{
    int o = 0, g, r;
    out[o++] = FONT_FIRST; out[o++] = (uint8_t)FONT_COUNT;
    for (g = 0; g < FONT_COUNT; g++) {
        out[o++] = 0;
        for (r = 0; r < 7; r++) out[o++] = (uint8_t)(font5x7[g][r] << 2);
    }
    return o;
}

/* Menu display name for a payload path: "x/snake.bin" -> "SNAKE",
 * "x/audio-dumper/manifest.txt" -> "AUDIO DUMPER". Upper-cased, '-'/'_' become
 * spaces, characters without a glyph become '?'. */
static void payload_name(const char *path, char *name, size_t n)
{
    char buf[200];
    const char *base, *slash;
    size_t l = strlen(path), k, o = 0;
    snprintf(buf, sizeof buf, "%s", path);
    l = strlen(buf);
    if (l > 13 && !strcmp(buf + l - 13, "/manifest.txt")) buf[l - 13] = 0;
    else { char *dot = strrchr(buf, '.'); if (dot && !strchr(dot, '/')) *dot = 0; }
    slash = strrchr(buf, '/');
    base = slash ? slash + 1 : buf;
    for (k = 0; base[k] && o + 1 < n; k++) {
        int c = toupper((unsigned char)base[k]);
        if (c == '-' || c == '_') c = ' ';
        else if (c != ' ' && (c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT)) c = '?';
        name[o++] = (char)c;
    }
    name[o] = 0;
}

/* Menu stream (ID 'M'): {row, col, ASCII..., 0}*, $FF. 20x18 visible cells. */
static int menu_put(uint8_t *out, int o, int row, int col, const char *text)
{
    out[o++] = (uint8_t)row; out[o++] = (uint8_t)col;
    while (*text && col++ < 20) out[o++] = (uint8_t)*text++;
    out[o++] = 0;
    return o;
}

static int build_menu_stream(uint8_t *out, const char **payloads, int npayload)
{
    static const char *const btn[] = { "A", "B", "START" };
    char line[64], name[40];
    int o = 0, i;
    o = menu_put(out, o, 1, 2, "HOMEBREW LOADER");
    for (i = 0; i < npayload && i < 3; i++) {
        payload_name(payloads[i], name, sizeof name);
        snprintf(line, sizeof line, "%s: %s", btn[i], name);
        o = menu_put(out, o, 5 + 2 * i, 1, line);
    }
    if (npayload == 0) o = menu_put(out, o, 5, 1, "NO PAYLOADS");
    static const char *const prompt[] = { "PRESS A", "PRESS A OR B", "PRESS A, B OR START" };
    if (npayload > 0) o = menu_put(out, o, 15, 1, prompt[(npayload < 3 ? npayload : 3) - 1]);
    out[o++] = 0xFF;
    return o;
}

/* Menu: font + text as two tagged streams, sent right after the loader copies;
 * the loader draws them, then waits for the button. */
/* The menu stream, padded after its $FF terminator (the GB stops reading there)
 * until its last chunk's ACK (CK_lo) cannot be mistaken for a REQ id 1..3
 * (send_stream). */
static int build_menu_stream_safe(uint8_t *out, const char **payloads, int npayload)
{
    uint8_t fr[FRAME_MAX];
    int mn = build_menu_stream(out, payloads, npayload), fn, lastoff;
    for (;;) {
        lastoff = (mn - 1) / CHUNK_RAW * CHUNK_RAW;
        fn = build_frame(out + lastoff, mn - lastoff, 1, ID_MENU,
                         (uint8_t)(lastoff / CHUNK_RAW | 0x80), fr, NULL);
        if (fr[fn - 2] > 3) return mn;
        out[mn++] = 0x10;
    }
}

/* Menu: font + text as two streams, sent right after the loader; the loader
 * draws them, then waits for the button. Returns the REQ id if it arrived
 * during the menu's ACK windows, 0 if not (then wait for it), -1 on failure. */
static int send_menu(const char **payloads, int npayload)
{
    static uint8_t font[3 + 8 * 64], menu[512];
    int fn = build_font_stream(font), mn = build_menu_stream_safe(menu, payloads, npayload);
    if (send_stream(font, fn, TRIES, 1, ID_FONT, 0) < 0) return -1;
    return send_stream(menu, mn, TRIES, 1, ID_MENU, npayload < 1 ? 0 : npayload > 3 ? 3 : npayload);
}

/* Write one frame (or raw blob) as an avra .db table + length .equ. */
static void inc_table(FILE *f, const char *label, const char *lenname,
                      const uint8_t *d, int n, const char *comment)
{
    int i;
    fprintf(f, "\n; %s\n.equ %s = %d\n%s:\n", comment, lenname, n, label);
    for (i = 0; i < n; i++)
        fprintf(f, "%s0x%02X%s", i % 16 ? "" : "\t.db ", d[i],
                i % 16 == 15 || i == n - 1 ? (i == n - 1 && n % 2 ? ", 0x00\n" : "\n") : ", ");
}

/* gbcpop attiny-inc <out.inc> <bootstrap.bin> <loader.bin> <payload>...
 * Export everything the standalone ATtiny85 launcher sends, precomputed, as
 * avra tables: the raw bootstrap (Stage 0 upload) and complete wire frames
 * (build_frame) for the loader, menu font, menu text and each payload's
 * manifest + body. Same code paths as `gbcpop loader`, so both hosts send
 * identical bytes. No IR. */
static int cmd_attiny_inc(const char *out_path, const char *boot_path,
                          const char *loader_path, const char **payloads, int npayload)
{
    static uint8_t boot[512], loader[4096], frame[16384], buf[3 + 8 * 64 + 512];
    static struct Payload pl;
    uint8_t man[4 * PL_MAXSEG + 4];
    int bn = read_file(boot_path, boot, sizeof boot), ln = read_file(loader_path, loader, sizeof loader);
    int i, n, fn, total = 0;
    FILE *f;
    char label[32], lenname[32], cmt[300];
    if (bn <= 0 || ln <= 0) return 1;
    if (bn > 512) { ERR("bootstrap %d B > 512\n", bn); return 1; }
    if (npayload < 1 || npayload > 3) { ERR("attiny-inc: need 1..3 payloads (A/B/Start)\n"); return 1; }
    f = fopen(out_path, "w");
    if (!f) { ERR("cannot write %s\n", out_path); return 1; }
    fprintf(f, "; GENERATED by `gbcpop attiny-inc` -- do not edit.\n"
               "; Each stream = its chunk frames back to back: $55 $55 START ID SEQ len16 body CK16\n"
               "; (loader-protocol doc; START $3D = RLE; SEQ bit 7 = last chunk). The GB ACKs a\n"
               "; chunk with its CK_lo = the frame's second-to-last byte.\n");
    inc_table(f, "boot_data", "BOOT_LEN", boot, bn, "Stage-0 bootstrap, raw (RPC upload to $C000)");
    total += bn;
    fn = build_stream_frames(loader, ln, 0, ID_LOADER, frame);   /* bootstrap has no RLE */
    inc_table(f, "loader_frame", "LOADER_FRAME_LEN", frame, fn, "loader (raw frame)");
    total += fn;
    n = build_font_stream(buf);
    fn = build_stream_frames(buf, n, 1, ID_FONT, frame);
    inc_table(f, "font_frame", "FONT_FRAME_LEN", frame, fn, "menu font stream ('F')");
    total += fn;
    n = build_menu_stream_safe(buf, payloads, npayload);
    fn = build_stream_frames(buf, n, 1, ID_MENU, frame);
    inc_table(f, "menu_frame", "MENU_FRAME_LEN", frame, fn, "menu text stream ('M')");
    total += fn;
    fprintf(f, "\n.equ NPAYLOAD = %d\n", npayload);
    for (i = 0; i < npayload; i++) {
        if (parse_payload(payloads[i], &pl) < 0) { fclose(f); return 1; }
        n = build_manifest(&pl, man);
        fn = build_stream_frames(man, n, 1, ID_MANIFEST, frame);
        snprintf(label, sizeof label, "pl%d_man", i + 1);
        snprintf(lenname, sizeof lenname, "PL%d_MAN_LEN", i + 1);
        snprintf(cmt, sizeof cmt, "payload %d manifest: %s", i + 1, payloads[i]);
        inc_table(f, label, lenname, frame, fn, cmt);
        total += fn;
        fn = build_stream_frames(pl.body, pl.bodylen, 1, ID_BODY, frame);
        snprintf(label, sizeof label, "pl%d_body", i + 1);
        snprintf(lenname, sizeof lenname, "PL%d_BODY_LEN", i + 1);
        snprintf(cmt, sizeof cmt, "payload %d body (%d bytes raw)", i + 1, pl.bodylen);
        inc_table(f, label, lenname, frame, fn, cmt);
        total += fn;
    }
    /* REQ id -> frames: 4 words each (word addresses, as avra labels are) */
    fprintf(f, "\n; REQ id 1..NPAYLOAD -> manifest frame, len, body frame, len\npayload_table:\n");
    for (i = 0; i < npayload; i++)
        fprintf(f, "\t.dw pl%d_man, PL%d_MAN_LEN, pl%d_body, PL%d_BODY_LEN\n", i + 1, i + 1, i + 1, i + 1);
    fclose(f);
    printf("wrote %s: %d bytes of tables (bootstrap %d, %d payload(s))\n", out_path, total, bn, npayload);
    return 0;
}

/* gbcpop loader <bootstrap.bin> <loader.bin> [payload1 [payload2 ...]]
 * Full launch: RPC-upload the bootstrap into $C000 and jump (Stage 0), feed the
 * loader over IR (Stage 1), read the loader's REQ byte, then stream the manifest
 * + bodies for the requested payload (Stage 2). Payloads are manifest.txt files
 * (multi-segment) or bare .bin blobs; the REQ id (1-based) selects one. Like the
 * ATtiny, it then keeps listening: a payload that returns to the menu (SELECT)
 * sends the next REQ. Exits after LOADER_IDLE_US without one. */
#define LOADER_IDLE_US 60000000u
static int cmd_loader(const char *boot_path, const char *loader_path,
                      const char **payloads, int npayload)
{
    static uint8_t boot[512], loader[4096];
    static struct Payload pl;
    int bn = read_file(boot_path, boot, sizeof boot);
    int ln = read_file(loader_path, loader, sizeof loader);
    int choice;
    (void)&ms_send_hello; (void)&ms_wait_hello;   /* reserved, unused */
    if (bn <= 0 || ln <= 0) return 1;
    if (bn > 512) { ERR("bootstrap %d B > 512\n", bn); return 1; }

    printf("Stage 0: uploading %d-byte bootstrap, jumping...\n", bn);
    if (loader_stage0(boot, bn) < 0) return 1;

    /* Tell the user what to do before the feed: the menu appears while we are
     * still busy here, so the instruction must come first. Pressing the button
     * any time after the menu appears is safe (do_transfer waits for the line
     * to be idle before REQ; send_stream takes a REQ in place of the menu ACK). */
    printf("Stage 1: feeding %d-byte loader (chunked, ACKed)...\n", ln);
    fprintf(stderr, "  >>> when the menu appears, press A "
                    "(payload 1) / B (2) / Start (3) <<<\n");
    ms_idle(20000.0);
    if (send_stream(loader, ln, TRIES, 0, ID_LOADER, 0) < 0) {   /* bootstrap has no RLE */
        ERR("loader feed failed\n");
        return 1;
    }

    printf("Stage 1: sending menu font (%d glyphs) + menu text...\n", FONT_COUNT);
    choice = send_menu(payloads, npayload);
    if (choice < 0) { ERR("menu transfer failed\n"); return 1; }

    /* The loader's menu waits for a joypad press, so REQ can arrive seconds later
     * (each ms_rx_byte self-times-out in ~30 ms with no edges). Poll up to 20 s,
     * unless the REQ already came in place of the menu ACK (choice > 0). */
    /* A lost REQ or a broken-off transfer is recovered by the GB: it gives up
     * after ~0.5 s of silence and sends the REQ once more, then returns to its
     * menu, so we just keep listening (docs: loader protocol §4). */
    printf("Stage 2: waiting for your button press (REQ)...\n");
    for (;;) {
        if (choice == 0) choice = wait_req(20000000u);
        if (choice < 0) { ERR("no REQ byte from loader within 20 s (did the loader boot / a button get pressed?)\n"); return 1; }
        if (test_drop_req(choice)) { choice = 0; continue; }
        printf("  loader requested payload id %d\n", choice);
        if (npayload == 0) { printf("  (no payloads given; nothing to send)\n"); return 0; }
        if (choice < 1 || choice > npayload) {
            ERR("no payload configured for id %d (have %d); press another button\n", choice, npayload);
            choice = 0;
            continue;
        }
        if (parse_payload(payloads[choice - 1], &pl) < 0) return 1;
        printf("  sending '%s': %d seg(s), %d body bytes, entry $%04X, flags $%02X\n",
               payloads[choice - 1], pl.nseg, pl.bodylen, pl.entry, pl.flags);
        if (send_payload_frame(&pl) == 0) {
            printf("Stage 3: '%s' delivered; the loader has applied it and jumped.\n",
                   payloads[choice - 1]);
            printf("  listening for the next REQ (SELECT in the payload returns to the "
                   "menu); exits after %u s without one\n", LOADER_IDLE_US / 1000000u);
            choice = wait_req(LOADER_IDLE_US);
            if (choice < 0) { printf("No further REQ. Exiting.\n"); return 0; }
            continue;
        }
        fprintf(stderr, "  the Game Boy sends its REQ again or returns to its menu\n");
        choice = 0;
    }
}
#else
static int cmd_payloadcheck(const char *p){ (void)p; ERR("payloadcheck needs the SHM or direct GPIO build\n"); return 1; }
static int cmd_attiny_inc(const char *o, const char *b, const char *l, const char **p, int n)
{ (void)o; (void)b; (void)l; (void)p; (void)n; ERR("attiny-inc needs the SHM or direct GPIO build\n"); return 1; }
static int cmd_loader(const char *a, const char *b, const char **c, int d)
{
    (void)a; (void)b; (void)c; (void)d;
    ERR("the 'loader' command needs the SHM (VBA) or the direct GPIO build (plain make)\n");
    return 1;
}
#endif


#ifdef USE_SHM
static int cmd_loader_echo(void)
{
    static const uint8_t tests[] = {0x5A,0x33,0xAA,0x00,0xFF,0x81,0x49,0x52};
    int i, ok=0, tot=(int)sizeof tests;
    printf("mark/space echo test (needs a GB running loader/irtest.gb)\n");
    for (i=0;i<tot;i++){
        int r=-1, try_;
        for (try_=0; try_<6; try_++){        /* stop-and-wait: retry on miss */
            ms_tx_byte(tests[i]);
            r = ms_rx_byte();
            if (r==tests[i]) break;
        }
        printf("  send $%02X -> %s$%02X  %s (%d tr%s)\n", tests[i],
               r<0?"timeout ":"", (unsigned)(r<0?0:r),
               (r==tests[i])?"OK":"MISMATCH", try_+1, try_?"ies":"y");
        if (r==tests[i]) ok++;
    }
    printf("  %d/%d echoed correctly\n", ok, tot);
    return ok==tot?0:1;
}
#define ARQ_CHUNK 8
#define ARQ_ACK 0x06
#define ARQ_NAK 0x15
/* stop-and-wait ARQ send: chunks [seq,len,data,ck] + a final len=0; retry on
 * NAK/timeout. Returns 0 on success. */
static int arq_send(const uint8_t *data, int n)
{
    int off = 0, seq = 0;
    for (;;) {
        int len = n - off; if (len > ARQ_CHUNK) len = ARQ_CHUNK;
        int tries;
        for (tries = 0; tries < 8; tries++) {
            unsigned sum = (unsigned)(seq + len);
            int i, r;
            ms_tx_byte((uint8_t)seq);
            ms_tx_byte((uint8_t)len);
            for (i = 0; i < len; i++) { ms_tx_byte(data[off + i]); sum += data[off + i]; }
            ms_tx_byte((uint8_t)(-sum));
            r = ms_rx_byte();
            if (getenv("ARQDBG"))
                fprintf(stderr, "  arq seq=%d len=%d try=%d -> reply %s%d\n",
                        seq, len, tries, r < 0 ? "timeout " : "0x", r < 0 ? 0 : r);
            if (r == ARQ_ACK) break;
        }
        if (tries == 8) { ERR("arq: no ACK for seq %d\n", seq); return -1; }
        if (len == 0) return 0;               /* the end chunk was ACKed */
        off += len; seq = (seq + 1) & 0xFF;
    }
}

/* Raw edge logger: dump every light edge the host reads from the GB ring for
 * `ms` milliseconds, with cT and inter-edge gaps. Symmetric to VBA's RX dump. */
static int cmd_listen(int ms)
{
    struct ShmSlot s;
    uint32_t t0 = now_us();
    long long prev = -1;
    int n = 0;
    printf("listen: logging GB->host edges for %d ms\n", ms);
    shm_try_peer();
    while ((uint32_t)(now_us() - t0) < (uint32_t)ms * 1000u) {
        if (shm_next(&s)) {
            long long gap = prev < 0 ? 0 : (long long)s.cT - prev;
            printf("  [cT=%lld] %s  gap=%lld cT (~%.1f us)\n",
                   (long long)s.cT, SHM_RX_LIGHT(s.led_on) ? "LIGHT" : "dark ",
                   gap, (double)gap * MC2US);
            prev = (long long)s.cT;
            n++;
        }
    }
    printf("  %d edges\n", n);
    return 0;
}

/* Full Stage-1..3 chain test (Stage-0 RPC replaced by boottest.gb booting the
 * bootstrap-receive at $100). Feeds loader.bin, then plays the host side of the
 * transfer: reads the loader's REQ byte and sends a manifest+body that relocates
 * the blinkstub to $C500 and jumps -> a correct chain shows the slow blink. */
static int cmd_loadertest(const char *loader_path, const char *payload_path)
{
    static const uint8_t stub[] = {
        0x3E,0xC1,0xE0,0x56,0x06,0x78,0x05,0x20,0xFD,0x3E,0xC0,0xE0,0x56,
        0x01,0x00,0x04,0x0B,0x78,0xB1,0x20,0xFB,0x18,0xE9 };
    static uint8_t loader[4096];
    static struct Payload pl;
    int ln = read_file(loader_path, loader, sizeof loader), req, rounds;
    if (ln <= 0) return 1;

    if (getenv("GBCPOP_TEST_MIDJOIN")) {
        /* Regression for false STARTs: what a receiver sees when it joins the
         * feed mid-copy -- the tail of a copy, no preamble. Loader code holds
         * stray $3C/$3D bytes that must not be taken as START. */
        int i;
        printf("chain: TEST_MIDJOIN: sending loader bytes 40..%d unframed first\n", ln - 1);
        for (i = 40; i < ln; i++) ms_tx_byte(loader[i]);
    }
    printf("chain: feeding %d-byte loader (chunked, ACKed)\n", ln);
    ms_idle(20000.0);
    if (send_stream(loader, ln, TRIES, 0, ID_LOADER, 0) < 0) {   /* bootstrap has no RLE */
        ERR("loader feed failed\n");
        return 1;
    }
    printf("chain: sending menu font + text\n");
    req = send_menu(payload_path ? &payload_path : NULL, payload_path ? 1 : 0);
    if (req < 0) { ERR("chain: menu transfer failed\n"); return 1; }

    /* GBCPOP_TEST_RETURN=n: the first n launches get a stub that returns to the
     * loader's menu at once (LOADER_RETURN); the test loader then asks again. */
    for (rounds = getenv("GBCPOP_TEST_RETURN") ? atoi(getenv("GBCPOP_TEST_RETURN")) : 0;
         rounds > 0; rounds--) {
        static const uint8_t ret_stub[] = { 0x3E,0x07,0xE0,0x70,0xC3,0x03,0xD0 };
        uint32_t t0;
        int hb = -1;
        if (req == 0) req = wait_req(3000000u);
        if (req < 0) { ERR("chain: TEST_RETURN: no REQ\n"); return 1; }
        printf("chain: TEST_RETURN: REQ %d -> stub that returns to the menu\n", req);
        pl.nseg = 1; pl.dest[0] = 0xC500; pl.len[0] = (uint16_t)sizeof ret_stub;
        pl.entry = 0xC500; pl.flags = 0; pl.bodylen = (int)sizeof ret_stub;
        memcpy(pl.body, ret_stub, sizeof ret_stub);
        if (send_payload_frame(&pl) < 0) { ERR("chain: TEST_RETURN: send failed\n"); return 1; }
        for (t0 = now_us(); (uint32_t)(now_us() - t0) < 1000000u && hb != 0xAA; )
            hb = ms_rx_byte();
        printf("chain: TEST_RETURN: heartbeat %s\n", hb == 0xAA ? "$AA received" : "MISSING");
        if (hb != 0xAA) return 1;
        req = 0;
    }

    printf("chain: waiting for the loader's REQ byte...\n");
    for (;;) {
        if (req == 0) req = wait_req(3000000u);   /* the test loader waits for idle first */
        if (req < 0 || !test_drop_req(req)) break;
        req = 0;                                  /* dropped: it asks again by itself */
    }
    printf("chain: loader requested payload id %d\n", req);

    if (payload_path) {                          /* real parser + frame sender */
        if (parse_payload(payload_path, &pl) < 0) return 1;
        printf("chain: sending '%s': %d seg(s), entry $%04X, %d body bytes\n",
               payload_path, pl.nseg, pl.entry, pl.bodylen);
    } else {                                     /* default: 1 seg blinkstub -> $C500 */
        pl.nseg = 1; pl.dest[0] = 0xC500; pl.len[0] = (uint16_t)sizeof stub;
        pl.entry = 0xC500; pl.flags = 0; pl.bodylen = (int)sizeof stub;
        memcpy(pl.body, stub, sizeof stub);
        printf("chain: sending default 1-seg blinkstub -> $C500\n");
    }
    send_payload_frame(&pl);
    {   /* stay on the link (VBA's clock only runs with us attached) and wait
         * for the test loader's $AA "transfer OK, applying" heartbeat */
        uint32_t t0 = now_us();
        int hb = -1;
        while ((uint32_t)(now_us() - t0) < 1000000u && hb != 0xAA) hb = ms_rx_byte();
        printf("chain: heartbeat %s\n", hb == 0xAA ? "$AA received" : "MISSING");
    }
    printf("chain: done; expect the slow blink in the VBA TX log\n");
    return 0;
}

static int cmd_boottest(void)
{
    /* Send the blinkstub body framed as preamble+START+len16+body, to exercise
     * the Stage-0 bootstrap receive (skip-to-START, read length, stream, jump). */
    static const uint8_t stub[] = {
        0x3E,0xC1,0xE0,0x56,0x06,0x78,0x05,0x20,0xFD,0x3E,0xC0,0xE0,0x56,
        0x01,0x00,0x04,0x0B,0x78,0xB1,0x20,0xFB,0x18,0xE9 };
    int len = (int)sizeof stub, i;
    ms_tx_byte(0x55); ms_tx_byte(0x55); ms_tx_byte(0x55);   /* preamble */
    ms_tx_byte(0x3C);                                       /* START */
    ms_tx_byte((uint8_t)(len & 0xFF)); ms_tx_byte((uint8_t)(len >> 8));
    for (i = 0; i < len; i++) ms_tx_byte(stub[i]);
    printf("boottest: sent preamble+START+len=%d+blinkstub; expect slow blink in log\n", len);
    return 0;
}

static int cmd_cal(void)
{
    int i;
    printf("cal: sending 0x0F x40 (4 short + 4 long spaces); GB TX -> VBA log\n");
    for (i = 0; i < 40; i++) ms_tx_byte(0x0F);
    return 0;
}

static int cmd_loader_stream(void)
{
    /* Send 12 known bytes; GB receives all then echoes. Decode GB echo from the
     * VBA TX log (polarity-aware) to verify end-to-end RX streaming. */
    uint8_t tx[12] = {0x55,0x55,0x55,0x3C,0x00,0xFF,0x5A,0xA5,0x01,0x80,0x33,0x7E};
    int n = 12, i;
    for (i = 0; i < n; i++) ms_tx_byte(tx[i]);
    printf("stream: sent 12 bytes {55 55 55 3C 00 FF 5A A5 01 80 33 7E}\n");
    return 0;
}
static int cmd_loader_bulk(void)
{
    uint8_t buf[64];
    int i, n = 24, r;
    unsigned sum = 0;
    for (i = 0; i < n; i++) { buf[i] = (uint8_t)(0x13 * i + 5); sum += buf[i]; }
    printf("bulk (ARQ): send %d bytes, expect checksum $%02X\n", n, sum & 0xFF);
    if (arq_send(buf, n) < 0) return 1;
    for (i = 0; i < 30; i++) {
        r = ms_rx_byte();
        if (r >= 0) { printf("  checksum $%02X %s\n", (unsigned)r,
                             r == (int)(sum & 0xFF) ? "OK" : "MISMATCH");
                      return r == (int)(sum & 0xFF) ? 0 : 1; }
    }
    printf("  no checksum (timeout)\n");
    return 1;
}
#else
static int cmd_loader_echo(void){ ERR("loader-echo needs the SHM build\n"); return 1; }
static int cmd_loader_bulk(void){ ERR("loader-bulk needs the SHM build\n"); return 1; }
static int cmd_loader_stream(void){ ERR("loader-stream needs the SHM build\n"); return 1; }
static int cmd_listen(int ms){ (void)ms; ERR("listen needs the SHM build\n"); return 1; }
static int cmd_cal(void){ ERR("cal needs the SHM build\n"); return 1; }
static int cmd_boottest(void){ ERR("boottest needs the SHM build\n"); return 1; }
static int cmd_loadertest(const char *p,const char *q){ (void)p;(void)q; ERR("loadertest needs the SHM build\n"); return 1; }
#endif

static int cmd_run(const char *file, uint16_t at)
{
    static uint8_t blob[0x0E00], back[256];
    uint8_t vbk0 = 0;
    size_t n;
    int off;
    FILE *f = fopen(file, "rb");

    if (!f) { ERR("cannot read %s\n", file); return 1; }
    n = fread(blob, 1, sizeof blob, f);
    fclose(f);
    if (n == 0) { ERR("%s is empty\n", file); return 1; }
    if (at < 0xC000 || at + n > 0xCE8C) {
        ERR("$%04X..$%04X must lie within $C000..$CE8B (TCG IR buffer / stack above)\n",
            at, (unsigned)(at + n - 1));
        return 1;
    }

    printf("Waiting for the Game Boy in any IR screen...\n");
    fflush(stdout);
    if (sync_as_sender(400, RX_LONG_US / 400) < 0) { ERR("no answer to $AA\n"); return 1; }

    for (off = 0; off < (int)n; off += 128) {
        int chunk = (int)n - off < 128 ? (int)n - off : 128;
        if (remote_write(blob + off, (uint16_t)(at + off), chunk) < 0) return 1;
        printf("\r  uploading %4d / %d bytes to $%04X", off + chunk, (int)n, at);
        fflush(stdout);
    }
    putchar('\n');

    for (off = 0; off < (int)n; off += 256) {
        int chunk = (int)n - off < 256 ? (int)n - off : 256;
        if (remote_read((uint16_t)(at + off), back, chunk) < 0) return 1;
        if (memcmp(back, blob + off, (size_t)chunk)) {
            ERR("verify failed in $%04X..$%04X, not jumping\n",
                at + off, at + off + chunk - 1);
            return 1;
        }
    }
    printf("  verified.\n");

    /* Homebrew written for a fresh boot assumes VRAM bank 0 (snake builds its
     * tiles at $8000 without selecting a bank); the TCG may have left bank 1. */
    if (remote_write(&vbk0, 0xFF4F, 1) < 0) return 1;

    printf("Jumping to $%04X...\n", at);
    if (remote_call(at, 0, 0) < 0) return 1;
    printf("Done: the Game Boy is now running %s.\n", file);
    return 0;
}

static const char *cart_type_name(uint8_t t)
{
    switch (t) {
    case 0x00: return "ROM only";
    case 0x01: case 0x02: case 0x03: return "MBC1";
    case 0x05: case 0x06: return "MBC2";
    case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: return "MBC3";
    case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: return "MBC5";
    default: return "unknown";
    }
}

static int ram_banks_from_header(uint8_t code)
{
    switch (code) {
    case 0x00: return 0;
    case 0x01: case 0x02: return 1;     /* 2 or 8 KiB: one window */
    case 0x03: return 4;                /* 32 KiB  */
    case 0x04: return 16;               /* 128 KiB */
    case 0x05: return 8;                /* 64 KiB  */
    default:   return 4;
    }
}

/* Read the header of whatever cartridge is in the slot now. */
static int bridge_header(int *banks_out)
{
    uint8_t h[32];
    int i;
    if (bridge_read(BR_NOBANK, 0x0134, 24, h) < 0) return -1;
    printf("  title     \"");
    for (i = 0; i < 16 && h[i]; i++) putchar(h[i] >= 0x20 && h[i] < 0x7F ? h[i] : '.');
    printf("\"\n");
    printf("  cart type $%02X (%s), ROM $%02X, RAM $%02X (%d bank%s)\n",
           h[0x13], cart_type_name(h[0x13]), h[0x14], h[0x15],
           ram_banks_from_header(h[0x15]), ram_banks_from_header(h[0x15]) == 1 ? "" : "s");
    if (banks_out) *banks_out = ram_banks_from_header(h[0x15]);
    return 0;
}

static void prompt_swap(void)
{
    printf("\n");
    printf("  ----------------------------------------------------------\n");
    printf("   Swap the cartridge now. The Game Boy stays powered on and\n");
    printf("   keeps running the bridge from work RAM; the screen will\n");
    printf("   hold the last Card Pop! frame.\n");
    printf("  ----------------------------------------------------------\n");
    printf("  Press Enter when the new cartridge is seated: ");
    fflush(stdout);
    while (getchar() != '\n') { }
}

static int cmd_swap_dump(const char *path, int do_upload, int do_prompt)
{
    static uint8_t img[0x20000];
    int banks = 4, bank, off, total;
    FILE *f;

    if (do_upload && cmd_upload() != 0) return 1;
    if (do_prompt) prompt_swap();
    if (bridge_wait(20) < 0) { ERR("bridge gone — did the swap glitch it?\n"); return 1; }
    printf("\nCartridge in the slot:\n");
    if (bridge_header(&banks) < 0) return 1;
    if (banks == 0) { ERR("this cartridge reports no save RAM\n"); return 1; }
    if (banks > 16) banks = 16;

    if (bridge_mapper(0x0000, 0x0A) < 0) { ERR("could not unlock its SRAM\n"); return 1; }
    total = banks * 0x2000;
    for (bank = 0; bank < banks; bank++)
        for (off = 0; off < 0x2000; off += 256) {
            if (bridge_read((uint8_t)bank, (uint16_t)(SRAM_BASE + off), 0,
                            img + bank * 0x2000 + off) < 0) return 1;
            printf("\r  reading  %6d / %d bytes", bank * 0x2000 + off + 256, total);
            fflush(stdout);
        }
    bridge_mapper(0x0000, 0x00);                 /* lock it again */
    putchar('\n');

    f = fopen(path, "wb");
    if (!f) { ERR("cannot write %s\n", path); return 1; }
    fwrite(img, 1, (size_t)total, f);
    fclose(f);
    printf("Saved %d bytes to %s\n", total, path);
    return 0;
}

static int cmd_swap_push(const char *path, int do_upload, int do_prompt)
{
    static uint8_t img[0x20000];
    long size;
    int banks, bank, off;
    FILE *f = fopen(path, "rb");

    if (!f) { ERR("cannot read %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); rewind(f);
    if (size <= 0 || size % 0x2000 || size > 0x20000) {
        ERR("%s is %ld bytes; expected a multiple of 8192\n", path, size);
        fclose(f); return 1;
    }
    if (fread(img, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 1; }
    fclose(f);
    banks = (int)(size / 0x2000);

    if (do_upload && cmd_upload() != 0) return 1;
    if (do_prompt) prompt_swap();
    if (bridge_wait(20) < 0) { ERR("bridge gone\n"); return 1; }
    printf("\nCartridge in the slot:\n");
    if (bridge_header(NULL) < 0) return 1;

    if (bridge_mapper(0x0000, 0x0A) < 0) { ERR("could not unlock its SRAM\n"); return 1; }
    for (bank = 0; bank < banks; bank++)
        for (off = 0; off < 0x2000; off += 128) {
            if (bridge_write((uint8_t)bank, (uint16_t)(SRAM_BASE + off),
                             img + bank * 0x2000 + off, 128) < 0) return 1;
            printf("\r  writing  %6d / %ld bytes", bank * 0x2000 + off + 128, size);
            fflush(stdout);
        }
    bridge_mapper(0x0000, 0x00);
    printf("\nWritten. Power-cycle the Game Boy with this cartridge in.\n");
    return 0;
}

static int cmd_bridge(const char *sub, char **args, int nargs)
{
    if (!strcmp(sub, "ping")) {
        if (bridge_ping(20) < 0) { ERR("no answer\n"); return 1; }
        printf("Bridge alive.\n");
        return 0;
    }
    if (!strcmp(sub, "header")) {
        if (bridge_wait(10) < 0) return 1;
        return bridge_header(NULL) < 0;
    }
    if (!strcmp(sub, "read") && nargs >= 3) {
        uint8_t buf[256];
        int bank = (int)parse_num(args[0]);
        uint16_t addr = (uint16_t)parse_num(args[1]);
        int len = (int)parse_num(args[2]);
        if (len < 1 || len > 256) { ERR("length 1..256\n"); return 1; }
        if (bridge_read((uint8_t)bank, addr, (uint8_t)(len & 0xFF), buf) < 0) return 1;
        hexdump(addr, buf, len);
        return 0;
    }
    if (!strcmp(sub, "mapper") && nargs >= 2) {
        uint16_t addr = (uint16_t)parse_num(args[0]);
        uint8_t v = (uint8_t)parse_num(args[1]);
        if (bridge_mapper(addr, v) < 0) return 1;
        printf("Wrote $%02X to $%04X.\n", v, addr);
        return 0;
    }
    if (!strcmp(sub, "halt")) {
        bridge_cmd(BR_HALT, 0, 0, 0, 8);
        printf("Bridge parked. Power-cycle to recover.\n");
        return 0;
    }
    ERR("bridge <ping|header|read bank addr len|mapper addr val|halt>\n");
    return 1;
}


/*
 * Wiring check. Blinks the LED at a visible rate and reports what the detector
 * sees, so a misnumbered pin or a wrong polarity shows up before any protocol
 * work. Point the LED at the detector (or a phone camera) while it runs.
 */
static int cmd_selftest(void)
{
    int i, lit, dark, saw_change = 0;
    uint8_t probe[4];

    printf("TX  BCM %d (%s)\n", cfg_tx_gpio, cfg_tx_invert ? "active low" : "active high");
    printf("RX  BCM %d (%s, pull %s)\n", cfg_rx_gpio,
           cfg_rx_invert ? "active low" : "active high",
           cfg_rx_pull == 2 ? "up" : cfg_rx_pull == 1 ? "down" : "off");

    for (i = 0; i < 8; i++) {
        int on_hits = 0, off_hits = 0, k;
        gp_write(1);
        for (k = 0; k < 2000; k++) { if (gp_read()) on_hits++; }
        gp_write(0);
        for (k = 0; k < 2000; k++) { if (gp_read()) off_hits++; }
        lit  = on_hits  * 100 / 2000;
        dark = off_hits * 100 / 2000;
        printf("  pass %d: LED on -> detector reads light %3d%% of samples, "
               "LED off -> %3d%%\n", i + 1, lit, dark);
        if (lit > 80 && dark < 20) saw_change = 1;
    }
    gp_write(0);

    if (saw_change)
        printf("\nLoopback looks good: the detector follows the LED.\n");
    else
        printf("\nNo clean loopback. That is expected if the LED and detector are\n"
               "shielded from each other; aim them at one another to test, or check\n"
               "--tx / --rx / --rx-active-high / --rx-pull.\n");

    printf("\nListening 3 s for traffic from a Game Boy...\n");
    for (i = 0; i < 60; i++) {
        int r = recv_byte(50000);
        if (r >= 0) {
            printf("  saw byte $%02X\n", r);
            probe[0] = (uint8_t)r;
            (void)probe;
        }
    }
    printf("Done.\n");
    return 0;
}


/*
 * Logic-analyser view of the IR line: record every pulse for a few seconds
 * without printing, then report widths, gaps and the cell period they imply.
 * This works even when nothing else does -- it needs no protocol at all, just
 * a Game Boy sitting in an IR screen sending $AA at something.
 */
#define TRACE_MAX 20000

static int cmd_trace(int seconds)
{
    static uint32_t rise[TRACE_MAX], fall[TRACE_MAX];
    int n = 0, prev, lvl, i;
    uint32_t t, t_end, pend = 0;
    int pending = 0;
    double sum_w = 0, sum_g = 0;
    int n_w = 0, n_g = 0;

    if (seconds < 1) seconds = 3;
    printf("Recording IR pulses for %d s (nothing is printed until the end,\n"
           "so the sampling loop stays tight). Put the Game Boy in an IR screen.\n",
           seconds);
    fflush(stdout);

    prev = gp_read();
    t_end = hw_now() + (uint32_t)seconds * 1000000u;
    while ((int32_t)(hw_now() - t_end) < 0 && n < TRACE_MAX) {
        lvl = gp_read();
        t = hw_now();
        if (lvl != prev) {
            if (lvl) { pend = t; pending = 1; }
            else if (pending) { rise[n] = pend; fall[n] = t; n++; pending = 0; }
            prev = lvl;
        }
    }

    if (n == 0) {
        printf("\nNo light seen at all on BCM %d.\n"
               "  - is the Game Boy actually transmitting? (Card Pop!, press A)\n"
               "  - check --rx / --rx-active-high / --rx-pull\n"
               "  - run `selftest` to prove the detector responds to our own LED\n",
               cfg_rx_gpio);
        return 1;
    }

    printf("\n%d pulses. Nominal cell is %.2f us, nominal pulse %.0f us.\n\n",
           n, CELL_US, (double)PULSE_US);
    printf("       t (ms)   width us    gap us   cells\n");
    for (i = 0; i < n; i++) {
        double w = (double)(int32_t)(fall[i] - rise[i]);
        double g = i ? (double)(int32_t)(rise[i] - rise[i - 1]) : 0.0;
        double cells = g / CELL_US;
        sum_w += w; n_w++;
        if (i && cells > 0.8 && cells < 1.2) { sum_g += g; n_g++; }
        if (i && g > CELL_US * 12) {
            printf("       ---- idle %.2f ms ----\n", g / 1000.0);
            printf("  %10.3f   %7.1f        --      --\n",
                   (double)(int32_t)(rise[i] - rise[0]) / 1000.0, w);
        } else if (i == 0) {
            printf("  %10.3f   %7.1f        --      --\n", 0.0, w);
        } else {
            printf("  %10.3f   %7.1f   %7.1f   %5.2f%s\n",
                   (double)(int32_t)(rise[i] - rise[0]) / 1000.0, w, g, cells,
                   (cells > 0.8 && cells < 1.2) ? "" : "  *");
        }
        if (i == 400 && n > 420) {
            printf("       ... %d more pulses omitted ...\n", n - 420);
            i = n - 20;
        }
    }

    printf("\n  mean pulse width      %.1f us   (Game Boy sends %.0f us)\n",
           sum_w / n_w, (double)PULSE_US);
    if (n_g) {
        double cell = sum_g / n_g;
        printf("  implied cell period   %.2f us   from %d adjacent-cell gaps\n",
               cell, n_g);
        printf("  vs nominal 440 T      %+.2f %%   (the receiver tolerates about "
               "-2.5%%/+7.5%%)\n", 100.0 * (cell - CELL_US) / CELL_US);
    } else {
        printf("  no adjacent-cell gaps seen -- the pulses are not spaced like\n"
               "  this protocol, or the detector is missing most of them.\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf(
"gbcpop — Pokémon Trading Card Game (GBC) infrared tool\n"
"\n"
"usage: sudo gbcpop [options] <command> [args]\n"
"\n"
"commands:\n"
"  probe                 handshake and report the peer's mode and name\n"
"  peek <addr> <len>     read up to 256 bytes out of the peer's memory\n"
"  poke <addr> <hex>     write bytes into the peer's memory\n"
"  give <id>[,<id>...]   hand the game cards via its \"Receive a card\" screen\n"
"  take                  receive a card list from its \"Send a card\" screen\n"
"  pop [--want <id>]     run a Card Pop! as the master, optionally forcing\n"
"                        the card by choosing our own trainer name\n"
"  homebrew <id>         write a stub into the peer's RAM and run it (adds\n"
"                        a card in any mode; see --addcard)\n"
"  dump-save <file>      read the cartridge's 32 KiB save over IR\n"
"  push-save <file>      write a save image back\n"
"  set-name <text>       change the trainer name in SRAM\n"
"  wipe-save [--yes]     destroy the save marker; next boot reinitialises\n"
"\n"
"  upload                install the resident bridge (payload.bin) and run it\n"
"  run <file> [addr]     upload a raw WRAM image (default $C000), verify, jump\n"
"  loader <bootstrap.bin> <loader.bin> [payload ...]\n"
"                        full launch: bootstrap, loader, menu, then the payload\n"
"                        picked on the GBC (A = 1st, B = 2nd, START = 3rd)\n"
"  bridge <sub> ...      ping | header | read <bank> <addr> <len> |\n"
"                        mapper <addr> <val> | halt\n"
"  swap-dump <file>      upload, prompt for the cartridge swap, dump its SRAM\n"
"  swap-push <file>      write an image into the swapped-in cartridge\n"
"\n"
"  serve                 act as a slave and log every RPC (debugging)\n"
"  monitor               print every byte seen on the IR line\n"
"  predict <a> <b>       no IR: what two trainer names would each receive\n"
"  forge <peer> <id>     no IR: a 16-byte name field that forces <id> on <peer>\n"
"  cards                 list the card ids\n"
"  selftest              blink the LED and watch the detector — check wiring\n"
"  trace [seconds]       record every IR pulse, then report widths, gaps and\n"
"                        the bit-cell period they imply — use this first\n"
"\n"
"options:\n"
"  --tx <gpio>     IR LED GPIO, BCM numbering (default 17)\n"
"  --rx <gpio>     IR detector GPIO, BCM numbering (default 18)\n"
"  --tx-invert     the LED lights when the GPIO is low\n"
"  --rx-invert     the detector reads low when light is present (the default)\n"
"  --rx-active-high  the detector reads high when light is present\n"
"  --rx-pull <m>   up | down | off for the detector input (default up)\n"
"  --name <text>   trainer name we present (default RASPI)\n"
"  --addcard <a>   AddCardToCollection address ($1CCE EU, $1D6E US)\n"
"  --stub-at <a>   where \"homebrew\" parks its stub (default $C300)\n"
"  --payload <f>   resident bridge image (default payload.bin)\n"
"  --payload-at <a>  where the bridge is loaded (default $C700)\n"
"  --no-upload     swap-* : the bridge is already running\n"
"  --upload        swap-push : install the bridge first\n"
"  --no-prompt / --prompt   swap-* : wait for Enter before talking or not\n"
"                  (swap-dump prompts by default, swap-push does not)\n"
"  --yes           wipe-save : skip the confirmation\n"
"  --rt-prio <n>   SCHED_FIFO priority, 0 disables (default 80)\n"
"  --peri-base <a> BCM peripheral base in hex, if autodetection is wrong\n"
"                  (Pi 1/Zero $20000000, Pi 2/3 $3F000000, Pi 4 $FE000000)\n"
"  --clock mono    use CLOCK_MONOTONIC instead of the BCM system timer\n"
"  -v              protocol steps; -vv adds per-byte pulse timing, dumped at\n"
"                  the end; -vvv prints it live (breaks block transfers, but\n"
"                  fine for probe / selftest / monitor)\n"
"\n"
"Numbers accept 0x.. or $.. or decimal.\n");
}

static long parse_num(const char *s)
{
    if (!s) return -1;
    if (s[0] == '$') return strtol(s + 1, NULL, 16);
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return strtol(s + 2, NULL, 16);
    return strtol(s, NULL, 0);
}

static void hw_close(void);
static void on_sigint(int sig) { (void)sig; stop_flag = 1; hw_close(); _exit(1); }

int main(int argc, char **argv)
{
    int i, rc = 0, want = -1, need_gpio = 1;
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: progress shows live even
                                           when stdout is not a tty */
    int opt_upload = -1, opt_prompt = -1, opt_yes = 0;
    const char *cmd = NULL;
    char *args[8];
    int nargs = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tx") && i + 1 < argc)         cfg_tx_gpio = (int)parse_num(argv[++i]);
        else if (!strcmp(argv[i], "--rx") && i + 1 < argc)    cfg_rx_gpio = (int)parse_num(argv[++i]);
        else if (!strcmp(argv[i], "--tx-invert"))             cfg_tx_invert = 1;
        else if (!strcmp(argv[i], "--rx-invert"))             cfg_rx_invert = 1;
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)  cfg_name = argv[++i];
        else if (!strcmp(argv[i], "--want") && i + 1 < argc)  want = (int)parse_num(argv[++i]);
        else if (!strcmp(argv[i], "--addcard") && i + 1 < argc) cfg_addcard = (uint16_t)parse_num(argv[++i]);
        else if (!strcmp(argv[i], "--stub-at") && i + 1 < argc) cfg_stub_at = (uint16_t)parse_num(argv[++i]);
        else if (!strcmp(argv[i], "--payload") && i + 1 < argc)  cfg_payload = argv[++i];
        else if (!strcmp(argv[i], "--payload-at") && i + 1 < argc) cfg_payload_at = (uint16_t)parse_num(argv[++i]);
        else if (!strcmp(argv[i], "--no-upload"))  opt_upload = 0;
        else if (!strcmp(argv[i], "--upload"))     opt_upload = 1;
        else if (!strcmp(argv[i], "--no-prompt"))  opt_prompt = 0;
        else if (!strcmp(argv[i], "--prompt"))     opt_prompt = 1;
        else if (!strcmp(argv[i], "--yes"))        opt_yes = 1;
        else if (!strcmp(argv[i], "--rt-prio") && i + 1 < argc) cfg_rt_prio = (int)parse_num(argv[++i]);
        else if (!strcmp(argv[i], "--rx-active-high")) cfg_rx_invert = 0;
        else if (!strcmp(argv[i], "--rx-pull") && i + 1 < argc) {
            const char *v = argv[++i];
            cfg_rx_pull = !strcmp(v, "up") ? 2 : !strcmp(v, "down") ? 1 : 0;
        }
#if !defined(USE_PIGPIO) && !defined(USE_WIRINGPI) && !defined(USE_SHM)   /* direct backend only */
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc)
            cfg_clock_st = strcmp(argv[++i], "mono") != 0;
        else if (!strcmp(argv[i], "--peri-base") && i + 1 < argc)
        {
            const char *v = argv[++i];
            cfg_peri_base = strtoul(v + (*v == '$' ? 1 : 0), NULL, 16);
        }
#endif
#ifdef USE_SHM                                       /* shm backend only */
        else if (!strcmp(argv[i], "--local") && i + 1 < argc)  cfg_shm_local = argv[++i];
        else if (!strcmp(argv[i], "--peer") && i + 1 < argc)   cfg_shm_peer = argv[++i];
#endif
        else if (!strcmp(argv[i], "-v"))                      cfg_verbose++;
        else if (!strcmp(argv[i], "-vv"))                     cfg_verbose = 2;
        else if (!strcmp(argv[i], "-vvv"))                    cfg_verbose = 3;
        else if (!strcmp(argv[i], "--debug"))                 cfg_verbose = 2;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!cmd) cmd = argv[i];
        else if (nargs < 8) args[nargs++] = argv[i];
    }
    if (!cmd) { usage(); return 1; }

    if (!strcmp(cmd, "cards")) {
        for (i = 1; i <= NUM_CARDS; i++) printf("  $%02X  %s\n", i, card_name[i]);
        return 0;
    }
    if (!strcmp(cmd, "forge")) {
        uint8_t peer[16], me[16], sg, xg, sm, xm, d = 0, e = 0;
        int dd, ee, found = 0, target;
        if (nargs < 2) { ERR("forge <peer name> <card id>\n"); return 1; }
        target = (int)parse_num(args[1]);
        memset(peer, 0, 16); strncpy((char *)peer, args[0], 15);
        name_digest(peer, &sg, &xg);
        for (ee = 0; ee < 256 && !found; ee++)
            for (dd = (ee & 1); dd < 256; dd += 2)     /* parity-reachable only */
                if (pick_card((uint8_t)dd, (uint8_t)ee) == target) {
                    d = (uint8_t)dd; e = (uint8_t)ee; found = 1; break;
                }
        if (!found) { ERR("card $%02X is not reachable by Card Pop!\n", target); return 1; }
        if (forge_name(me, cfg_name, (uint8_t)(sg - e), (uint8_t)(xg - d)) < 0) {
            ERR("no 16-byte name reaches d=$%02X e=$%02X\n", d, e); return 1;
        }
        name_digest(me, &sm, &xm);
        printf("peer \"%s\"  sum $%02X xor $%02X\n", args[0], sg, xg);
        printf("target d $%02X e $%02X -> $%02X %s\n", d, e, target, cardname(target));
        printf("forged name field (prefix \"%s\"): sum $%02X xor $%02X\n", cfg_name, sm, xm);
        hexdump(0xC590, me, 16);
        printf("verify: peer receives $%02X (%s)\n",
               pick_card((uint8_t)(xg - xm), (uint8_t)(sg - sm)),
               cardname(pick_card((uint8_t)(xg - xm), (uint8_t)(sg - sm))));
        return 0;
    }
    if (!strcmp(cmd, "predict")) {
        if (nargs < 2) { ERR("predict needs two names\n"); return 1; }
        return cmd_predict(args[0], args[1]);
    }

    if (need_gpio) {
        signal(SIGINT, on_sigint);
        if (hw_init() < 0) return 1;
    }

    if      (!strcmp(cmd, "probe"))   rc = cmd_probe();
    else if (!strcmp(cmd, "serve"))   rc = cmd_serve();
    else if (!strcmp(cmd, "monitor")) rc = cmd_monitor();
    else if (!strcmp(cmd, "take"))    rc = cmd_take();
    else if (!strcmp(cmd, "pop"))     rc = cmd_pop(want);
    else if (!strcmp(cmd, "upload"))  rc = cmd_upload();
    else if (!strcmp(cmd, "loader-echo")) rc = cmd_loader_echo();
    else if (!strcmp(cmd, "loader-bulk")) rc = cmd_loader_bulk();
    else if (!strcmp(cmd, "loader-stream")) rc = cmd_loader_stream();
    else if (!strcmp(cmd, "listen")) rc = cmd_listen(argc>2?atoi(argv[2]):3000);
    else if (!strcmp(cmd, "cal")) rc = cmd_cal();
    else if (!strcmp(cmd, "boottest")) rc = cmd_boottest();
    else if (!strcmp(cmd, "loadertest")) rc = cmd_loadertest(nargs?args[0]:"loader/loader.bin", nargs>1?args[1]:NULL);
    else if (!strcmp(cmd, "payloadcheck")) rc = cmd_payloadcheck(nargs?args[0]:NULL);
    else if (!strcmp(cmd, "attiny-inc")) {
        if (nargs < 4) { ERR("attiny-inc <out.inc> <bootstrap.bin> <loader.bin> <payload1> [payload2 [payload3]]\n"); rc = 1; }
        else rc = cmd_attiny_inc(args[0], args[1], args[2], (const char **)(args + 3), nargs - 3);
    }
    else if (!strcmp(cmd, "loader")) {
        if (nargs < 2) { ERR("loader <bootstrap.bin> <loader.bin> [payload1 ...]\n"); rc = 1; }
        else rc = cmd_loader(args[0], args[1], (const char **)(args + 2), nargs - 2);
    }
    else if (!strcmp(cmd, "run")) {
        if (nargs < 1) { ERR("run <file.bin> [addr]\n"); rc = 1; }
        else rc = cmd_run(args[0], nargs > 1 ? (uint16_t)parse_num(args[1]) : 0xC000);
    }
    else if (!strcmp(cmd, "selftest")) rc = cmd_selftest();
    else if (!strcmp(cmd, "trace"))   rc = cmd_trace(nargs ? (int)parse_num(args[0]) : 3);
    else if (!strcmp(cmd, "wipe-save")) rc = cmd_wipe_save(opt_yes);
    else if (!strcmp(cmd, "dump-save")) {
        if (nargs < 1) { ERR("dump-save <file>\n"); rc = 1; } else rc = cmd_dump_save(args[0]);
    } else if (!strcmp(cmd, "push-save")) {
        if (nargs < 1) { ERR("push-save <file>\n"); rc = 1; } else rc = cmd_push_save(args[0]);
    } else if (!strcmp(cmd, "set-name")) {
        if (nargs < 1) { ERR("set-name <text>\n"); rc = 1; } else rc = cmd_set_name(args[0]);
    } else if (!strcmp(cmd, "bridge")) {
        if (nargs < 1) { ERR("bridge <sub> ...\n"); rc = 1; }
        else rc = cmd_bridge(args[0], args + 1, nargs - 1);
    } else if (!strcmp(cmd, "swap-dump")) {
        if (nargs < 1) { ERR("swap-dump <file>\n"); rc = 1; }
        else rc = cmd_swap_dump(args[0], opt_upload != 0, opt_prompt != 0);
    } else if (!strcmp(cmd, "swap-push")) {
        if (nargs < 1) { ERR("swap-push <file>\n"); rc = 1; }
        else rc = cmd_swap_push(args[0], opt_upload == 1, opt_prompt == 1);
    }
    else if (!strcmp(cmd, "homebrew")) {
        if (nargs < 1) { ERR("homebrew <card id>\n"); rc = 1; }
        else rc = cmd_homebrew((int)parse_num(args[0]), cfg_addcard, cfg_stub_at);
    }
    else if (!strcmp(cmd, "peek")) {
        if (nargs < 2) { ERR("peek <addr> <len>\n"); rc = 1; }
        else rc = cmd_peek((uint16_t)parse_num(args[0]), (int)parse_num(args[1]));
    } else if (!strcmp(cmd, "poke")) {
        uint8_t buf[128]; int n = 0;
        const char *p;
        if (nargs < 2) { ERR("poke <addr> <hexbytes>\n"); rc = 1; }
        else {
            for (p = args[1]; *p && n < (int)sizeof buf; ) {
                if (*p == ' ' || *p == ',') { p++; continue; }
                char tmp[3] = { p[0], p[1], 0 };
                buf[n++] = (uint8_t)strtol(tmp, NULL, 16);
                p += 2;
            }
            rc = cmd_poke((uint16_t)parse_num(args[0]), buf, n);
        }
    } else if (!strcmp(cmd, "give")) {
        uint8_t ids[60]; int n = 0;
        const char *p = nargs ? args[0] : NULL;
        char copy[256], *tok;
        if (p) { strncpy(copy, p, sizeof copy - 1); copy[sizeof copy - 1] = 0; }
        if (!p) { ERR("give <id>[,<id>...]\n"); rc = 1; }
        else {
            for (tok = strtok(copy, ","); tok && n < 60; tok = strtok(NULL, ","))
                ids[n++] = (uint8_t)parse_num(tok);
            rc = cmd_give(ids, n);
        }
    } else {
        ERR("unknown command \"%s\"\n", cmd);
        rc = 1;
    }

    dbg_flush();
    dbg_stats();
    hw_close();
    return rc;
}
