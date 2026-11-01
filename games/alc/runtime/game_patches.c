#include "game_patches.h"
#include <libkern/OSCacheControl.h>
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Astral Chain: the player's 1.0.1 exefs patches (60 fps, 900p, LOD, film grain), applied to main after relocation and
   before its text is sealed. scripts/prepare_patches.py turns each .pchtxt into patches/<slug>.tsv ("offset new orig",
   hex), already checked against the original image and the rewrite; here every site is checked again against the loaded
   bytes, and a mismatch stops the start (a wrong version or a rewritten word must never be patched).
   ALC_PATCHES=<slug>,<slug>,... picks them (the launcher sets it from the settings; unset or empty: none). */
/* The logic-step floor of the 60 FPS patch adapts to the actual present pace. The floor exists so the logic
   advances a whole 30-fps step when a frame's work is less than one: at a 60 fps pace that is 0.5 (half a step
   per 16.7 ms frame — the whole point of the patch), but the pace can legitimately be 30 fps — the interp "30+30"
   mode paces presents at exactly 1/30 s, and an interval of 2 does the same without the patch — while a frame's
   measured work stays ~17 ms (present waits are not counted). A constant 0.5 floor then halves the game speed
   (the "slow motion" a player gets when dropping to 30). The graphics layer reports each present's gap; the floor
   is rewritten in place (the loader's text is RX, so it takes an mprotect round trip) to gap/16.7*0.5, quantized
   to a quarter step. At every pace this equals the stock behaviour: 0.5 at 60, 1.0 at 30, 1.5 at 20. */
static const struct { float floor; uint32_t fmov, movw; } STEP_FLOORS[] = {
    {0.50f, 0x1E2C1001u, 0x52A7E008u},   // fmov s1, #0.5        | mov w8, #0x3f000000
    {0.75f, 0x1E2D1001u, 0x52A7E808u},   // fmov s1, #0.75       | mov w8, #0x3f400000
    {1.00f, 0x1E2E1001u, 0x52A7F008u},   // fmov s1, #1.0        | mov w8, #0x3f800000
    {1.25f, 0x1E2E9001u, 0x52A7F408u},   // fmov s1, #1.25       | mov w8, #0x3fa00000
    {1.50f, 0x1E2F1001u, 0x52A7F808u},   // fmov s1, #1.5        | mov w8, #0x3fc00000
    {1.75f, 0x1E2F9001u, 0x52A7FC08u},   // fmov s1, #1.75       | mov w8, #0x3fe00000
    {2.00f, 0x1E201001u, 0x52A80008u},   // fmov s1, #2.0        | mov w8, #0x40000000
};
#define STEP_FLOOR_SITES 3
static const uint32_t STEP_FLOOR_AT[STEP_FLOOR_SITES] = {0x91dc94, 0x91dca0, 0x91dcac};
static const uint32_t STEP_FLOOR_WAS[STEP_FLOOR_SITES] = {0x1e2c1001u, 0x1e2c1001u, 0x52a7e008u};   // the patch's 0.5
static int step_floor_live;              // the 60 fps patch applied and the sites still read the patch words
static float step_floor_now = 0.5f;
static Module *patched_main;
static int floor_index(float floor) {
    for (int i = 0; i < (int)(sizeof STEP_FLOORS / sizeof *STEP_FLOORS); i++) if (STEP_FLOORS[i].floor == floor) return i;
    return -1;
}
static void set_step_floor(float floor) {
    if (!patched_main || !step_floor_live) return;
    int best = floor_index(floor), ci = floor_index(step_floor_now);
    if (best < 0 || best == ci) return;
    uintptr_t lo = (uintptr_t)patched_main->base + (STEP_FLOOR_AT[0] & ~0xfffull);
    for (int i = 0; i < STEP_FLOOR_SITES; i++) {
        uint32_t w = *(uint32_t *)((uintptr_t)patched_main->base + STEP_FLOOR_AT[i]);
        uint32_t cur = i == 2 ? STEP_FLOORS[ci].movw : STEP_FLOORS[ci].fmov;
        uint32_t neww = i == 2 ? STEP_FLOORS[best].movw : STEP_FLOORS[best].fmov;
        if (w != cur && w != neww && w != STEP_FLOOR_WAS[i])
            { fprintf(stderr, "[alc] step floor main+%#x reads %08x, not a known floor word; leaving it\n", STEP_FLOOR_AT[i], w); return; }
    }
    /* mprotect reports EINVAL across the loader's sub-mappings; the Mach call handles the same range fine. Keep EXECUTE:
       the game's own timer thread runs inside this page every frame, and a page without execute faults on its next
       instruction fetch (a real crash seen as an instruction abort at main+0x91dbb0). The stores are aligned 32-bit
       writes, so a thread running between them only ever sees one of the two floors, never a torn instruction. */
    if (mach_vm_protect(mach_task_self(), (mach_vm_address_t)lo, 0x2000, FALSE, VM_PROT_READ | VM_PROT_WRITE | 0x04)) {
        fprintf(stderr, "[alc] step floor: raising the text to RW failed\n");
        return;
    }
    for (int i = 0; i < STEP_FLOOR_SITES; i++) {
        uint32_t w = i == 2 ? STEP_FLOORS[best].movw : STEP_FLOORS[best].fmov;
        *(uint32_t *)((uintptr_t)patched_main->base + STEP_FLOOR_AT[i]) = w;
        sys_icache_invalidate((void *)((uintptr_t)patched_main->base + STEP_FLOOR_AT[i]), 4);
    }
    mach_vm_protect(mach_task_self(), (mach_vm_address_t)lo, 0x2000, FALSE, VM_PROT_READ | 0x04);   // VM_PROT_EXEC
    step_floor_now = STEP_FLOORS[best].floor;
    fprintf(stderr, "[alc] step floor -> %.2f (present pace changed; logic speed follows the pace, not the work time)\n", step_floor_now);
}
void runtime_game_patches_note_present_gap(float ms) {
    if (!step_floor_live) return;
    static float hist[32]; static int n, at;   // the pace is the median gap: one loading hitch must not move it
    if (ms > 60.0f) return;                    // loading frames are not a pace
    hist[at % 32] = ms; at++; if (n < 32) n++;
    if (n < 12) return;
    float v[32];
    for (int i = 0; i < n; i++) v[i] = hist[(at - n + i) % 32];
    for (int i = 1; i < n; i++) { float x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    float med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    float floor = med / 16.667f * 0.5f;
    floor = floor < 0.5f ? 0.5f : floor > 2.0f ? 2.0f : floor;
    float q = roundf(floor * 4.0f) / 4.0f;
    if (fabsf(q - step_floor_now) >= 0.125f) set_step_floor(q);
}

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
    if (!f) { fprintf(stderr, "[alc] patch %s: %s missing (scripts/prepare_patches.py)\n", slug, path); runtime_fail("game patch table missing"); }
    char line[4096]; unsigned applied = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        uint64_t off = strtoull(line, NULL, 0); char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        uint8_t now[1024], was[1024]; size_t nn, nw;
        if (!a || !b || !hexbytes(a + 1, now, sizeof now, &nn) || !hexbytes(b + 1, was, sizeof was, &nw) || nn != nw)
            runtime_fail("malformed game patch table");
        /* Patches live in .text, or entirely inside the assembler padding between .text and .rodata —
           original dead bytes, mapped r-x with the text (the 60 FPS whole-tick cave lives there). */
        if (off + nn > m->text_end && !(off >= m->text_end && off + nn <= m->ro_off))
            runtime_fail("game patch in the rewrite area");
        if (off + nn > m->file_size) runtime_fail("game patch beyond main");
        if (memcmp(m->base + off, was, nw)) { fprintf(stderr, "[alc] patch %s at main+%#llx: original bytes differ\n", slug, (unsigned long long)off); runtime_fail("game patch does not match this game version"); }
        memcpy(m->base + off, now, nn); sys_icache_invalidate(m->base + off, nn); applied++;
    }
    fclose(f);
    fprintf(stderr, "[alc] game patch %s: %u sites\n", slug, applied);
}
void runtime_game_patches(Module *main_module) {
    patched_main = main_module;
    const char *list = getenv("ALC_PATCHES");
    if (!list || !*list) { fprintf(stderr, "[alc] game patches: none (ALC_PATCHES)\n"); return; }
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *s = strtok_r(buf, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
        for (const char *c = s; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) runtime_fail("ALC_PATCHES: bad patch name");
        apply_file(main_module, s);
    }
    /* the step-floor sites exist only with the 60 fps patch: all three still read the patch words? */
    step_floor_live = 1;
    for (int i = 0; i < STEP_FLOOR_SITES; i++)
        if (*(uint32_t *)((uintptr_t)main_module->base + STEP_FLOOR_AT[i]) != STEP_FLOOR_WAS[i]) step_floor_live = 0;
    if (step_floor_live) fprintf(stderr, "[alc] adaptive step floor armed (0.5 at a 60 fps pace, 1.0 at 30, 1.5 at 20)\n");
}

