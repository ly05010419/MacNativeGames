/* TOTK: the decision of the auto frame-rate cap (hle_nvn.c cap_auto_note), kept free of globals so it can be tested
   (games/totk/tests/cap_auto/probe.c). k is the number of display ticks per game frame (1 = 60 fps on a 60 Hz screen).

   The rule itself (unchanged): the rate follows the typical frame, the 75th percentile of the last 120 frames' work and
   of the last 16 GPU times. It slows down only when that frame no longer fits the current step at all (after 1 s), to
   the fastest step it fits with 5 % to spare, and speeds up only when it fits the faster step with 20 % to spare (after
   1.5 s).

   Probes (adapted from nxnative's frame-step probes, dm/gfx/nvn/nvn_fstep.h, commits 9409472 and b53bb89, MIT,
   https://github.com/seedds/nxnative): an iPhone that drops a game from 60 to 30 fps lowers its CPU and GPU clocks, or
   parks the threads on the efficiency cores, because the load fell; the work measured at 30 then never fits 60 with
   20 % to spare again, and the game stays at 30. nxnative measured it there: Breath of the Wild's GPU time went from
   14.4 to about 25 ms with the same 4,900 draws, Tears of the Kingdom ran 6 % on performance cores at 1.6 GHz, and a
   GPU-bound game's cost sat near the fit line at either rate. So the faster rate's cost is remembered, and a probe tries
   that rate again, ignoring the fit rule, when one of these holds at the slower rate:
   - CAPA_SIG_SAME: the faster rate held CAPA_HELD_NS (8 s) or longer, and the cost at the slower rate is at most
     CAPA_SAME (1.1) times what it cost there: the scene costs what it did, so the step down came from a hitch;
   - CAPA_SIG_CPU: the caller reports the CPU parked on efficiency cores (capa_cpu_window);
   - CAPA_SIG_GPU: the GPU time is at least CAPA_GPU_SLOW (1.3) times its time at the faster rate and alone does not
     fit it (the GPU clock followed the slower rate).
   Anti-flap: a probe waits for the hold-off since the last switch (2 s, doubled after each failed probe up to 32 s);
   its first CAPA_GRACE frames are not counted (the clocks rise for the shorter deadline); then, while the window holds
   up to 60 frames, the 75th percentile of CAPA_TRIAL frames or more that misses the step ends it at once (without the
   rule's 1 s), later the normal rule judges it; a probe whose rate does not last CAPA_HELD_NS counts as failed. A probe that lasts resets the hold-off.
   The caller gates every probe on the thermal state and Low Power Mode (capa_env.power_ok). */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CAPA_WIN = 120, CAPA_GWIN = 16, CAPA_GRACE = 30, CAPA_TRIAL = 15, CAPA_NCUR = 30, CAPA_CPU_LOW = 30 };
#define CAPA_SLOW_NS 1000000000ull        /* a slower step must be wanted this long */
#define CAPA_FAST_NS 1500000000ull        /* a faster step (fit rule) must be wanted this long */
#define CAPA_HELD_NS 8000000000ull        /* a rate that lasts this long keeps its cost; a probe that lasts this long held */
#define CAPA_HOLD0_NS 2000000000ull       /* hold-off before a probe, after any switch */
#define CAPA_HOLD_MAX_NS 32000000000ull   /* doubled after each failed probe up to this */
#define CAPA_SAME 1.1                     /* cost at the slower rate against the cost that held the faster one */
#define CAPA_GPU_SLOW 1.3                 /* GPU time at the slower rate against its time at the faster one */

enum { CAPA_SIG_SAME = 1, CAPA_SIG_CPU = 2, CAPA_SIG_GPU = 4, CAPA_SIG_ALL = 7 };
enum { CAPA_EV_SWITCH = 1, CAPA_EV_PROBE = 2, CAPA_EV_HELD = 4, CAPA_EV_FAILED = 8 };

typedef struct {
    int kmin, kmax;     /* the caps' range of display ticks */
    double tick_ns;     /* one display tick */
    int cpu_slowed;     /* the CPU is parked on efficiency cores (capa_cpu_window) */
    int power_ok;       /* thermal state nominal or fair and Low Power Mode off: probes allowed */
    int signals;        /* CAPA_SIG_* the caller enables (0 = the rule without probes) */
} capa_env;

