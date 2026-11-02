// Frame step in whole vsyncs (pure function, shared by Mac / iOS; unit test tests/t_fstep.c). The rewriter's frame-step stubs (patch_nso.py FRAME_STEP: Tears of the Kingdom 1.4.3,
// Breath of the Wild 1.9.0) step the game by the vsyncs (1/60 s) the host hands them each frame, and the host keeps each frame on screen for k vsyncs (hle_nvn.c present_itv).
// At a steady rate the step is the time on screen: even motion, and only steps the games' own interval logic uses (60 / 30 / 20 fps). Real time the game falls behind (a late
// frame, a hitch) is a debt paid back one extra vsync per frame, so over time game time = real time. k follows the frames' cost: three late frames in a row step up to the cost
// (30 fps when 60 does not hold: no 40 fps steps on 120 Hz screens); 30 frames in a row whose cost would fit k − 1 try it, and a trial that fails waits twice as long before
// the next (1 s → 8 s), so a device on the edge does not flap.
// A probe is a trial back to kmin that the cost does not justify: while the caller reports the CPU slowed for reasons other than heat (an iPhone's performance controller
// parks a game that waits out a long hold on the efficiency cores at half the clock, where its cost never fits k − 1 again), the hold-off alone starts one. Its first
// FSTEP_GRACE frames may be late without failing it, the time the controller needs to raise the clock for the shorter deadline; after them it is judged like a trial.
// A probe that holds leaves the hold-off as it is, and the run at kmin it starts must last FSTEP_HOLD_MAX (8 s) before the hold-off returns to 1 s: a shorter one counts as
// a failed probe, so a CPU that really cannot hold kmin is not probed every second.
// A step up after a run at kmin of FSTEP_HOLD_MAX keeps the cost that held there, and a cost at the longer hold of at most FSTEP_SAME times it starts a probe too: the scene
// costs what it did, so the step up came from a hitch (Breath of the Wild on an iPhone: pipeline compiles after a cutscene). The fit trial alone does not bring such a game
// back: an iPhone lowers its GPU clock until the work fills most of the frame, so a GPU-bound game's cost sits near FSTEP_FIT at either hold (BotW: GPU 12.8–15.2 ms at hold
// 2, p50, where hold 1 had held for minutes; the same 4,900 draws took 5.8 ms after a trip to the background and 13.5 ms half a minute later).
// A cap that falls and rises again (Tears of the Kingdom's prerendered movies, the iOS thermal cap in hle_nvn.c) keeps what was learned at each kmin: the cost that held it
// (cref) and the GPU reference (nvn_fgpu below); and the frames after the cap rises are a probe with its grace, as the clocks are still set for the longer hold. Before, the
// rise set k to kmin with no grace, so three frames late while the clock rose stepped up again, with cref dropped at the cap change and a GPU reference taken at the 30 fps
// cap: neither the fit trial (a GPU-bound cost near FSTEP_FIT) nor a probe brought kmin back (t_fstep: the movie and thermal cap cases). A cref kept from before the cap
// starts a probe only when the cost at the longer hold is within FSTEP_SAME of it, and a failed probe doubles the hold-off as any other does.
#pragma once
#include <math.h>
#include <stdint.h>

#define FSTEP_MAX 3           // the largest step: both games' own interval logic uses 1–3 (BotW clamps to [2, 3], TOTK picks 2 or 3), so their integer readers never see 4
#define FSTEP_LATE 0.1        // a frame is late when it or its GPU work took this many vsyncs (1.7 ms) more than k: 53 fps is not 60 (start-to-start jitter on time is well below)
#define FSTEP_FIT 0.85        // a frame would fit k − 1 when its cost is at most this share of it
#define FSTEP_RUN 30          // frames in a row that fit k − 1 before a trial, and frames a trial must hold
#define FSTEP_DEBT_MAX 15.0   // debt kept at most (0.25 s): the rest of a long stall (a load) is dropped, not replayed
#define FSTEP_HOLD0 60.0      // vsyncs since the last step up before a trial: 1 s, doubled after each failed trial up to 8 s
#define FSTEP_HOLD_MAX 480.0
#define FSTEP_GRACE 30        // a probe's first frames, whose lateness neither fails it nor counts toward a step up (the controller lowered the clock within 10 frames of a step to 30)
#define FSTEP_SAME 1.1        // the cost at the longer hold against the cost that held kmin: at most this much is the same scene (BotW at hold 2: 1.0–1.07 times its hold-1 GPU time)

