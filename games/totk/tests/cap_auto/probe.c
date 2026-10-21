/* TOTK: the auto frame-rate cap's decision (gfx/nvn/nvn_cap_auto.h) on synthetic frame costs, CPU only.
   A frame at k display ticks takes k ticks, or as many whole ticks as its cost needs; the cost may depend on k (the
   iPhone lowers its clocks at the slower rate) and on the frames since the last switch (the clocks rising again).
   Cases at 60 Hz: steady 60; a hitch, then the cost at 30 that held 60 (probe back); a hitch, then the CPU parked on
   efficiency cores; a hitch, then the GPU clock lowered; a genuinely heavy scene (probes fail, the hold-off grows, no
   flapping); thermal serious (no probe until it clears); a short run at 60 (no remembered cost). Negative controls:
   the same sequences with probes off stay at 30. The 144 Hz sequence of the present-interval probe gives the same
   steps with and without probes. Build from games/totk, output outside the project:
   xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn tests/cap_auto/probe.c -o /tmp/cap-auto-probe */
#include <stdio.h>
#include <stdlib.h>
#include "nvn_cap_auto.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    capa_state s; capa_env e; int k; uint64_t t;
    unsigned since_switch;                  /* frames since the last switch */
    int switches, probes_seen, held, failed;
    uint64_t probe_t[64]; int np;           /* probe start times */
    uint64_t at_fast_ns;                    /* time spent at kmin */
    int sig_union;
} sim;
typedef struct { double cpu[3], gpu[3]; unsigned ramp; double ramp_cpu, ramp_gpu; int slowed_at2; } scene;   /* index: k (1 = 60 fps, 2 = 30 fps) */

static void sim_init(sim *m, int hz, int kmin, int kmax, int k, int signals) {
    memset(m, 0, sizeof *m);
    m->e.kmin = kmin; m->e.kmax = kmax; m->e.tick_ns = 1e9 / hz; m->e.power_ok = 1; m->e.signals = signals;
    m->k = k; m->t = 1000000000ull;
}
static void sim_frame(sim *m, double cpu_ms, double gpu_ms) {
    capa_gpu(&m->s, gpu_ms);
    double need = (cpu_ms > gpu_ms ? cpu_ms : gpu_ms) * 1e6; int ticks = m->k;
    while (ticks * m->e.tick_ns < need) ticks++;
    uint64_t dt = (uint64_t)(ticks * m->e.tick_ns); m->t += dt;
    if (m->k == m->e.kmin) m->at_fast_ns += dt;
    int nk = capa_note(&m->s, m->k, (uint64_t)(cpu_ms * 1e6), m->t, &m->e);
    if (m->s.ev & CAPA_EV_SWITCH) { m->switches++; m->since_switch = 0; } else m->since_switch++;
    if (m->s.ev & CAPA_EV_PROBE) { m->probes_seen++; m->sig_union |= m->s.ev_sig; if (m->np < 64) m->probe_t[m->np++] = m->t; }
    if (m->s.ev & CAPA_EV_HELD) m->held++;
    if (m->s.ev & CAPA_EV_FAILED) m->failed++;
    m->k = nk;
}
static void sim_run(sim *m, double secs, const scene *c) {
    uint64_t end = m->t + (uint64_t)(secs * 1e9);
    while (m->t < end) {
        int k = m->k > 2 ? 2 : m->k;
        int ramp = k == 1 && m->since_switch < c->ramp;
        m->e.cpu_slowed = c->slowed_at2 && m->k == 2;
        sim_frame(m, ramp ? c->ramp_cpu : c->cpu[k], ramp ? c->ramp_gpu : c->gpu[k]);
    }
}
static scene sc(double cpu1, double gpu1, double cpu2, double gpu2) { scene c = {{0, cpu1, cpu2}, {0, gpu1, gpu2}, 0, 0, 0, 0}; return c; }