/* The TotK tree's settings panel and its picture controls: Astral Chain offers the MetalFX output upscale (key 3) and
   the HUD, not TotK's aspect / output-mode / render-resolution modes, so those answer "not offered" here. The panel's
   rows are built from what exists (the launcher's presets remain the place for resolution). */
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_output_mode(void) { return 0; }   /* OUTPUT_MODE_OFF: the launcher owns resolution */
int runtime_output_modes(void) { return 0; }
int runtime_output_mode_set(int mode) { (void)mode; return 0; }
int runtime_output_native(int mode, unsigned *w, unsigned *h) { (void)mode; (void)w; (void)h; return 0; }
int runtime_aspect_set_ratio(float r) { (void)r; return 0; }
float runtime_aspect_ratio(void) { return 0; }
float runtime_aspect_start(void) { return 0; }
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
void runtime_audio_hold(int on) { (void)on; }
void runtime_audio_output_restart(int recreate) { (void)recreate; }
int runtime_phone_fps(void) { return 60; }
void runtime_phone_fps_set(int fps) { (void)fps; }

/* The TotK host hooks the shared graphics layer and the input keys call keep their names and answer "off". */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
/* Key 3 cycles the MetalFX output upscale (the present stage that sharpens into windows and full-screen draws larger
   than the game's 1600x900 canvas). Astral Chain has no TotK 2K modes behind it; in a 1:1 window both states look the
   same, the switch shows once the window or the full-screen draw exceeds the canvas. */
extern int mfxup_enabled(void);
extern void mfxup_set(int on);
extern int mfxup_output_enabled(void);
extern void mfxup_output_set(int on);
int runtime_output_mode_cycle(void) {
    /* The scaler has a master switch (TOTK_MFX_UPSCALE) AND an output-stage switch; without the master the key
       would flip the second one against a closed first and nothing would change. Flip both. */
    int on = !mfxup_output_enabled();
    mfxup_set(on);
    mfxup_output_set(on);
    fprintf(stderr, "[alc] key 3: MetalFX output upscale %s\n", on ? "on" : "off");
    return 1;
}
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