typedef struct {
    int k, step;              // vsyncs each frame stays on screen; the step handed to the game for the next frame (k, or k + 1 while paying back)
    int kmin;                 // the last call's kmin (a lower one takes effect at once)
    double debt;              // real time the game is behind, in vsyncs
    int late_run, fit_run, trial;   // late frames in a row; frames in a row whose cost fits k − 1; frames left of a trial at k − 1 (0 = none)
    int probe, grace;         // 1: the trial in progress is a probe; 2: a probe held and its run at kmin has not ended yet; frames left of the probe's grace
    double hold, since;       // vsyncs a trial waits for after a step up; vsyncs since the last step up
    double shown_slack;       // vsyncs a frame on time may show longer than k (set by the caller): one refresh on screens whose rate is not a multiple of 60; the cost keeps FSTEP_LATE
    int screen;               // set by the caller before each call, then cleared: 0 = judge lateness by shown and cost; 1 = the screen showed a recent frame on time, 2 = late. With present
                              // snapshots (nvn_mtl.m) the game runs ahead of the screen and a frame that runs long is absorbed, so only what the screen shows counts
    double run, crun, cref;   // vsyncs at kmin in a row; the cost of its frames on time (running mean; not during a probe's trial, whose frames run while the clock rises);
                              // crun when the last run of FSTEP_HOLD_MAX or more ended in a step up or a cap change (0 = none yet)
    double ccur; int ck, cn;  // the cost of the frames on time at the longer hold ck (running mean) and their count
    int ckmin;                // the kmin run, crun and cref belong to
    double cref_at[FSTEP_MAX + 1];   // cref of each kmin, kept while another kmin is in force
    uint64_t late, paid, ups, downs, failed, probes; double dropped;   // statistics for the log
} nvn_fstep;

// Whether the cost at the longer hold is the cost that held kmin (a probe without a slowed report)
static inline int nvn_fstep_same(const nvn_fstep *s) { return s->cref > 0 && s->cn >= FSTEP_RUN && s->ccur <= FSTEP_SAME * s->cref; }