static void steady_60(void) {
    sim m; sim_init(&m, 60, 1, 2, 2, CAPA_SIG_ALL);   /* the cap starts at 2 ticks, as g_auto_k does */
    scene c = sc(9, 8, 9, 8); sim_run(&m, 60, &c);
    CHECK(m.k == 1 && m.switches == 1 && m.probes_seen == 0, "steady 60: k %d, %d switches, %d probes", m.k, m.switches, m.probes_seen);
}
/* 20 s at 60 at a cost of 14 ms (fits 16.7, but not the 13.3 ms the fit rule wants), then a 2 s hitch, then `after` */
static void hitch(sim *m, const scene *before, const scene *after, double secs) {
    sim_run(m, 20, before);
    CHECK(m->k == 1, "60 holds before the hitch (k %d)", m->k);
    scene h = sc(30, 12, 30, 12); sim_run(m, 2.5, &h);
    CHECK(m->k == 2, "the hitch steps down to 30 (k %d)", m->k);
    sim_run(m, secs, after);
}
static void same_cost(int signals, int expect_back) {
    sim m; sim_init(&m, 60, 1, 2, 1, signals);
    scene b = sc(14, 12, 14, 12), a = sc(14, 12, 14.5, 12.5);   /* at 30 the clocks fall until the work costs what it did */
    hitch(&m, &b, &a, 30);
    if (expect_back) {
        CHECK(m.k == 1 && m.probes_seen == 1 && m.held == 1 && m.failed == 0 && m.sig_union == CAPA_SIG_SAME, "same cost: k %d, %d probes, %d held, %d failed, signals %d",
              m.k, m.probes_seen, m.held, m.failed, m.sig_union);
        CHECK(m.probe_t[0] - (m.t - 30000000000ull) < 8000000000ull, "same cost: the probe came %.1f s after the hitch", (m.probe_t[0] - (m.t - 30000000000ull)) / 1e9);
    } else CHECK(m.k == 2 && m.probes_seen == 0, "negative control (probes off): stays at 30 (k %d, %d probes)", m.k, m.probes_seen);
}
static void ecores(int signals, int expect_back) {
    sim m; sim_init(&m, 60, 1, 2, 1, signals);
    scene b = sc(14, 12, 14, 12), a = sc(14, 12, 24, 12);   /* parked on efficiency cores at 30: 24 ms of CPU */
    a.slowed_at2 = 1; a.ramp = 20; a.ramp_cpu = 24; a.ramp_gpu = 12;   /* the first 20 frames at 60 still run at the low clock */
    hitch(&m, &b, &a, 30);
    if (expect_back) CHECK(m.k == 1 && m.probes_seen == 1 && m.held == 1 && m.failed == 0 && (m.sig_union & CAPA_SIG_CPU), "efficiency cores: k %d, %d probes, %d held, %d failed, signals %d",
                           m.k, m.probes_seen, m.held, m.failed, m.sig_union);
    else CHECK(m.k == 2 && m.probes_seen == 0, "negative control (CPU signal off): stays at 30 (k %d, %d probes)", m.k, m.probes_seen);
}
static void gpu_clock(int signals, int expect_back) {
    sim m; sim_init(&m, 60, 1, 2, 1, signals);
    scene b = sc(8, 14, 8, 14), a = sc(8, 14, 8, 21);   /* the GPU clock followed 30: 21 ms with the same draws */
    a.ramp = 15; a.ramp_cpu = 8; a.ramp_gpu = 21;
    hitch(&m, &b, &a, 30);
    if (expect_back) CHECK(m.k == 1 && m.probes_seen == 1 && m.held == 1 && (m.sig_union & CAPA_SIG_GPU), "GPU clock: k %d, %d probes, %d held, signals %d", m.k, m.probes_seen, m.held, m.sig_union);
    else CHECK(m.k == 2 && m.probes_seen == 0, "negative control (GPU signal off): stays at 30 (k %d, %d probes)", m.k, m.probes_seen);
}
/* The scene really got heavier: 22 ms at 60, but 15 ms at 30 (the clocks fell; the "same cost" signal fires wrongly)
   and the CPU parked on efficiency cores at 30. Probes fail, each waits longer, and 60 is rarely shown */
