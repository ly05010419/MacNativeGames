// Engine frame limits kept in game memory (FPSLocker's evaluate_write entries): some engines cap their own frame rate with a value the host can raise. The target is the profile's
// fps_target (DM_FPS_TARGET overrides; 0 = leave the game's own limit), the same key that drives Tears of the Kingdom's frame step (loader.c install_fps_state). Called on every present
// (hle_nvn.c); the game sets the values after boot and may set them again, so each is written whenever it differs. Fixed addresses: each row runs only on the main build it was found in (AGENTS.md).
// Unreal Engine needs no row: the rewriter finds its console variables by their registered names in any build (dm/tools/ue_cvar.py, main's fixups UE_CVAR:<name>), and the host writes
// the frame limit, the profile's ue_fps_cvar (t.MaxFPS, or a game's own variable its settings apply to t.MaxFPS: SHIN MEGAMI TENSEI V: Vengeance's Project.FrameRateLimit), and the
// present interval, rhi.SyncInterval (nvn.SyncInterval in UE 4.18's Switch RHI: DRAGON QUEST XI S). Each is a TAutoConsoleVariable whose +0x10 points to its value, the game thread's
// copy then the render thread's. The games' configs set the limit to 30 and the interval to 2 (their notes name the .ini files)
#include "hle.h"
#include <mach/mach.h>