// Once per frame. shown: real time since the previous frame's start, in vsyncs; cost: the frame's work, max(CPU, GPU), in vsyncs (a GPU that cannot keep up makes frames late too); kmin: 1 at a 60 fps cap, 2 at 30;
// kmax: the longest hold (FSTEP_MAX; 2 at a 30 fps cap, where the display link runs at 30 Hz and holds only even counts); fixed: a step the game has set itself (BotW's
// VFR override; 0 = none), which the host then holds as it is; slow: the CPU runs below its speed for reasons other than heat, so a cost above k − 1 is no evidence
// against kmin (probes). Returns the step for the next frame; s->k is how long that frame stays on screen
static inline int nvn_fstep_next(nvn_fstep *s, double shown, double cost, int kmin, int kmax, int fixed, int slow) {
    if (kmin < 1) kmin = 1;
    if (kmax > FSTEP_MAX) kmax = FSTEP_MAX;
    if (kmax < kmin) kmax = kmin;
    if (!s->hold) s->hold = FSTEP_HOLD0;
    if (fixed > 0) {   // the game's own step: speed is right by construction, nothing to pay back
        s->k = s->step = fixed < FSTEP_MAX ? fixed : FSTEP_MAX;
        s->debt = 0; s->late_run = s->fit_run = s->trial = s->probe = s->grace = 0; s->run = 0;
        s->kmin = FSTEP_MAX + 1;   // when it ends, k starts again from kmin
        return s->step;
    }
    if (!s->k) { s->k = s->step = s->kmin = kmin; return s->step; }   // first frame: nothing measured yet
    int held = s->k;   // the frame that just ended stayed on screen this long
    s->debt += shown - s->step;
    if (s->debt > FSTEP_DEBT_MAX) { s->dropped += s->debt - FSTEP_DEBT_MAX; s->debt = FSTEP_DEBT_MAX; }
    if (s->debt < -1) s->debt = -1;   // ahead (a frame shorter than its step): nothing slows the game down, so at most one vsync is remembered
    s->since += shown;
    int cap_probe = kmin < s->kmin && s->kmin <= FSTEP_MAX && s->k > kmin;
    if (cap_probe) {   // the cap rose: a probe at once, whose grace covers the clocks still set for the longer hold
        s->k = kmin; s->probes++; s->trial = FSTEP_RUN; s->probe = 1; s->grace = FSTEP_GRACE; s->fit_run = s->late_run = 0;
    } else if (kmin < s->kmin || s->k < kmin) s->k = kmin;   // the game's own step ended, or the cap fell
    if (s->k > kmax) s->k = kmax;
    s->kmin = kmin;
    int scr = s->screen; s->screen = 0;
    int late = scr ? scr == 2 : shown > held + FSTEP_LATE + s->shown_slack || cost > held + FSTEP_LATE, grace = s->grace > 0;
    if (grace && !cap_probe) s->grace--;   // the completed frame at the old cap does not consume the new hold's grace
    if (late) { s->late++; s->late_run = grace ? 0 : s->late_run + 1; s->fit_run = 0; } else s->late_run = 0;
    // The cost of the frames on time (a hitch is not what the scene costs): at kmin, and at the longer hold
    if (kmin != s->ckmin) {   // another cap: its run starts over, and what held it before comes back
        if (s->ckmin >= 1 && s->ckmin <= FSTEP_MAX) {
            // A cap can end a sustained run before any hitch saved its cost. Short runs keep the earlier proven reference.
            if (s->run >= FSTEP_HOLD_MAX && s->crun > 0) s->cref = s->crun;
            s->cref_at[s->ckmin] = s->cref;
        }
        s->ckmin = kmin; s->run = s->crun = 0; s->cref = kmin <= FSTEP_MAX ? s->cref_at[kmin] : 0;
    }
    // A frame the screen showed on time while its cost did not fit (absorbed by present snapshots) is not what the hold costs: the probes would take it for the scene that held kmin
    int fits = !scr || cost <= held;
    if (held <= kmin) { s->run += shown; s->cn = 0; if (!late && fits && s->probe != 1) s->crun = s->crun > 0 ? s->crun + (cost - s->crun) / 32 : cost; }
    else {
        s->run = 0;
        if (held != s->ck) { s->ck = held; s->cn = 0; }
        if (!late && fits) { s->ccur = s->cn ? s->ccur + (cost - s->ccur) / 8 : cost; s->cn++; }
    }
    if (s->late_run >= 3 && s->k < kmax) {   // step up to the cost, at least one vsync
        int want = (int)ceil(cost - 0.05);
        if (want < s->k + 1) want = s->k + 1;
        if (want > kmax) want = kmax;
        if (s->trial || (s->probe == 2 && s->since < FSTEP_HOLD_MAX)) { s->failed++; s->trial = 0; s->hold = s->hold * 2 < FSTEP_HOLD_MAX ? s->hold * 2 : FSTEP_HOLD_MAX; }
        else if (s->probe == 2) s->hold = FSTEP_HOLD0;   // the probe's run at kmin lasted
        if (held == kmin && s->run >= FSTEP_HOLD_MAX) s->cref = s->crun;   // kmin held 8 s at this cost
        s->k = want; s->ups++; s->late_run = s->fit_run = 0; s->since = 0; s->probe = 0;
    } else if (s->trial && !grace && !--s->trial) { if (s->probe) { s->probe = 2; s->since = 0; } else s->hold = FSTEP_HOLD0; }   // the trial held (a probe's run at kmin is judged at the next step up)
    if (!late) s->fit_run = s->k > kmin && cost <= FSTEP_FIT * (s->k - 1) ? s->fit_run + 1 : 0;
    if (s->fit_run >= FSTEP_RUN && s->since >= s->hold && !s->trial) { s->k--; s->downs++; s->trial = FSTEP_RUN; s->fit_run = 0; }
    else if ((slow || nvn_fstep_same(s)) && s->k > kmin && s->since >= s->hold && !s->trial) {   // probe: straight to kmin, so the controller sees the whole deadline
        s->k = kmin; s->downs++; s->probes++; s->trial = FSTEP_RUN; s->probe = 1; s->grace = FSTEP_GRACE; s->fit_run = s->late_run = 0;
    }
    s->step = s->k + (s->debt >= 1 && s->k < FSTEP_MAX);
    s->paid += s->step > s->k;
    return s->step;
}

