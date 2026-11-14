// Frame step controller unit tests (nvn_fstep.h): frames are simulated in vsyncs. A frame whose work w fits its hold k stays on screen k vsyncs; a longer one takes w
// (the next frame starts at once, as the display-link wait does when behind). Game time is the sum of the steps, real time the sum of the frame times.
#include "../nvn/nvn_fstep.h"
#include <stdio.h>
#include <string.h>

static int fail, checks;
static void check(int ok, const char *what) { checks++; fail += !ok; printf("%-78s %s\n", what, ok ? "✓" : "✗"); }

typedef struct { nvn_fstep s; double real, game; int next, slow; } sim;
// n frames of work w (vsyncs) each; GPU-or-CPU cost = w; slow: the CPU is reported slowed for reasons other than heat (probes)
static void run(sim *m, int n, double w, int kmin, int kmax, int fixed) {
    for (int i = 0; i < n; i++) {
        int k = m->s.k ? m->s.k : kmin, step = m->next ? m->next : kmin;
        double shown = w > k ? w : k;
        m->real += shown; m->game += step;
        m->next = nvn_fstep_next(&m->s, shown, w, kmin, kmax, fixed, m->slow);
    }
}

int main(void) {
    char b[160];
    {   sim m = {0}; run(&m, 600, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.paid == 0 && m.game == m.real, "steady 60: hold 1, step 1, game time = real time");
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 600, 1.32, 1, 3, 0);   // 22 ms frames
        snprintf(b, sizeof b, "steady 22 ms frames: 30 fps after three late frames, no flapping (k %d, ups %llu, downs %llu)", m.s.k, (unsigned long long)m.s.ups, (unsigned long long)m.s.downs);
        check(m.s.k == 2 && m.s.ups == 1 && m.s.downs == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.debt < 1, b);
    }
    {   sim m = {0}; run(&m, 120, 0.5, 1, 3, 0); run(&m, 300, 1.12, 1, 3, 0);   // 18.7 ms frames: 53 fps
        check(m.s.k == 2 && m.s.ups == 1, "steady 18.7 ms frames (53 fps): 30 fps, not 53 with a payback every 8 frames");
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        m.real += 6; m.game += m.next; m.next = nvn_fstep_next(&m.s, 6, 6, 1, 3, 0, 0);   // one 100 ms hitch
        check(m.next == 2 && m.s.k == 1, "100 ms hitch: the next step pays back one vsync, hold stays 1");
        int two = 0; for (int i = 0; i < 20; i++) { int st = m.next; run(&m, 1, 0.5, 1, 3, 0); two += st == 2; }
        snprintf(b, sizeof b, "  five steps of 2, then 1 (%d steps of 2); game time %.0f = real time %.0f", two, m.game, m.real);
        check(two == 5 && m.game == m.real && m.s.k == 1, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0);
        m.real += 30; m.game += m.next; m.next = nvn_fstep_next(&m.s, 30, 30, 1, 3, 0, 0);   // a 0.5 s stall
        run(&m, 60, 0.5, 1, 3, 0);
        snprintf(b, sizeof b, "0.5 s stall: 15 vsyncs paid back, %.0f dropped, not replayed (paid %llu)", m.s.dropped, (unsigned long long)m.s.paid);
        check(m.s.dropped == 14 && m.s.paid == 15 && m.real - m.game == 14 && m.s.k == 1, b);
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0);   // a heavy scene: 30 fps
        check(m.s.k == 2, "heavy scene: hold 2");
        run(&m, 29, 0.6, 1, 3, 0);
        check(m.s.k == 2, "  cost drops: 29 frames that fit 1 are not enough");
        run(&m, 1, 0.6, 1, 3, 0);
        check(m.s.k == 1 && m.s.trial == FSTEP_RUN, "  the 30th starts a trial at hold 1");
        run(&m, FSTEP_RUN + 10, 0.6, 1, 3, 0);
        check(m.s.k == 1 && m.s.trial == 0 && m.s.hold == FSTEP_HOLD0 && m.s.failed == 0, "  the trial holds: 60 fps again");
    }
    {   // the cost says 0.6, but frames at hold 1 take 1.5 (the cost misses something): each trial fails, and the trials spread out 1 → 2 → 4 → 8 s
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 30, 1.5, 1, 3, 0);
        check(m.s.k == 2, "frames at hold 1 take 1.5 vsyncs: hold 2");
        double last = m.real, gaps[4]; int ng = 0; uint64_t downs = m.s.downs;
        for (int i = 0; i < 4000 && ng < 4; i++) {
            double w = m.s.k == 1 ? 1.5 : 0.6;
            run(&m, 1, w, 1, 3, 0);
            if (m.s.downs != downs) { downs = m.s.downs; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "  trials %.0f, %.0f, %.0f, %.0f vsyncs apart; %llu failed", gaps[0], gaps[1], gaps[2], gaps[3], (unsigned long long)m.s.failed);
        check(ng == 4 && gaps[0] >= 60 && gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[3] < 560 && m.s.failed >= 3, b);
    }
    {   sim m = {0}; run(&m, 300, 0.5, 2, 2, 0);
        check(m.s.k == 2 && m.next == 2 && m.game == m.real, "30 fps cap: hold 2, step 2");
        m.real += 6; m.game += m.next; m.next = nvn_fstep_next(&m.s, 6, 6, 2, 2, 0, 0);
        int three = 0; for (int i = 0; i < 10; i++) { int st = m.next; run(&m, 1, 0.5, 2, 2, 0); three += st == 3; }
        snprintf(b, sizeof b, "  a hitch at the 30 cap is paid back with steps of 3 (%d), hold stays 2", three);
        check(three == 4 && m.s.k == 2 && m.game == m.real, b);
        run(&m, 300, 3.5, 2, 2, 0);
        check(m.s.k == 2, "  frames longer than the hold at the 30 cap: hold stays 2 (the 30 Hz link holds only even counts)");
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 1, 0.5, 2, 3, 0);
        check(m.s.k == 2, "movie flag (kmin 2): hold 2 at once");
        run(&m, 120, 0.5, 2, 3, 0); run(&m, 1, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.ups == 0 && m.s.late == 0, "  movie ends (kmin 1): hold 1 at once");
    }
    {   sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 120, 0.5, 1, 3, 2);
        check(m.s.k == 2 && m.next == 2 && m.s.debt == 0, "the game fixes its step at 2 (BotW's VFR override): hold 2, step 2");
        run(&m, 2, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.next == 1 && m.s.late == 0, "  the override ends: hold 1, step 1, no late frame counted");
    }
    {   // a parked CPU: 20 ms frames at hold 2 never fit hold 1, and without the slow report nothing tries it
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); run(&m, 2000, 1.2, 1, 3, 0);
        check(m.s.k == 2 && m.s.downs == 0 && m.s.probes == 0, "not slowed: 1.2-vsync frames at hold 2 stay at hold 2, no probe");
    }
    {   // the same with the CPU reported slowed: the probe goes to hold 1, its frames take 1.5 vsyncs for 20 frames while the clock rises, then 0.6
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        int at1 = 0, up_during_grace = 0; uint64_t ups = m.s.ups;
        for (int i = 0; i < 400; i++) {
            double w = m.s.k == 1 ? (at1++ < 20 ? 1.5 : 0.6) : 1.2;
            run(&m, 1, w, 1, 3, 0);
            if (m.s.grace > 0 && m.s.ups != ups) up_during_grace = 1;
        }
        snprintf(b, sizeof b, "slowed, the clock rises: one probe to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.ups == ups && !up_during_grace && m.s.trial == 0 && m.s.probe == 2 && m.s.hold == FSTEP_HOLD0, b);
        snprintf(b, sizeof b, "  20 late frames in the grace are paid back: game time %.0f against real time %.2f (debt %.2f)", m.game, m.real, m.s.debt);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // slowed, but hold 1 really does not fit: each probe fails after its grace, and the probes spread out 1 → 2 → 4 → 8 s
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        double last = m.real, gaps[5]; int ng = 0, at1 = 0, first_len = -1; uint64_t probes = m.s.probes;
        for (int i = 0; i < 6000 && ng < 5; i++) {
            run(&m, 1, m.s.k == 1 ? 1.5 : 1.2, 1, 3, 0);
            if (m.s.k == 1) at1++; else if (at1 && first_len < 0) first_len = at1;
            if (m.s.probes != probes) { probes = m.s.probes; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "slowed, hold 1 does not fit: the first probe lasts its grace + 3 late frames (%d frames at hold 1)", first_len);
        check(first_len == FSTEP_GRACE + 3, b);
        snprintf(b, sizeof b, "  probes %.0f, %.0f, %.0f, %.0f vsyncs apart; %llu failed", gaps[1], gaps[2], gaps[3], gaps[4], (unsigned long long)m.s.failed);
        check(ng == 5 && gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[4] >= 480 && gaps[4] < 600 && m.s.failed >= 4, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // slowed, hold 1 holds for a while: a probe that holds but whose run at hold 1 ends within 8 s counts as failed, and the next waits twice as long
        sim m = {0}; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.3, 1, 3, 0); m.slow = 1;
        for (int i = 0; i < 1000 && m.s.probe != 2; i++) run(&m, 1, m.s.k == 1 ? 0.6 : 1.2, 1, 3, 0);
        double hold0 = m.s.hold; uint64_t failed = m.s.failed;
        run(&m, 120, 0.6, 1, 3, 0); run(&m, 3, 1.5, 1, 3, 0);
        snprintf(b, sizeof b, "slowed, hold 1 lasts 2 s after the probe: counted as failed (failed %llu, hold-off %.0f → %.0f vsyncs)", (unsigned long long)m.s.failed, hold0, m.s.hold);
        check(m.s.k == 2 && m.s.failed == failed + 1 && m.s.hold == 2 * hold0 && m.s.probe == 0, b);
        for (int i = 0; i < 2000 && m.s.probe != 2; i++) run(&m, 1, m.s.k == 1 ? 0.6 : 1.2, 1, 3, 0);
        hold0 = m.s.hold; failed = m.s.failed;
        run(&m, 600, 0.6, 1, 3, 0); run(&m, 3, 1.5, 1, 3, 0);
        snprintf(b, sizeof b, "  hold 1 lasts 10 s after the next probe: the hold-off returns to 1 s (failed %llu, hold-off %.0f → %.0f vsyncs)", (unsigned long long)m.s.failed, hold0, m.s.hold);
        check(m.s.k == 2 && m.s.failed == failed && hold0 == 120 && m.s.hold == FSTEP_HOLD0, b);
    }
    {   sim m = {0}; m.slow = 1; run(&m, 600, 0.5, 1, 3, 0);
        check(m.s.k == 1 && m.s.probes == 0 && m.s.downs == 0 && m.game == m.real, "slowed at hold 1 already: nothing to probe");
        sim c = {0}; c.slow = 1; run(&c, 300, 0.5, 2, 2, 0);
        check(c.s.k == 2 && c.s.probes == 0, "  slowed at the 30 cap (kmin 2): nothing to probe");
    }
    {   // Breath of the Wild on an iPhone: GPU 0.86 at hold 1, three heavy frames step to 2, where the GPU clock falls (1.5); after the probe it rises again within 20 frames
        sim m = {0}; nvn_fgpu g = {0}; int at1 = 0, stepped = 0;
        for (int i = 0; i < 2000; i++) {
            double w = i < 600 ? 0.86 : i < 603 ? 1.25 : m.s.k >= 2 ? 1.5 : at1++ < 20 ? 1.5 : 0.86;
            stepped |= i >= 603 && m.s.k == 2;
            m.slow = nvn_fstep_gpu_slowed(&g, w, m.s.k ? m.s.k : 1, 1, m.s.probe);
            run(&m, 1, w, 1, 3, 0);
        }
        snprintf(b, sizeof b, "GPU clock follows the hold (BotW): one probe back to hold 1 that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(stepped && m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // a heavier scene at the same clock: 0.86 at hold 1, then 1.12 (19 ms) for good, which is 1.3 times less than enough to call the GPU slowed
        sim m = {0}; nvn_fgpu g = {0};
        for (int i = 0; i < 2000; i++) { double w = i < 600 ? 0.86 : 1.12; m.slow = nvn_fstep_gpu_slowed(&g, w, m.s.k ? m.s.k : 1, 1, m.s.probe); run(&m, 1, w, 1, 3, 0); }
        check(m.s.k == 2 && m.s.probes == 0, "heavier scene, GPU 0.86 → 1.12 at hold 2: not slowed, no probe");
    }
    {   // a loading screen: the GPU at 0.03, then lag on the CPU steps to 2, where the GPU takes 0.17 (5.7 times as much but 3 ms): the CPU holds the frame back, not the GPU
        sim m = {0}; nvn_fgpu g = {0}; int slowed = 0;
        for (int i = 0; i < 1200; i++) { double gp = i < 600 || m.s.k == 1 ? 0.03 : 0.17, w = i < 600 ? 0.5 : 1.3; m.slow = nvn_fstep_gpu_slowed(&g, gp, m.s.k ? m.s.k : 1, 1, m.s.probe); slowed |= m.slow; run(&m, 1, w, 1, 3, 0); }
        check(m.s.k == 2 && !slowed && m.s.probes == 0, "GPU 0.03 → 0.17 under CPU lag: a GPU at 3 ms is not called slowed, no probe");
    }
    {   // the GPU stays slow at hold 1 too: each probe fails, the reference stays the hold-1 time from before, and the probes spread out
        sim m = {0}; nvn_fgpu g = {0}; double last = 0, gaps[4]; int ng = 0; uint64_t probes = 0;
        for (int i = 0; i < 6000 && ng < 4; i++) {
            double w = i < 600 ? 0.86 : i < 603 ? 1.25 : 1.5;
            m.slow = nvn_fstep_gpu_slowed(&g, w, m.s.k ? m.s.k : 1, 1, m.s.probe);
            run(&m, 1, w, 1, 3, 0);
            if (m.s.probes != probes) { probes = m.s.probes; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "GPU slow at hold 1 too: probes fail (%llu of %llu), reference kept at %.2f", (unsigned long long)m.s.failed, (unsigned long long)m.s.probes, g.ref);
        check(ng == 4 && m.s.failed >= 3 && g.ref < 1.0, b);
        snprintf(b, sizeof b, "  probes %.0f, %.0f, %.0f vsyncs apart; game time %.0f against real time %.2f (debt %.2f)", gaps[1], gaps[2], gaps[3], m.game, m.real, m.s.debt);
        check(gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
    }
    {   // Breath of the Wild on an iPhone (2026-10-21): frames of 0.80 / 0.92 vsyncs at hold 1 for 20 s, three pipeline compiles step to 2, and the scene costs the same there:
        // every other frame is above FSTEP_FIT, so no 30 fit in a row; the cost that held hold 1 starts one probe, which holds
        sim m = {0};
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        run(&m, 3, 1.6, 1, 3, 0);
        int stepped = m.s.k == 2; double ref = m.s.cref;
        for (int i = 0; i < 600; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        snprintf(b, sizeof b, "hitch after 20 s at hold 1, same cost at hold 2 (BotW): one probe back that holds (k %d, probes %llu, failed %llu)", m.s.k, (unsigned long long)m.s.probes, (unsigned long long)m.s.failed);
        check(stepped && ref > 0.84 && ref < 0.88 && m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0 && m.s.probe == 2 && m.s.downs == 1, b);
        snprintf(b, sizeof b, "  game time %.0f against real time %.2f (debt %.2f, dropped %.0f)", m.game, m.real, m.s.debt, m.s.dropped);
        check(m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01 && m.s.dropped == 0, b);
    }
    {   // the scene gets heavier (1.15 / 1.25 at hold 1, 1.4 times what held): hold 2 and no probe; back to the old scene, the cost that held starts one
        sim m = {0};
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        for (int i = 0; i < 1200; i++) run(&m, 1, i % 2 ? 1.25 : 1.15, 1, 3, 0);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.downs == 0, "heavier scene, cost 1.4 times what held hold 1: hold 2, no probe");
        for (int i = 0; i < 600; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        check(m.s.k == 1 && m.s.probes == 1 && m.s.failed == 0, "  the scene gets lighter again, back to the cost that held: one probe back that holds");
    }
    {   // hold 1 held only 5 s before the hitch: that run proves nothing (a device that steps up every few seconds keeps 30 rather than flap)
        sim m = {0};
        for (int i = 0; i < 300; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        run(&m, 3, 1.6, 1, 3, 0);
        for (int i = 0; i < 2000; i++) run(&m, 1, i % 2 ? 0.92 : 0.80, 1, 3, 0);
        check(m.s.k == 2 && m.s.probes == 0 && m.s.cref == 0, "hitch after 5 s at hold 1: no cost that held, no probe");
    }
    {   // the cost at hold 2 is what held, but hold 1 no longer fits (1.3 there): each probe fails, the probes spread out 1 → 2 → 4 → 8 s, and the cost that held is kept
        sim m = {0}; double last = 0, gaps[5]; int ng = 0; uint64_t probes = 0;
        run(&m, 1200, 0.86, 1, 3, 0); run(&m, 3, 1.6, 1, 3, 0);
        for (int i = 0; i < 6000 && ng < 5; i++) {
            run(&m, 1, m.s.k == 1 ? 1.3 : 0.86, 1, 3, 0);
            if (m.s.probes != probes) { probes = m.s.probes; gaps[ng++] = m.real - last; last = m.real; }
        }
        snprintf(b, sizeof b, "same cost at hold 2 but hold 1 does not fit: probes fail (%llu of %llu), cost that held kept at %.2f", (unsigned long long)m.s.failed, (unsigned long long)m.s.probes, m.s.cref);
        check(ng == 5 && m.s.failed >= 4 && m.s.cref > 0.85 && m.s.cref < 0.87, b);
        snprintf(b, sizeof b, "  probes %.0f, %.0f, %.0f, %.0f vsyncs apart; game time %.0f against real time %.2f (debt %.2f)", gaps[1], gaps[2], gaps[3], gaps[4], m.game, m.real, m.s.debt);
        check(gaps[1] >= 120 && gaps[2] >= 240 && gaps[3] >= 480 && gaps[4] >= 480 && gaps[4] < 600 && m.real - m.game - m.s.debt > -1.01 && m.real - m.game - m.s.debt < 1.01, b);
    }
    {   sim m = {0}; run(&m, 1200, 0.86, 1, 3, 0); run(&m, 1, 0.5, 2, 3, 0);
        check(m.s.cref == 0 && m.s.run <= 2, "movie flag (kmin 2): the cost that held kmin 1 is dropped");
    }
    {   // 165 Hz with virtual ticks (nvn_mtl.m link_range): cheap frames show on the refreshes nearest their slots, 12.1 / 18.2 ms at hold 1, 30.3 / 36.4 at 2, 48.5 / 54.5 at 3
        for (int sl = 1; sl >= 0; sl--) {
            nvn_fstep s = {.k = 3, .step = 3, .kmin = 1, .hold = FSTEP_HOLD0, .shown_slack = sl ? 60.0 / 165 : 0};
            for (int i = 0; i < 360; i++) {
                double ms = s.k == 3 ? (i % 4 ? 48.485 : 54.545) : s.k == 2 ? (i % 2 ? 30.303 : 36.364) : (i % 4 ? 18.182 : 12.121);
                nvn_fstep_next(&s, ms * 60 / 1000, 0.6, 1, 3, 0, 0);
            }
            snprintf(b, sizeof b, sl ? "165 Hz, cheap frames at hold 3, one refresh of slack: trials back to 2 and 1 (k %d, downs %llu)"
                                     : "  without the slack the 54.5 ms frames count as late: hold 3 (20 fps) for good (k %d, downs %llu)", s.k, (unsigned long long)s.downs);
            check(sl ? s.k == 1 && s.downs == 2 : s.k == 3 && s.downs == 0, b);
        }
        sim m = {0}; m.s.shown_slack = 60.0 / 165; run(&m, 60, 0.5, 1, 3, 0); run(&m, 300, 1.32, 1, 3, 0);
        check(m.s.k == 2, "  the slack does not hide work: 22 ms frames still step to hold 2");
    }
    printf("== %d checks, %d failed ==\n", checks, fail);
    return fail != 0;
}