// What a field holds: the frame rate as a float, double or int32, vsyncs per frame (60 / frame rate) as an int32, or the frame time in milliseconds as a float. Copies: consecutive copies of the
// value (Unreal keeps one for the game thread, one for the render thread). A row with no pointer (ptr_off 0) has its field at main + field: a constant in main's read-only data, which the loader
// leaves writable (the image's data block on iOS too, loader.c map_image).
// At 60 a frame time gets MS_MARGIN less than the vsync: such a limiter counts from the frame's start, and the work after it (the GPU fence wait, the next frame's start) adds to the
// period, so frames at exactly 16.667 ms came 0.11 ms late each on the Mac (59.6 fps, every 150th frame shown two vsyncs). Slightly shorter, the host's present pacing sets the pace
// (the acquire waits): Luigi's Mansion 3's display interval p99 18.69 → 16.67 ms, 60.0 fps. Not at 30: with present_interval display the presents follow every vsync and hold no
// frame for two, so a shorter time showed some frames for one (30.4 fps); 30 writes the game's own 33.333 ms
// Also: a frame time in ticks as a uint32 (scale / frame rate: 19,200,000 for the Switch's 19.2 MHz system tick), a frame step in seconds as a float (1 / frame rate), and a mode
// switch an engine reads before it takes those fields at all (written 1 at any target)
enum { FPS_F32, VSYNCS_I32, MS_F32, FPS_I32, TICKS_U32, SECS_F32, MODE_U32, FPS_F64 };
#define MS_MARGIN 0.5f
static float ms_want(int fps) { return 1000.0f / (float)fps - (fps == 60 ? MS_MARGIN : 0); }
typedef struct { const char *build, *what; uint64_t ptr_off; uint32_t field; uint8_t kind, copies; uint32_t scale; } lock_t;
static const lock_t k_locks[] = {
    // Pokemon Brilliant Diamond 1.3.0: Unity's Application.targetFrameRate. Its icall registration (main+0xe34e60) resolves to main+0xdc8e70 -> +0x5fa640,
    // which stores w0 at main+0x4afc868; the getter at +0x5fa630 reads the same int32. The game sets 30 when entering the field; display pacing alone does not remove that cap.
    { "94ceae325c205c4b9d6f7235552f28fd", "Unity targetFrameRate", 0, 0x4afc868, FPS_I32, 1 },
    // Monster Hunter Rise 16.0.2: RE Engine's frame limit, a float at [main+0x1295b3f0]+0x434 (the pointer is a global in main's bss). The frame loop (main+0x6b106c8) sleeps until
    // [+0x430] / [+0x434] sixtieths of a second have passed ([+0x430] = 60) and steps the game by the time that really passed (+0x41c), so a higher limit runs the game faster in frames
    // at the same speed. The game sets 30 after boot; it also asks for present interval 2, which the profile's present_interval display ignores.
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100B04011742000/C9A3DD7702075ECD.yaml)
    { "c9a3dd7702075ecd90101f4555389582", "RE Engine frame limit", 0x1295b3f0, 0x434, FPS_F32, 1 },
    // Luigi's Mansion 3 1.4.0: Next Level Games' frame limiter. At the end of each frame the main thread calls the frame callbacks (main+0xb7c308) with the milliseconds since the frame
    // started; the limiter (main+0x7a3e70) runs jobs or sleeps until 33.333 ms have passed, the float at main+0x13c94dc (read only there, at main+0x7a3ea8 and main+0x7a3f18), and the game
    // steps by the time that really passed. The game asks for present interval 2 too, which the profile's present_interval display ignores; both are needed
    { "79e5950ffa85acf63e28c9aec051eac2", "NLG frame limiter target", 0, 0x13c94dc, MS_F32, 1 },
    // Need for Speed: Hot Pursuit Remastered 1.0.3: Criterion's frame timing. A uint32 in main's bss (main+0xf0043c) selects the engine's 60 fps mode, in which it takes its frame time from
    // the timer object at [main+0xed96a8]: +0x30 the frame in system ticks (19.2 MHz, 640,000 at 30 fps) and +0x28 a tenth of it; the game's own step, at [main+0xeffc60] +0x30 (2D) and +0x50
    // (3D), is a float of seconds per frame. Its limiter, a spin on the system tick at main+0x483588..+0x4835a4, is cut by the rewriter (patches.py). From masagrator's FPSLocker config for this
    // build (FPSLocker-Warehouse, patches/010029B0118E8000/799D1061182C1302.yaml; the frame-time instruction found by theboy181)
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 60 fps mode", 0, 0xf0043c, MODE_U32, 1, 0 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion frame ticks", 0xed96a8, 0x30, TICKS_U32, 1, 19200000 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion frame ticks / 10", 0xed96a8, 0x28, TICKS_U32, 1, 1920000 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 2D game step", 0xeffc60, 0x30, SECS_F32, 1, 0 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 3D game step", 0xeffc60, 0x50, SECS_F32, 1, 0 },
    // EA SPORTS FC 27 1.8d.310c: the game's frame regulator, an object at [main+0xda20e38] (its constructor, main+0x99e80c8, sets 60.0). Its tick (main+0x99ea580) waits until
    // 1 / [+0x8] seconds have passed since the last frame; [+0x10] is the rate it reports. main+0x5d501c0 sets both to 30.0 whenever GameRenderSettings.Enable is 1 (fmov v1.2d, #30.0;
    // stur q1, [x9, #8]), a constant of the Switch build, so it is written here. The simulation's own rate is Frostbite's SimulationTimeSettings, set below by name: on a build without
    // this row the game keeps 30 fps at the right speed (the simulation at 60 and the regulator at 30: match clock as at 30). The game asks for present interval 2
    // (BaseDisplaySettings.PresentInterval), which the profile's present_interval display ignores
    { "a48c0de3bc353efa0e4d359bde05ef24", "FIFA frame regulator", 0xda20e38, 0x8, FPS_F64, 2, 0 },
};
#define NLOCKS ((int)(sizeof k_locks / sizeof k_locks[0]))

