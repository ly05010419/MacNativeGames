// Engine frame limits kept in game memory (FPSLocker's evaluate_write entries): some engines cap their own frame rate with a value the host can raise. The target is the profile's
// fps_target (DM_FPS_TARGET overrides; 0 = leave the game's own limit), the same key that drives Tears of the Kingdom's frame step (loader.c install_fps_state). Called on every present
// (hle_nvn.c); the game sets the values after boot and may set them again, so each is written whenever it differs. Fixed addresses: each row runs only on the main build it was found in (AGENTS.md)
#include "hle.h"
#include <mach/mach.h>

// What a field holds: the frame rate as a float or int32, vsyncs per frame (60 / frame rate) as an int32, or the frame time in milliseconds as a float. Copies: consecutive copies of the value
// (Unreal keeps one for the game thread, one for the render thread). A row with no pointer (ptr_off 0) has its field at main + field: a constant in main's read-only data, which the loader
// leaves writable (the image's data block on iOS too, loader.c map_image).
// At 60 a frame time gets MS_MARGIN less than the vsync: such a limiter counts from the frame's start, and the work after it (the GPU fence wait, the next frame's start) adds to the
// period, so frames at exactly 16.667 ms came 0.11 ms late each on the Mac (59.6 fps, every 150th frame shown two vsyncs). Slightly shorter, the host's present pacing sets the pace
// (the acquire waits): Luigi's Mansion 3's display interval p99 18.69 → 16.67 ms, 60.0 fps. Not at 30: with present_interval display the presents follow every vsync and hold no
// frame for two, so a shorter time showed some frames for one (30.4 fps); 30 writes the game's own 33.333 ms
enum { FPS_F32, VSYNCS_I32, MS_F32, FPS_I32 };
#define MS_MARGIN 0.5f
static float ms_want(int fps) { return 1000.0f / (float)fps - (fps == 60 ? MS_MARGIN : 0); }
static const struct { const char *build, *what; uint64_t ptr_off; uint32_t field; uint8_t kind, copies; } k_locks[] = {
    // Pokemon Brilliant Diamond 1.3.0: Unity's Application.targetFrameRate. Its icall registration (main+0xe34e60) resolves to main+0xdc8e70 -> +0x5fa640,
    // which stores w0 at main+0x4afc868; the getter at +0x5fa630 reads the same int32. The game sets 30 when entering the field; display pacing alone does not remove that cap.
    { "94ceae325c205c4b9d6f7235552f28fd", "Unity targetFrameRate", 0, 0x4afc868, FPS_I32, 1 },
    // Monster Hunter Rise 16.0.2: RE Engine's frame limit, a float at [main+0x1295b3f0]+0x434 (the pointer is a global in main's bss). The frame loop (main+0x6b106c8) sleeps until
    // [+0x430] / [+0x434] sixtieths of a second have passed ([+0x430] = 60) and steps the game by the time that really passed (+0x41c), so a higher limit runs the game faster in frames
    // at the same speed. The game sets 30 after boot; it also asks for present interval 2, which the profile's present_interval display ignores.
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100B04011742000/C9A3DD7702075ECD.yaml)
    { "c9a3dd7702075ecd90101f4555389582", "RE Engine frame limit", 0x1295b3f0, 0x434, FPS_F32, 1 },
    // Octopath Traveler 0 1.0.5: Unreal Engine 5's console variables t.MaxFPS (the game thread's frame limit: it sleeps until 1 / t.MaxFPS has passed and steps the game by the time that
    // really passed) and rhi.SyncInterval (vsyncs per present: the RHI's present interval). Each is a TAutoConsoleVariable in main's bss (vtable, the console object, then at +0x10 the pointer to
    // its value: the game thread's copy, then the render thread's). The game's config sets t.MaxFPS 30 ([ConsoleVariables] of DefaultEngine.ini; FrameRateLimit=30 of SwitchGameUserSettings.ini
    // sets it again) and rhi.SyncInterval 2 (SwitchDeviceProfiles.ini), so the game asks for present interval 2. Found from the names' references in the static constructors that register them
    // (main+0x2ca6524, main+0x1026e54; RegisterConsoleVariable, then AsVariableFloat / AsVariableInt stored at +0x10)
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal t.MaxFPS", 0x7c738d0 + 0x10, 0, FPS_F32, 2 },
    { "9fe60a212cff9b9a3656ab63d4f882e5", "Unreal rhi.SyncInterval", 0x7b32690 + 0x10, 0, VSYNCS_I32, 2 },
    // DRAGON QUEST XI S 1.0.4: Unreal Engine 4.18's t.MaxFPS and nvn.SyncInterval (the Switch RHI's present interval), the same TAutoConsoleVariable layout. The game's config sets
    // FrameRateLimit=30 (DefaultGameUserSettings.ini) and nvn.SyncInterval=2 (JackGame/Config/Switch/SwitchEngine.ini). Found from the names' references (UTF-32, as the engine's TCHAR on
    // the Switch) in the static constructors: main+0x1e7f98 (TAutoConsoleVariable<float> constructor main+0x6ee0e0, AsVariableFloat stored at +0x10) and main+0x21c00c
    { "1719aabfa5eae42b132b65770b871288", "Unreal t.MaxFPS", 0x8501c38 + 0x10, 0, FPS_F32, 2 },
    { "1719aabfa5eae42b132b65770b871288", "Unreal nvn.SyncInterval", 0x85270b8 + 0x10, 0, VSYNCS_I32, 2 },
    // DRAGON QUEST VII Reimagined 1.2.0: Unreal Engine 4.27's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16, the engine's TCHAR in this build). The game's config sets
    // FrameRateLimit=30 (DefaultGameUserSettings.ini, SwitchGameUserSettings.ini) and rhi.syncinterval=2 (SwitchDeviceProfiles.ini). Registered at main+0x4a61b40 (float, AsVariableFloat
    // stored at +0x10) and main+0x36164f4 (int, AsVariableInt at +0x10)
    { "f7ec03e4cb9de3bd8319d4e899d2e9cc", "Unreal t.MaxFPS", 0x7f41908 + 0x10, 0, FPS_F32, 2 },
    { "f7ec03e4cb9de3bd8319d4e899d2e9cc", "Unreal rhi.SyncInterval", 0x7dedf38 + 0x10, 0, VSYNCS_I32, 2 },
    // FINAL FANTASY RESONANCE 1.0.1: Unreal Engine 5.6.1's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16). The engine's Switch device profile sets rhi.syncinterval=2
    // (BaseSwitchDeviceProfiles.ini). Registered at main+0x33a4dec (float, IConsoleManager slot 2; AsVariableFloat stored at +0x10) and main+0x149e0bc (int, slot 1; AsVariableInt at +0x10)
    { "84e182f72ee1da4f477804bd05b54c15", "Unreal t.MaxFPS", 0x8c29af0 + 0x10, 0, FPS_F32, 2 },
    { "84e182f72ee1da4f477804bd05b54c15", "Unreal rhi.SyncInterval", 0x8adca10 + 0x10, 0, VSYNCS_I32, 2 },
    // Asian release 1.0.1: PR #289 verified the registering constructor at main+0x149e0a0 and its object at main+0x8adca10.
    { "b7f965b9d7227db8e29f04e84ea7331b", "Unreal rhi.SyncInterval", 0x8adca10 + 0x10, 0, VSYNCS_I32, 2 },
    // OCTOPATH TRAVELER II 1.1.1: Unreal Engine 4.27's t.MaxFPS and rhi.SyncInterval, the same layout (names in UTF-16). The game's Switch device profile sets t.MaxFPS=30 and
    // rhi.syncinterval=2 (Majesty/Config/Switch/SwitchDeviceProfiles.ini; its Windows, PS4 and PS5 profiles set t.MaxFPS=60). Registered at main+0x2e50444 (float, AsVariableFloat stored at
    // +0x10) and main+0x134d380 (int, AsVariableInt at +0x10)
    { "0d9649011312f62e93f6a5673a09d7e9", "Unreal t.MaxFPS", 0x76fef58 + 0x10, 0, FPS_F32, 2 },
    { "0d9649011312f62e93f6a5673a09d7e9", "Unreal rhi.SyncInterval", 0x75ad0d0 + 0x10, 0, VSYNCS_I32, 2 },
    // SHIN MEGAMI TENSEI V: Vengeance 1.0.3: its Switch profile sets Project.FrameRateLimit=30 and rhi.syncinterval=2. The project variable (registered at +0xdd8624)
    // feeds GameUserSettings.FrameRateLimit at +0xb4 (+0xdd7d24 / +0xdd7f94), which applies it to t.MaxFPS. Write this source value so settings changes keep the target.
    // rhi.SyncInterval is registered at +0x2aa0754. Both use the same TAutoConsoleVariable value pointer at +0x10 and consecutive game/render-thread copies.
    { "0a4f14a5c12f470a4d3168a8f2ae6005", "Unreal Project.FrameRateLimit", 0x6d6d5d0 + 0x10, 0, FPS_F32, 2 },
    { "0a4f14a5c12f470a4d3168a8f2ae6005", "Unreal rhi.SyncInterval", 0x796de10 + 0x10, 0, VSYNCS_I32, 2 },
    // Luigi's Mansion 3 1.4.0: Next Level Games' frame limiter. At the end of each frame the main thread calls the frame callbacks (main+0xb7c308) with the milliseconds since the frame
    // started; the limiter (main+0x7a3e70) runs jobs or sleeps until 33.333 ms have passed, the float at main+0x13c94dc (read only there, at main+0x7a3ea8 and main+0x7a3f18), and the game
    // steps by the time that really passed. The game asks for present interval 2 too, which the profile's present_interval display ignores; both are needed
    { "79e5950ffa85acf63e28c9aec051eac2", "NLG frame limiter target", 0, 0x13c94dc, MS_F32, 1 },
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