typedef struct {
    uint64_t win[CAPA_WIN]; unsigned nwin, pos;   /* the frames' work (ns) */
    double gpu_ms[CAPA_GWIN]; unsigned ngpu;      /* the frames' GPU time (ms), fed by capa_gpu */
    int k_seen, want_k, idle; uint64_t want_since;
    /* probes */
    uint64_t since; unsigned at_k;                /* time of the last switch; frames noted at k since then */
    double crun, grun; unsigned nrun;             /* mean need / GPU (ns) at k of the frames that fit it */
    int kref; double cref, gref;                  /* the faster rate, its cost when it held CAPA_HELD_NS (0 = none), its GPU time */
    double ccur, gcur; unsigned ncur;             /* mean need / GPU (ns) at the slower rate */
    uint64_t hold; int probe, grace;              /* hold-off; 1 while the probed rate has not lasted CAPA_HELD_NS; frames of grace left */
    uint64_t probes, failed, held;                /* statistics for the log */
    /* the last call, for the log */
    int ev, ev_sig, ev_from; unsigned ev_n; uint64_t ev_busy, ev_gpu, ev_ran;
} capa_state;

static inline int capa_cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static inline uint64_t capa_p75(const uint64_t *v, unsigned n) {   /* 75th percentile of n samples (n <= CAPA_WIN) */
    uint64_t s[CAPA_WIN]; memcpy(s, v, n * sizeof *s); qsort(s, n, sizeof *s, capa_cmp_u64); return s[(n * 3) / 4 < n ? (n * 3) / 4 : n - 1];
}
static inline void capa_gpu(capa_state *s, double ms) { s->gpu_ms[s->ngpu++ % CAPA_GWIN] = ms; }
static inline uint64_t capa_gpu_typical_ns(const capa_state *s) {   /* 75th percentile of the last 16 frames' GPU time */
    uint64_t v[CAPA_GWIN]; unsigned n = s->ngpu < CAPA_GWIN ? s->ngpu : CAPA_GWIN;
    for (unsigned i = 0; i < n; i++) v[i] = (uint64_t)(s->gpu_ms[i] * 1e6);
    return n ? capa_p75(v, n) : 0;
}
/* Auto is not in use (another mode, interpolation, a movie): forget the window; the remembered cost stays */
static inline void capa_idle(capa_state *s) { s->nwin = s->pos = 0; s->want_since = 0; s->idle = 1; s->probe = s->grace = 0; }
static inline void capa_forget(capa_state *s, uint64_t now) {   /* k set from outside or the display rate changed */
    s->kref = 0; s->cref = s->gref = 0; s->probe = s->grace = 0; s->hold = CAPA_HOLD0_NS;
    s->since = now; s->at_k = s->nrun = s->ncur = 0;
}
static inline void capa_switched(capa_state *s, uint64_t now) { s->since = now; s->at_k = s->nrun = s->ncur = 0; s->ev |= CAPA_EV_SWITCH; }
static inline double capa_mean(double m, unsigned n, double v, double w) { return n ? m + (v - m) * w : v; }

/* Once per frame with the frame's work and the time; k is the step in use. Returns the step for the next frames;
   s->ev says what happened (CAPA_EV_*), s->ev_sig why a probe started */