static int rd(uint64_t a, void *out, size_t n) { vm_size_t got = 0; return a >= 0x1000 && vm_read_overwrite(mach_task_self(), (vm_address_t)a, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n; }

// NieR 1.0.3 exposes its preset minimum but ships fixed mode-specific fallbacks.
// Keep the native current-scale and UV controller intact; only select its
// verified lower bound. This is independent of the FPS choice.
// The 1.0.3 builds the rewrite cuts (patches.py): the scale clamp and the publish branch that are checked, the two fallback floors the cuts
// replace, and the preset minimum. The Asian release (0100B8E016F76000) shifts the function by 0x13c0; its preset minimum is the target of the
// relocation of the slot the function loads it through (main+0x12c8500).
// display is the game's display object: its first word points to the NVN device (the present interval at +0x9c160), and +0x504 is the frame-interval
// mode (0: 60 fps, 1: 30 fps) the setter main+0x9c0590 (Asian release; the global's 0x13c0 before) writes; in the Asian release it is the target of the
// relocation of the slot that setter reads (main+0x12c2d18).
static const struct { const char *build; uint32_t clamp, publish, floor0, floor1, minimum, display; } k_nier_dr[] = {
    {"3c10ec35c5ef4c3686b4053afb0e58e7", 0x9bf568, 0x9bf578, 0x9bf528, 0x9bf534, 0x128e068, 0x192be00},
    {"98397a10419894652f372d4dc1b6fc2c", 0x9c0928, 0x9c0938, 0x9c08e8, 0x9c08f4, 0x128f130, 0x192d600},
};
static void dynamic_resolution_tick(void) {
    static int state = -1, dynamic, row;
    static uint8_t *base;
    if (state < 0) {
        state = 0;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
            for (int k = 0; k < (int)(sizeof k_nier_dr / sizeof *k_nier_dr); k++) if (!strcmp(g_mod[i].build_id, k_nier_dr[k].build)) { base = g_mod[i].base; row = k; }
        if (!base) return;
        const char *e=getenv("DM_DYNAMIC_RESOLUTION");
        if (e && strcmp(e,"0") && strcmp(e,"1")) LOG("[RED] DM_DYNAMIC_RESOLUTION=%s: must be 0 or 1; keeping full resolution",e);
        dynamic=e && !strcmp(e,"1");
        if (dynamic) {
            uint32_t clamp = 0, publish = 0;
            uint64_t floors[2] = {0};
            if (!rd((uint64_t)(base + k_nier_dr[row].clamp), &clamp, 4) || !rd((uint64_t)(base + k_nier_dr[row].publish), &publish, 4) ||
                !rd((uint64_t)(base + k_nier_dr[row].floor0), &floors[0], 8) || !rd((uint64_t)(base + k_nier_dr[row].floor1), &floors[1], 8) ||
                clamp != 0x1e254c80u || publish != 0x54000489u ||
                floors[0] != 0x72a7f00c5280000cull || floors[1] != 0x72a7f00c5280000cull) {
                LOG("[RED] dynamic resolution needs a verified native scale/UV rewrite; keeping full resolution");
                dynamic = 0;
            }
        }
        state=1;
        LOG("main: NieR dynamic resolution %s (DM_DYNAMIC_RESOLUTION)",dynamic?"on; native controller, 70–100% scale":"off; full resolution");
    }
    if (!state) return;
    uint32_t cur[2];
    uint64_t a = (uint64_t)(base + k_nier_dr[row].minimum);
    if (!rd(a, cur, sizeof cur)) return;
    uint32_t minimum = dynamic ? 0x3f333333u : 0x3f800000u;
    if (cur[0] != minimum) {
        uint32_t *p = (uint32_t *)(uintptr_t)a;
        dm_guest_write_prepare(p, 4);
        __atomic_store_n(p, minimum, __ATOMIC_RELAXED);
    }
    if (!dynamic && cur[1] != 0x3f800000u) {
        uint32_t *p = (uint32_t *)(uintptr_t)(a + 4);
        dm_guest_write_prepare(p, 4);
        __atomic_store_n(p, 0x3f800000u, __ATOMIC_RELAXED);
    }
}

// Frostbite's simulation rate, found by name (no fixed addresses, so it holds across a game's updates). Frostbite reflects its settings classes: a class record (its name, then at
// +0x40 its field list) and per field a record of 0x20 bytes (name, hash, offset at +0x10, type). SimulationTimeSettings' ForceSimRate (int32) and MaxSimFps (float) cap the
// simulation's ticks a second: 30 in EA SPORTS FC 27, whose frames then cannot go faster than 30 whatever paces them. A static constructor registers the class: it stores the record's
// address into its type info (adrp / add of the record, then str into the type info just made by adrp / add), and the code that reads the settings asks the settings registry for the
// instance with that type info in x1 and a cache word in x2 (adrp / add each, then bl), which keeps the instance's address. The instance holds its type info at +8, which checks the cache.
// Both fields are written each frame they differ (the game may load its settings again), like the frame limits above
static struct { int n; uint8_t *base; uint64_t ti, cache[8]; int ncache; uint32_t off_rate, off_max; } fb;
static int fb_name_at(uint8_t *base, uint64_t size, uint64_t p, const char *want) {   // p, a relocated pointer, points into the module at the string want
    uint64_t b = (uint64_t)(uintptr_t)base; size_t n = strlen(want);
    return p >= b && p - b + n + 1 <= size && !memcmp((const void *)(uintptr_t)p, want, n + 1);
}
static int64_t fb_pair(const uint32_t *t, uint64_t i, int reg) {   // the target of adrp / add into reg at words i, i + 1, or -1 (module offsets: the text sits at offset 0)
    uint32_t w = t[i], x = t[i + 1];
    if ((w & 0x9F000000u) != 0x90000000u || (int)(w & 31) != reg || (x & 0xFFC00000u) != 0x91000000u || (x & 31) != (uint32_t)reg || ((x >> 5) & 31) != (uint32_t)reg) return -1;
    int64_t imm = (int64_t)(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)); if (imm & (1 << 20)) imm -= 1 << 21;
    return (int64_t)((i * 4) & ~0xfffull) + imm * 4096 + ((x >> 10) & 0xfff);
}
static void fb_find(void) {
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || !m->text_end || !m->ro_off || m->ro_off >= m->size) return;
    static const char cls[] = "SimulationTimeSettings";
    uint8_t *base = m->base, *ro = base + m->ro_off; uint64_t size = m->back_off ? m->back_off : m->size, b = (uint64_t)(uintptr_t)base;
    const uint8_t *s = memmem(ro, (size_t)(size - m->ro_off), "\0SimulationTimeSettings\0", sizeof cls + 1); if (!s) return;   // not Frostbite
    uint64_t name = (uint64_t)(uintptr_t)(s + 1), rec = 0;
    for (uint64_t o = (m->ro_off + 7) & ~7ull; o + 0x48 <= size && !rec; o += 8) {
        if (*(uint64_t *)(base + o) != name) continue;
        uint64_t fl = *(uint64_t *)(base + o + 0x40); if (fl < b || fl - b + 0x20 > size) continue;
        for (uint64_t f = fl - b; f + 0x20 <= size; f += 0x20) {   // the field list ends where a name stops being one
            uint64_t fn = *(uint64_t *)(base + f); if (fn < b || fn - b >= size) break;
            uint32_t off = *(uint32_t *)(base + f + 0x10);
            if (fb_name_at(base, size, fn, "ForceSimRate")) fb.off_rate = off;
            else if (fb_name_at(base, size, fn, "MaxSimFps")) fb.off_max = off;
        }
        if (fb.off_rate && fb.off_max) rec = o;
    }
    if (!rec) { LOG("[RED] main: Frostbite's %s found, but not its ForceSimRate and MaxSimFps fields; leaving the simulation rate", cls); return; }
    const uint32_t *t = (const uint32_t *)base; uint64_t nw = m->text_end / 4;
    for (uint64_t i = 0; i + 3 < nw && !fb.ti; i++) {   // the registration: adrp / add xR = record, str xR, [xT] with xT = the type info from an adrp / add just before
        int r = (int)(t[i] & 31); if (fb_pair(t, i, r) != (int64_t)rec) continue;
        uint32_t st = t[i + 2]; if ((st & 0xFFFFFC00u) != 0xF9000000u || (int)(st & 31) != r) continue;   // str xR, [xT] (no offset)
        int xt = (int)((st >> 5) & 31);
        for (uint64_t k = i >= 6 ? i - 6 : 0; k < i; k++) { int64_t ti = fb_pair(t, k, xt); if (ti > 0) fb.ti = (uint64_t)ti; }
    }
    if (!fb.ti) { LOG("[RED] main: Frostbite's %s: its type info was not found; leaving the simulation rate", cls); return; }
    for (uint64_t i = 8; i + 12 < nw && fb.ncache < 8; i++) {   // the readers: adrp / add x1 = type info, adrp / add x2 = cache word, near each other before a bl
        if (fb_pair(t, i, 1) != (int64_t)fb.ti) continue;
        for (uint64_t k = i - 8; k < i + 10; k++) {
            int64_t c = fb_pair(t, k, 2); if (c <= 0 || (uint64_t)c + 8 > m->size) continue;
            int known = 0; for (int j = 0; j < fb.ncache; j++) known |= fb.cache[j] == (uint64_t)c;
            if (!known) fb.cache[fb.ncache++] = (uint64_t)c;
            break;
        }
    }
    if (!fb.ncache) { LOG("[RED] main: Frostbite's %s: no settings lookup found; leaving the simulation rate", cls); return; }
    fb.base = base; fb.n = 1;
    LOG("main: Frostbite %s at main+%#llx (ForceSimRate +%#x, MaxSimFps +%#x), type info main+%#llx, %d instance caches", cls, (unsigned long long)rec, fb.off_rate, fb.off_max,
        (unsigned long long)fb.ti, fb.ncache);
}
static void fb_tick(int target, const char *src) {
    static int nlog; uint64_t ti = (uint64_t)(uintptr_t)(fb.base + fb.ti);
    for (int j = 0; j < fb.ncache; j++) {
        uint64_t inst = 0, tinfo = 0; int32_t rate; float max;
        if (!rd((uint64_t)(uintptr_t)(fb.base + fb.cache[j]), &inst, sizeof inst) || !inst || !rd(inst + 8, &tinfo, sizeof tinfo) || tinfo != ti) continue;   // not made yet, or another word
        if (!rd(inst + fb.off_rate, &rate, sizeof rate) || !rd(inst + fb.off_max, &max, sizeof max)) continue;
        if (rate != target) { int32_t *p = (int32_t *)(uintptr_t)(inst + fb.off_rate); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, target, __ATOMIC_RELAXED); }
        if (max != (float)target) { float w = (float)target, *p = (float *)(uintptr_t)(inst + fb.off_max); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n((uint32_t *)p, *(uint32_t *)&w, __ATOMIC_RELAXED); }
        if ((rate != target || max != (float)target) && nlog < 8) { nlog++; LOG("main: Frostbite simulation rate ForceSimRate %d, MaxSimFps %g → %d (%s)", rate, max, target, src); }
        return;
    }
}

