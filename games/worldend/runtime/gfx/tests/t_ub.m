#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { long long w=(want), g=(got); count++; printf("%s expected %lld → got %lld %s\n", name, w, g, w==g?"OK":"FAIL"); if(w!=g) fail++; } while(0)
int g_t_ub_var = 7, g_t_ub_var2 = -3;   // global variables found by @set via symbol name
// Unit tests for the phase logic of same-process alternating measurement (DM_NVN_UB): no rendering is started, it only advances by frame number.
// Configuration: two classes, 60 frames per phase. Phase order = baseline, a, baseline, b, baseline, a, ...; a = set g_t_ub_var=5 and drop keys abcd...; b = set g_t_ub_var2=9 and drop everything (*).
int main(void) {
    setenv("DM_NVN_UB", "a=@set:g_t_ub_var=5,abcd;b=@set:g_t_ub_var2=9,*;c=@set:g_no_such_var=1", 1); setenv("DM_NVN_UB_N", "60", 1);
    struct { int f, v1, v2, skip_abcd, skip_other; const char *what; } T[] = {
        {0, 7, -3, 0, 0, "phase 0 baseline"}, {59, 7, -3, 0, 0, "phase 0 last frame"},
        {60, 5, -3, 1, 0, "phase 1 = a: variable 1 set, abcd dropped"}, {119, 5, -3, 1, 0, "phase 1 last frame"},
        {120, 7, -3, 0, 0, "phase 2 baseline: variable 1 restored"},
        {180, 7, 9, 1, 1, "phase 3 = b: variable 2 set, everything dropped"},
        {240, 7, -3, 0, 0, "phase 4 baseline: variable 2 restored"},
        {300, 7, -3, 0, 0, "phase 5 = c: variable does not exist, nothing changes"},
        {360, 7, -3, 0, 0, "phase 6 baseline"},
        {420, 5, -3, 1, 0, "phase 7 = a (second round)"},
    };
    int ti = 0, n = (int)(sizeof T / sizeof T[0]);
    for (int f = 0; f <= 420 && ti < n; f++) { mtl_set_frame((uint64_t)f);
        if (f != T[ti].f) continue;
        char nm[96]; snprintf(nm, sizeof nm, "%s variable 1", T[ti].what); EXPECT(nm, T[ti].v1, g_t_ub_var);
        snprintf(nm, sizeof nm, "%s variable 2", T[ti].what); EXPECT(nm, T[ti].v2, g_t_ub_var2);
        snprintf(nm, sizeof nm, "%s drops abcd0123", T[ti].what); EXPECT(nm, T[ti].skip_abcd, ub_skip("abcd0123ffff0000"));
        snprintf(nm, sizeof nm, "%s drops other keys", T[ti].what); EXPECT(nm, T[ti].skip_other, ub_skip("9999888877776666"));
        ti++; }
    EXPECT("checkpoints reached", n, ti);
    // On-screen present intervals are tallied per phase: counted only from frame 45 of the phase on; boundaries of the three bands are 20.8 / 29.2 ms; −1 is recorded as discarded
    for (int f = 480; f < 480 + 45; f++) { mtl_set_frame((uint64_t)f); mtl_ub_present(16.7); }   // transition segment: not counted
    EXPECT("transition segment not counted", 0, atomic_load(&g_ubp_c[0]));
    mtl_set_frame(480 + 45); mtl_ub_present(16.7); mtl_ub_present(25.0); mtl_ub_present(25.0); mtl_ub_present(33.3); mtl_ub_present(-1); mtl_ub_present(0);
    EXPECT("16.7 ms band", 1, atomic_load(&g_ubp_c[0])); EXPECT("25 ms band", 2, atomic_load(&g_ubp_c[1])); EXPECT("≥33 ms band", 1, atomic_load(&g_ubp_c[2])); EXPECT("discarded", 1, atomic_load(&g_ubp_c[3]));
    EXPECT("sum of intervals (microseconds)", 16700 + 25000 + 25000 + 33300, (long long)atomic_load(&g_ubp_sum_us));
    printf("t_ub: %d checks, %d failed\n", count, fail); return fail != 0;
}