void dm_fps_lock_tick(void) {
    dynamic_resolution_tick();   // separate from the FPS cap
    static int state = -2, rows[NLOCKS], nrow, nlog[NLOCKS], target, nier; static uint8_t *base; static const char *src; static uint32_t display;
    if (state == -1) return;
    if (state == -2) {
        state = -1;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
            if (!strcmp(g_mod[i].build_id, "992787e2b54259948b844059a8ab21ad")) { base = g_mod[i].base; nier = 1; }
            for (int k = 0; k < (int)(sizeof k_nier_dr / sizeof *k_nier_dr); k++) if (!strcmp(g_mod[i].build_id, k_nier_dr[k].build)) { base = g_mod[i].base; nier = 2; display = k_nier_dr[k].display; }
            for (int k = 0; k < NLOCKS; k++) if (!strcmp(g_mod[i].build_id, k_locks[k].build)) { rows[nrow++] = k; base = g_mod[i].base; }
        }
        if (!nrow && !nier) return;
        const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET; src = e ? "DM_FPS_TARGET" : "game profile fps_target";
        if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; leaving the game's frame limit", e); t = 0; }
        if (!t) { for (int r = 0; r < nrow; r++) LOG("main: %s left as the game sets it (%s)", k_locks[rows[r]].what, src); if (nier) LOG("main: NieR frame interval left as the game sets it (%s)", src); return; }
        target = t; state = 0;
    }
    if (nier == 1) {
        // NieR:Automata (Switch) 1.0.2, matching the 60 FPS Atmosphere cheat
        // for build 992787E2B5425994. Its chain follows the frame interval field;
        // value 2 is the game's 30 FPS default and 1 selects 60 FPS.
        uint64_t p = 0, q = 0; static int nlog_nier;
        if (rd((uint64_t)(uintptr_t)(base + 0x192c818), &p, sizeof p) && p) {
            const uint64_t offsets[] = {0x40, 0x10, 0x60, 0x180};
            int ok = 1;
            for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; i++) {
                if (!rd(p + offsets[i], &q, sizeof q) || !q) { ok = 0; break; }
                p = q;
            }
            if (ok) {
                uint64_t a = p + 0xf14; uint32_t cur = 0, want = (uint32_t)(60 / target);
                if (rd(a, &cur, sizeof cur) && cur != want && (cur == 1 || cur == 2)) {
                    uint32_t *dst = (uint32_t *)(uintptr_t)a;
                    dm_guest_write_prepare(dst, sizeof *dst); __atomic_store_n(dst, want, __ATOMIC_RELAXED);
                    if (nlog_nier++ < 8) LOG("main: NieR frame interval %u → %u (%d fps; %s)", cur, want, target, src);
                }
            }
        }
    } else if (nier == 2) {
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
    for (int r = 0; r < nrow; r++) {
        int k = rows[r]; uint64_t obj = (uint64_t)(uintptr_t)base;
        if (k_locks[k].ptr_off && (!rd((uint64_t)(uintptr_t)(base + k_locks[k].ptr_off), &obj, sizeof obj) || !obj)) continue;   // not created yet
        for (int c = 0; c < k_locks[k].copies; c++) {
            uint64_t a = obj + k_locks[k].field + 4 * (uint64_t)c; uint32_t cur, want; char was[16];
            if (!rd(a, &cur, sizeof cur)) break;
            if (k_locks[k].kind == FPS_F32 || k_locks[k].kind == MS_F32) {
                int ms = k_locks[k].kind == MS_F32; float f, w = ms ? ms_want(target) : (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(ms ? f >= 8 && f <= 100 : f >= 10 && f <= 120)) continue;   // already set, or not set yet (RE Engine: 6000 at boot; Unreal: 0 = no limit until the config sets it)
                snprintf(was, sizeof was, ms ? "%.3f ms" : "%g", f);
            } else {
                int fps = k_locks[k].kind == FPS_I32;
                want = fps ? (uint32_t)target : 60 / (uint32_t)target;
                if (cur == want || cur < (fps ? 10u : 1u) || cur > (fps ? 120u : 4u)) continue;
                snprintf(was, sizeof was, "%u", cur);
            }
            uint32_t *p = (uint32_t *)(uintptr_t)a;
            dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog[k] < 8) { nlog[k]++; char now[24];
                if (k_locks[k].kind == MS_F32) snprintf(now, sizeof now, "%.3f ms (%d fps)", ms_want(target), target);
                else snprintf(now, sizeof now, "%u%s", k_locks[k].kind == FPS_F32 ? (unsigned)target : want, k_locks[k].kind == FPS_F32 || k_locks[k].kind == FPS_I32 ? " fps" : " vsyncs");
                LOG("main: %s%s %s → %s (%s)", k_locks[k].what, k_locks[k].copies > 1 ? (c ? " (render thread)" : " (game thread)") : "", was, now, src); }
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