// The Unreal rows of this main: the profile's frame-limit variable and the present intervals, at the objects the rewriter found (0 rows: not an Unreal main). *missing: an Unreal main
// (the rewriter found some of its variables) without the profile's frame-limit variable
static int ue_rows(const module_t *m, lock_t *out, int *missing) {
    static char what[3][48]; int n = 0, have = 0;
    for (int k = 0; k < m->n_ue_cvar && n < 3; k++) {
        const char *name = m->ue_cvar[k].name; int fps = !strcmp(name, DM_GAME_UE_FPS_CVAR);
        if (!fps && strcmp(name, "rhi.SyncInterval") && strcmp(name, "nvn.SyncInterval")) continue;
        have |= fps; snprintf(what[n], sizeof what[n], "Unreal %s", name);
        out[n] = (lock_t){ "", what[n], m->ue_cvar[k].off + 0x10, 0, fps ? FPS_F32 : VSYNCS_I32, 2, 0 }; n++;
    }
    *missing = m->n_ue_cvar > 0 && !have;
    return n;
}

// The running main's engine limiters, found once: by the first present (dm_fps_lock_tick) or by the graphics layer's present cap, which may ask first (dm_fps_lock_engine, from the acquire)
static lock_t ue[3]; static const lock_t *rows[NLOCKS + 3]; static int nrow, nier, ue_missing; static uint8_t *base; static uint32_t display;
static pthread_once_t g_found = PTHREAD_ONCE_INIT;
static void lock_find(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        for (int k = 0; k < (int)(sizeof k_nier_dr / sizeof *k_nier_dr); k++) if (!strcmp(g_mod[i].build_id, k_nier_dr[k].build)) { base = g_mod[i].base; nier = 1; display = k_nier_dr[k].display; }
        for (int k = 0; k < NLOCKS; k++) if (!strcmp(g_mod[i].build_id, k_locks[k].build)) { rows[nrow++] = &k_locks[k]; base = g_mod[i].base; }
        int nue = ue_rows(&g_mod[i], ue, &ue_missing);
        for (int k = 0; k < nue; k++) { rows[nrow++] = &ue[k]; base = g_mod[i].base; }
    }
    fb_find();
}
// Whether the host writes fps_target into an engine limiter of this main (a row for its build, NieR's frame interval, Unreal's variables or Frostbite's simulation rate, found by name).
// Without one, and without a frame step state page, nothing makes the game follow the cap, so the graphics layer keeps the game's own present interval (hle_nvn.c fps_present_cap):
// presenting at a cap the game does not step by runs a fixed-step game fast or slow (Tears of the Kingdom 1.4.2 at 2x with DM_FPS_TARGET=60)
int dm_fps_lock_engine(void) { pthread_once(&g_found, lock_find); return nrow || nier || fb.n; }
void dm_fps_lock_tick(void) {
    dynamic_resolution_tick();   // separate from the FPS cap
    static int state = -2, nlog[NLOCKS + 3], target; static const char *src;
    if (state == -1) return;
    if (state == -2) {
        state = -1; pthread_once(&g_found, lock_find);
        if (!nrow && !nier && !fb.n && !ue_missing) return;
        const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET; src = e ? "DM_FPS_TARGET" : "game profile fps_target";
        if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; leaving the game's frame limit", e); t = 0; }
        if (t && ue_missing) LOG("[RED] main: Unreal %s not found by the rewriter (ue_cvar.py NAMES; game profile ue_fps_cvar): the game's own frame limit stays", DM_GAME_UE_FPS_CVAR);
        if (!nrow && !nier && !fb.n) return;
        if (!t) { for (int r = 0; r < nrow; r++) LOG("main: %s left as the game sets it (%s)", rows[r]->what, src); if (nier) LOG("main: NieR frame interval left as the game sets it (%s)", src);
                  if (fb.n) LOG("main: Frostbite simulation rate left as the game sets it (%s)", src); return; }
        target = t; state = 0;
    }
    if (nier) {
        // NieR:Automata US/EU 1.0.3 (3c10ec35c5ef4c3686b4053afb0e58e7):
        // - [base + 0x192be00] points to the NVN graphics device object whose +0x9c160 holds the present interval (1 = 60 FPS, 2 = 30 FPS),
        //   and base + 0x192c304 (+0x504) holds the requested interval mode (0 = 60 FPS, 1 = 30 FPS).
        // Resolution policy is applied independently above, so 30/60 FPS does
        // not implicitly enable or disable the native resolution controller.
        uint64_t p = 0; static int nlog_nier;
        uint32_t want = (uint32_t)(60 / target), want_mode = want - 1, cur_mode = 0;
        if (rd((uint64_t)(uintptr_t)(base + display + 0x504), &cur_mode, sizeof cur_mode) && cur_mode != want_mode && (cur_mode == 0 || cur_mode == 1)) {
            uint32_t *dst = (uint32_t *)(uintptr_t)(base + display + 0x504);
            dm_guest_write_prepare(dst, sizeof *dst);
            __atomic_store_n(dst, want_mode, __ATOMIC_RELAXED);
        }
        if (rd((uint64_t)(uintptr_t)(base + display), &p, sizeof p) && p) {
            uint64_t a = p + 0x9c160; uint32_t cur = 0;
            if (rd(a, &cur, sizeof cur) && cur != want && (cur == 1 || cur == 2)) {
                uint32_t *dst = (uint32_t *)(uintptr_t)a;
                dm_guest_write_prepare(dst, sizeof *dst); __atomic_store_n(dst, want, __ATOMIC_RELAXED);
                if (nlog_nier++ < 8) LOG("main: NieR frame interval %u → %u (%d fps; %s)", cur, want, target, src);
            }
        }
    }
    if (fb.n) fb_tick(target, src);
    for (int r = 0; r < nrow; r++) {
        const lock_t *L = rows[r]; uint64_t obj = (uint64_t)(uintptr_t)base;
        if (L->ptr_off && (!rd((uint64_t)(uintptr_t)(base + L->ptr_off), &obj, sizeof obj) || !obj)) continue;   // not created yet
        if (L->kind == FPS_F64) {
            for (int c = 0; c < L->copies; c++) {
                uint64_t a = obj + L->field + 8 * (uint64_t)c; double cur, want = target;
                if (!rd(a, &cur, sizeof cur)) break;
                if (cur == want || !(cur >= 10 && cur <= 120)) continue;   // already set, or not set yet
                double *p = (double *)(uintptr_t)a;
                dm_guest_write_prepare(p, sizeof *p); __atomic_store_n((uint64_t *)p, *(uint64_t *)&want, __ATOMIC_RELAXED);
                if (nlog[r] < 8) { nlog[r]++; LOG("main: %s%s %g → %d fps (%s)", L->what, L->copies > 1 ? (c ? " (reported rate)" : " (target)") : "", cur, target, src); }
            }
            continue;
        }
        for (int c = 0; c < L->copies; c++) {
            uint64_t a = obj + L->field + 4 * (uint64_t)c; uint32_t cur, want; char was[16];
            if (!rd(a, &cur, sizeof cur)) break;
            if (L->kind == FPS_F32 || L->kind == MS_F32) {
                int ms = L->kind == MS_F32; float f, w = ms ? ms_want(target) : (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(ms ? f >= 8 && f <= 100 : f >= 10 && f <= 120)) continue;   // already set, or not set yet (RE Engine: 6000 at boot; Unreal: 0 = no limit until the config sets it)
                snprintf(was, sizeof was, ms ? "%.3f ms" : "%g", f);
            } else if (L->kind == SECS_F32) {
                float f, w = 1.0f / (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(f >= 1.0f / 120 && f <= 0.1f)) continue;   // not set yet (0 before the game's first frame)
                snprintf(was, sizeof was, "%.5f s", f);
            } else if (L->kind == TICKS_U32) {
                want = L->scale / (uint32_t)target;
                if (cur == want || cur < L->scale / 120 || cur > L->scale / 10) continue;
                snprintf(was, sizeof was, "%u", cur);
            } else if (L->kind == MODE_U32) {
                want = 1;
                if (cur) continue;   // already 1, or not this switch: 0 is the engine's default
                snprintf(was, sizeof was, "%u", cur);
            } else {
                int fps = L->kind == FPS_I32;
                want = fps ? (uint32_t)target : 60 / (uint32_t)target;
                if (cur == want || cur < (fps ? 10u : 1u) || cur > (fps ? 120u : 4u)) continue;
                snprintf(was, sizeof was, "%u", cur);
            }
            uint32_t *p = (uint32_t *)(uintptr_t)a;
            dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog[r] < 8) { nlog[r]++; char now[24];
                if (L->kind == MS_F32) snprintf(now, sizeof now, "%.3f ms (%d fps)", ms_want(target), target);
                else if (L->kind == SECS_F32) snprintf(now, sizeof now, "%.5f s (%d fps)", 1.0 / target, target);
                else if (L->kind == TICKS_U32) snprintf(now, sizeof now, "%u (%d fps)", want, target);
                else if (L->kind == MODE_U32) snprintf(now, sizeof now, "1 (%d fps)", target);
                else snprintf(now, sizeof now, "%u%s", L->kind == FPS_F32 ? (unsigned)target : want, L->kind == FPS_F32 || L->kind == FPS_I32 ? " fps" : " vsyncs");
                LOG("main: %s%s %s → %s (%s)", L->what, L->copies > 1 ? (c ? " (render thread)" : " (game thread)") : "", was, now, src); }
        }
    }
}