// The GPU clock follows the hold too: Breath of the Wild on an iPhone went from 14.4 to about 25 ms of GPU time a frame at hold 2 with the same 4,900 draws, the CPU still on its
// performance cores, so its cost never fitted hold 1 again. The GPU counts as slowed (another reason for nvn_fstep_next's slow) when its time at a hold above kmin is at least
// FSTEP_GPU_SLOW times its time at kmin and it alone does not fit k − 1 (a GPU at 3 ms of 17, 6 times its time on a loading screen, is not what holds a frame back).
// The reference is not updated during a probe's trial (its frames run while the clock is still rising), so a failed probe leaves it as it was. It is kept for each kmin: one
// learned at a 30 fps cap is the GPU time at the clock that cap left (lowered for 30 fps, and under the thermal cap for the heat), and taken as hold 1's after the cap rose it
// made the same work at hold 2 look not slowed
#define FSTEP_GPU_SLOW 1.3    // GPU time at the longer hold against its time at kmin
#define FSTEP_GPU_N 30        // frames at the longer hold before the comparison counts
typedef struct { double ref, cur; int n, k; double ref_at[FSTEP_MAX + 1]; } nvn_fgpu;   // GPU time at kmin (running mean; of every kmin in ref_at); at the current hold k and its frame count
static inline int nvn_fgpu_slow(const nvn_fgpu *g) { return g->n >= FSTEP_GPU_N && g->ref > 0 && g->cur >= FSTEP_GPU_SLOW * g->ref && g->cur > FSTEP_FIT * (g->k - 1); }
// Once per frame, with the frame's GPU time (vsyncs; 0 = none finished yet) and the hold and probe state (nvn_fstep.probe) it ran under
static inline int nvn_fstep_gpu_slowed(nvn_fgpu *g, double gpu, int k, int kmin, int probe) {
    if (kmin < 1 || kmin > FSTEP_MAX) return 0;   // no reference slot for it (hle_nvn.c passes 1 or 2)
    g->ref = g->ref_at[kmin];
    if (gpu <= 0) return nvn_fgpu_slow(g);
    if (k <= kmin) { if (probe != 1) g->ref = g->ref_at[kmin] = g->ref > 0 ? g->ref + (gpu - g->ref) / 32 : gpu; g->n = 0; g->k = k; return 0; }
    if (k != g->k) { g->k = k; g->n = 0; }
    g->cur = g->n ? g->cur + (gpu - g->cur) / 8 : gpu; g->n++;
    return nvn_fgpu_slow(g);
}