static inline int capa_note(capa_state *s, int k, uint64_t busy_ns, uint64_t now, const capa_env *e) {
    s->ev = 0;
    if (!s->hold) s->hold = CAPA_HOLD0_NS;
    if (k < e->kmin) k = e->kmin; if (k > e->kmax) k = e->kmax;
    if (k != s->k_seen) { s->k_seen = k; s->want_since = 0; capa_forget(s, now); }   /* first use, or the display rate changed */
    if (s->idle) { s->idle = 0; s->since = now; s->at_k = s->ncur = s->nrun = 0; }
    if (s->grace) {   /* a probe's first frames: the clocks rise for the shorter deadline */
        if (!--s->grace) s->ngpu = 0;   /* the GPU times so far were measured before the probe */
        return k;
    }
    s->win[s->pos] = busy_ns; s->pos = (s->pos + 1) % CAPA_WIN; if (s->nwin < CAPA_WIN) s->nwin++;
    s->at_k++;
    int trial = s->probe && s->nwin < CAPA_WIN / 2;   /* a probe's first frames after the grace */
    if (s->nwin < (trial ? CAPA_TRIAL : CAPA_WIN / 2)) return k;
    double tick = e->tick_ns;
    uint64_t busy = capa_p75(s->win, s->nwin), gpu = capa_gpu_typical_ns(s), need = busy > gpu ? busy : gpu;
    s->ev_busy = busy; s->ev_gpu = gpu; s->ev_n = s->nwin;
    int target = k;
    if (need > k * tick) { target = e->kmin; while (target < e->kmax && need > 0.95 * target * tick) target++; }
    else if (k > e->kmin && need < 0.8 * (k - 1) * tick) { target = e->kmin; while (target < k && need > 0.8 * target * tick) target++; }
    /* what the frames cost at k, once the window holds only frames at k */
    uint64_t ran = now - s->since;
    if (s->probe && ran >= CAPA_HELD_NS) { s->probe = 0; s->hold = CAPA_HOLD0_NS; s->held++; s->ev |= CAPA_EV_HELD; }
    if (s->at_k >= CAPA_WIN) {
        if (need <= k * tick) { s->crun = capa_mean(s->crun, s->nrun, (double)need, 1.0 / 64); s->grun = capa_mean(s->grun, s->nrun, (double)gpu, 1.0 / 64); s->nrun++; }
        if (s->kref && s->kref < k) { s->ccur = capa_mean(s->ccur, s->ncur, (double)need, 1.0 / 16); s->gcur = capa_mean(s->gcur, s->ncur, (double)gpu, 1.0 / 16); s->ncur++; }
    }
    if (target == k) {
        s->want_since = 0;
        if (k <= e->kmin || !e->signals || !e->power_ok || s->probe || ran < s->hold) return k;
        int sig = 0, to = s->kref && s->kref < k ? s->kref : k - 1;
        if (s->kref && s->kref < k && s->ncur >= CAPA_NCUR) {
            if (s->cref > 0 && s->ccur <= CAPA_SAME * s->cref) sig |= CAPA_SIG_SAME;
            if (s->gref > 0 && s->gcur >= CAPA_GPU_SLOW * s->gref && s->gcur > 0.8 * to * tick) sig |= CAPA_SIG_GPU;
        }
        if (e->cpu_slowed) sig |= CAPA_SIG_CPU;
        sig &= e->signals;
        if (!sig) return k;
        if (to < e->kmin) to = e->kmin;
        s->probes++; s->probe = 1; s->grace = CAPA_GRACE; s->nwin = s->pos = 0;
        s->ev_sig = sig; s->ev_from = k; capa_switched(s, now); s->ev |= CAPA_EV_PROBE;
        s->k_seen = to; return to;
    }
    if (!(trial && target > k)) {
        if (target != s->want_k || !s->want_since) { s->want_k = target; s->want_since = now; return k; }
        if (now - s->want_since < (target > k ? CAPA_SLOW_NS : CAPA_FAST_NS)) return k;
    }
    s->want_since = 0; s->ev_from = k; s->ev_ran = ran;
    if (target > k) {   /* slower */
        if (s->probe) {   /* the probed rate did not last: wait longer before the next */
            s->failed++; s->probe = 0; s->hold = s->hold * 2 < CAPA_HOLD_MAX_NS ? s->hold * 2 : CAPA_HOLD_MAX_NS; s->ev |= CAPA_EV_FAILED;
        } else if (ran >= CAPA_HELD_NS && s->nrun >= CAPA_WIN) { s->kref = k; s->cref = s->crun; s->gref = s->grun; }   /* k held at this cost */
        else if (!s->kref || s->kref >= target) { s->kref = k; s->cref = 0; s->gref = s->nrun >= CAPA_NCUR ? s->grun : 0; }   /* a short run keeps no cost */
    } else s->probe = 0;   /* faster by the fit rule */
    capa_switched(s, now);
    s->k_seen = target; return target;
}

/* The CPU parked on efficiency cores (CAPA_SIG_CPU): under CAPA_CPU_LOW % of the process's CPU time on performance
   cores in each of the last two windows (one window alone dipped that low on a Mac at 30 fps now and then, nxnative).
   cpu and ptime: the process's total CPU time and its time on performance cores (TASK_POWER_INFO_V2, mach units). */
typedef struct { uint64_t pc, pp; int share, low_was, slowed; } capa_cpu;
static inline int capa_cpu_window(capa_cpu *c, uint64_t cpu, uint64_t ptime) {
    if (c->pc && cpu > c->pc && ptime >= c->pp) c->share = (int)(100 * (ptime - c->pp) / (cpu - c->pc)); else if (!c->pc) c->share = -1;
    c->pc = cpu; c->pp = ptime;
    int low = c->share >= 0 && c->share < CAPA_CPU_LOW;
    c->slowed = low && c->low_was; c->low_was = low;
    return c->slowed;
}