// The Mac's DM_CTL "cvarf <main offset> <value>" (hle_hid.c), for trying an Unreal console variable while the game runs: the TAutoConsoleVariable at main+offset holds at +0x10
// the pointer to its value, the game thread's copy then the render thread's; both are set to the float
void dm_ctl_cvar_f32(uint64_t off, float v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvarf main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { float *p = (float *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvarf main+%#llx = %g (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
// "peek <guest address> <count>": count floats (and their bits) from guest memory, read safely, to the log
void dm_ctl_peek(uint64_t a, int n) {
    for (int i = 0; i < n && i < 64; i += 4) {
        uint32_t w[4] = {0}; int k = n - i < 4 ? n - i : 4;
        if (!rd(a + 4 * (uint64_t)i, w, 4 * (size_t)k)) { LOG("DM_CTL: peek %#llx: not readable", (unsigned long long)(a + 4 * (uint64_t)i)); return; }
        float f[4]; memcpy(f, w, sizeof f);
        LOG("DM_CTL: peek %#llx: %g %g %g %g (%08x %08x %08x %08x)", (unsigned long long)(a + 4 * (uint64_t)i), f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3]);
    }
}
// "memdump <guest address> <bytes> <path>": guest memory to a file, read safely (stops at the first unreadable page)
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] DM_CTL: memdump: cannot open %s", path); return; }
    static uint8_t buf[1 << 16]; uint64_t done = 0;
    while (done < n) { size_t k = n - done < sizeof buf ? (size_t)(n - done) : sizeof buf; if (!rd(a + done, buf, k)) break; fwrite(buf, 1, k, f); done += k; }
    fclose(f); LOG("DM_CTL: memdump %#llx: %llu of %llu bytes → %s", (unsigned long long)a, (unsigned long long)done, (unsigned long long)n, path);
}
// "cvari <main offset> <value>": the same for an int console variable
void dm_ctl_cvar_i32(uint64_t off, int32_t v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvari main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { int32_t *p = (int32_t *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvari main+%#llx = %d (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