static void heavy_scene(void) {
    sim m; sim_init(&m, 60, 1, 2, 1, CAPA_SIG_ALL);
    scene b = sc(14, 12, 14, 12), a = sc(22, 12, 15, 12); a.slowed_at2 = 1;
    sim_run(&m, 20, &b);
    uint64_t t0 = m.t; uint64_t fast0 = m.at_fast_ns;
    sim_run(&m, 180, &a);
    double fast = (m.at_fast_ns - fast0) / 1e9;
    CHECK(m.k == 2 || m.s.probe, "heavy scene: ends at 30 or in a probe (k %d)", m.k);
    CHECK(m.held == 0 && m.failed >= m.probes_seen - 1 && m.probes_seen >= 3 && m.probes_seen <= 9, "heavy scene: %d probes, %d failed, %d held", m.probes_seen, m.failed, m.held);
    CHECK(fast < 0.1 * 180, "heavy scene: %.1f s of 180 at 60 fps", fast);
    for (int i = 2; i < m.np; i++)
        CHECK(m.probe_t[i] - m.probe_t[i - 1] >= m.probe_t[i - 1] - m.probe_t[i - 2] || m.probe_t[i - 1] - m.probe_t[i - 2] > CAPA_HOLD_MAX_NS,
              "heavy scene: the gap between probes grows (%.1f s after %.1f s)", (m.probe_t[i] - m.probe_t[i - 1]) / 1e9, (m.probe_t[i - 1] - m.probe_t[i - 2]) / 1e9);
    CHECK(m.s.hold == CAPA_HOLD_MAX_NS, "heavy scene: the hold-off reached %.0f s (%.0f s)", CAPA_HOLD_MAX_NS / 1e9, m.s.hold / 1e9);
    printf("heavy scene: probes at");
    for (int i = 0; i < m.np; i++) printf(" %.1f", (m.probe_t[i] - t0) / 1e9);
    printf(" s; %.1f s of 180 at 60 fps, %d switches\n", fast, m.switches);
}
static void thermal(void) {
    sim m; sim_init(&m, 60, 1, 2, 1, CAPA_SIG_ALL);
    scene b = sc(14, 12, 14, 12), a = sc(14, 12, 24, 12); a.slowed_at2 = 1;
    sim_run(&m, 20, &b); m.e.power_ok = 0;   /* thermal state serious (or Low Power Mode) */
    scene h = sc(30, 12, 30, 12); sim_run(&m, 2.5, &h);
    sim_run(&m, 60, &a);
    CHECK(m.k == 2 && m.probes_seen == 0, "thermal serious: no probe (k %d, %d probes)", m.k, m.probes_seen);
    m.e.power_ok = 1; sim_run(&m, 20, &a);
    CHECK(m.k == 1 && m.probes_seen == 1, "thermal back to fair: probes back to 60 (k %d, %d probes)", m.k, m.probes_seen);
}
static void short_run(void) {
    sim m; sim_init(&m, 60, 1, 2, 1, CAPA_SIG_SAME);
    scene b = sc(14, 12, 14, 12), a = sc(14, 12, 14.5, 12.5);
    sim_run(&m, 4, &b);   /* 60 held only 4 s: its cost is not remembered */
    scene h = sc(30, 12, 30, 12); sim_run(&m, 2.5, &h);
    sim_run(&m, 30, &a);
    CHECK(m.k == 2 && m.probes_seen == 0, "short run at 60: no probe on cost (k %d, %d probes)", m.k, m.probes_seen);
}
/* The present-interval probe's 144 Hz sequence (hle_nvn.c dm_nvn_probe_present_interval): the same steps with probes */
static void rule_144(int signals) {
    sim m; sim_init(&m, 144, 2, 4, 2, signals); capa_state *s = &m.s; uint64_t t = 1000000000ull; int k = 2;
#define NOTE(b) (k = capa_note(s, k, (b), t, &m.e))
    NOTE(1);
    for (int i = 0; i < 140; i++) { t += 15000000ull; NOTE(15000000ull); }
    CHECK(k == 3, "144 Hz (signals %d): slows to 48 fps (k %d)", signals, k);
    for (int i = 0; i < 16; i++) capa_gpu(s, 16.0);
    for (int i = 0; i < 160; i++) { t += 20833333ull; NOTE(5000000ull); }
    CHECK(k == 3, "144 Hz (signals %d): no faster than the GPU holds (k %d)", signals, k);
    for (int i = 0; i < 16; i++) capa_gpu(s, 6.0);
    for (int i = 0; i < 160; i++) { t += 20833333ull; NOTE(5000000ull); }
    CHECK(k == 2, "144 Hz (signals %d): back to 72 fps (k %d)", signals, k);
    for (int i = 0; i < 200; i++) { t += 13888889ull; NOTE(i % 8 ? 5000000ull : 30000000ull); }
    CHECK(k == 2 && !s->probes, "144 Hz (signals %d): not slowed by occasional hitches (k %d, %llu probes)", signals, k, (unsigned long long)s->probes);
#undef NOTE
}
static void cpu_window(void) {
    capa_cpu c = {0}; const int share[] = {90, 20, 90, 20, 20, 20, 80}, want[] = {0, 0, 0, 0, 1, 1, 0};
    uint64_t cpu = 1000, p = 900; capa_cpu_window(&c, cpu, p);
    for (int i = 0; i < 7; i++) { cpu += 1000; p += (uint64_t)share[i] * 10; int r = capa_cpu_window(&c, cpu, p);
        CHECK(r == want[i] && c.share == share[i], "CPU windows: window %d at %d%% -> %d (want %d, share %d)", i, share[i], r, want[i], c.share); }
}
int main(void) {
    steady_60();
    same_cost(CAPA_SIG_ALL, 1); same_cost(CAPA_SIG_SAME, 1); same_cost(0, 0);
    ecores(CAPA_SIG_ALL, 1); ecores(CAPA_SIG_SAME, 0);
    gpu_clock(CAPA_SIG_ALL, 1); gpu_clock(CAPA_SIG_SAME, 0);
    heavy_scene(); thermal(); short_run();
    rule_144(0); rule_144(CAPA_SIG_ALL);
    cpu_window();
    printf("cap auto: %d / %d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail != 0;
}
