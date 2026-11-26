#include "game_patches.h"
#include "aspect_math.h"
#include <libkern/OSCacheControl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bayonetta 3: the player's 1.2.0 exefs patches (60 fps, 900p, LOD, film grain), applied to main after relocation and
   before its text is sealed. scripts/prepare_patches.py turns each .pchtxt into patches/<slug>.tsv ("offset new orig",
   hex), already checked against the original image and the rewrite; here every site is checked again against the loaded
   bytes, and a mismatch stops the start (a wrong version or a rewritten word must never be patched).
   BY3_PATCHES=<slug>,<slug>,... picks them (the launcher sets it from the settings; unset or empty: none). */
static int hexbytes(const char *s, uint8_t *out, size_t cap, size_t *n) {
    size_t k = 0;
    for (; s[0] && s[1] && s[0] != '\t' && s[0] != '\n'; s += 2) {
        unsigned v; if (k == cap || sscanf(s, "%2x", &v) != 1) return 0;
        out[k++] = (uint8_t)v;
    }
    *n = k; return k > 0;
}
static void apply_file(Module *m, const char *slug) {
    char path[512]; snprintf(path, sizeof path, "patches/%s.tsv", slug);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[by3] patch %s: %s missing (scripts/prepare_patches.py)\n", slug, path); runtime_fail("game patch table missing"); }
    char line[4096]; unsigned applied = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        uint64_t off = strtoull(line, NULL, 0); char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        uint8_t now[1024], was[1024]; size_t nn, nw;
        if (!a || !b || !hexbytes(a + 1, now, sizeof now, &nn) || !hexbytes(b + 1, was, sizeof was, &nw) || nn != nw)
            runtime_fail("malformed game patch table");
        if (off + nn > m->text_end && off < m->ro_off) runtime_fail("game patch in the rewrite area");
        if (off + nn > m->file_size) runtime_fail("game patch beyond main");
        if (memcmp(m->base + off, was, nw)) { fprintf(stderr, "[by3] patch %s at main+%#llx: original bytes differ\n", slug, (unsigned long long)off); runtime_fail("game patch does not match this game version"); }
        memcpy(m->base + off, now, nn); sys_icache_invalidate(m->base + off, nn); applied++;
    }
    fclose(f);
    fprintf(stderr, "[by3] game patch %s: %u sites\n", slug, applied);
}
/* MCD v1769472: bypass UE4 Switch RHI guest frame pacer (FFramePacer / RHIFrameFlipThread / RHIFrameOffsetThread)
   and guest MaxTickRate throttle so the host NVN display link and pacer (DM_PRESENT_HZ / Key 8: 30/60/90/120/unlimited)
   pace frames directly without double-sleeping on 60 Hz guest timers. */
static void mcd_patch_word(Module *m, size_t off, uint32_t expected, uint32_t replacement, const char *label) {
    uint32_t actual = 0;
    if (off + sizeof actual > m->file_size || off + sizeof actual > m->text_end)
        runtime_fail("MCD patch outside guest text");
    memcpy(&actual, m->base + off, sizeof actual);
    if (actual != expected) {
        fprintf(stderr, "[mcd] patch %s at main+%#zx mismatch: got %#x expected %#x\n", label, off, actual, expected);
        runtime_fail("MCD patch version mismatch");
    }
    memcpy(m->base + off, &replacement, sizeof replacement);
    sys_icache_invalidate(m->base + off, sizeof replacement);
}
static void mcd_unlock_frame_pacer(Module *m) {
    const char *e = getenv("MCD_GUEST_PACER");
    if (e && !strcmp(e, "1")) return;
    mcd_patch_word(m, 0x01e75e20, 0x320003e2u, 0x52800002u, "rhi.SyncInterval default");
    mcd_patch_word(m, 0x01e825ec, 0xb8687af7u, 0x52800017u, "RHIFrameFlipThread SyncInterval");
    mcd_patch_word(m, 0x01e82800, 0xf81e0ff3u, 0x52800000u, "RHIGetSyncInterval mov0");
    mcd_patch_word(m, 0x01e82804, 0xa9017bfdu, 0xd65f03c0u, "RHIGetSyncInterval ret");
    mcd_patch_word(m, 0x01e82d70, 0xa9be4ff4u, 0xd65f03c0u, "FFramePacer::Init ret");
    mcd_patch_word(m, 0x0316cbcc, 0x4ea81d00u, 0x1e2703e0u, "UEngine::GetMaxTickRate zero");
    fprintf(stderr, "[mcd] guest frame pacer unlocked (host NVN pacer controls frame rate)\n");
}
void runtime_game_patches(Module *main_module) {
    mcd_unlock_frame_pacer(main_module);
    const char *list = getenv("BY3_PATCHES");
    if (!list || !*list) { fprintf(stderr, "[by3] game patches: none (BY3_PATCHES)\n"); return; }
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *s = strtok_r(buf, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
        for (const char *c = s; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) runtime_fail("BY3_PATCHES: bad patch name");
        apply_file(main_module, s);
    }
}

/* Key 3 (MetalFX 2K on/off) and Key 4 (16:9 -> fit to window -> 21:9 -> 16:9). */
static _Atomic(uint32_t) g_aspect_val;
static float g_aspect_wide = (float)ASPECT_WIDE;
static atomic_int g_aspect_step;
static _Atomic(uint32_t) g_aspect_view;

static inline float bits_to_float(uint32_t bits) { float v; memcpy(&v, &bits, 4); return v; }
static inline uint32_t float_to_bits(float v) { uint32_t bits; memcpy(&bits, &v, 4); return bits; }

static void d3_aspect_apply(void) {
    double want = aspect_step_ratio(atomic_load(&g_aspect_step), bits_to_float(atomic_load(&g_aspect_view)), g_aspect_wide);
    float a = aspect_effective(want);
    atomic_store(&g_aspect_val, float_to_bits(a));
}

void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) {
    extern int mfxup_output_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    int next = !mfxup_output_enabled();
    dm_gfx_set_flag("mfx_output", next);
    dm_gfx_set_flag("mfx_upscale", next);
    fprintf(stderr, "[d3] key 3: %s\n", next ? "MetalFX 2K" : "off");
    return 1;
}
float runtime_aspect_now(void) { return bits_to_float(atomic_load(&g_aspect_val)); }
int runtime_aspect_step(void) { return atomic_load(&g_aspect_step); }
void runtime_aspect_view(double w, double h) {
    if (!(w > 0 && h > 0)) return;
    atomic_store(&g_aspect_view, float_to_bits((float)(w / h)));
    if (atomic_load(&g_aspect_step) == ASPECT_STEP_FIT) d3_aspect_apply();
}
int runtime_aspect_toggle(void) {
    int step = aspect_next_step(atomic_load(&g_aspect_step), bits_to_float(atomic_load(&g_aspect_view)), g_aspect_wide);
    atomic_store(&g_aspect_step, step);
    d3_aspect_apply();
    fprintf(stderr, "[d3] key 4: step %d aspect %.4f\n", step, runtime_aspect_now());
    return 1;
}
