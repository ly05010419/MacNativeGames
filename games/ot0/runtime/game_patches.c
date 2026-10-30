#include "game_patches.h"
#include "game_version.h"
#include <libkern/OSCacheControl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bayonetta 3: the player's 1.2.0 exefs patches (60 fps, 900p, LOD, film grain), applied to main after relocation and
   before its text is sealed. scripts/prepare_patches.py turns each .pchtxt into patches/<version>/<slug>.tsv ("offset new orig",
   hex), already checked against the original image and the rewrite; here every site is checked again against the loaded
   bytes, and a mismatch stops the start (a wrong version or a rewritten word must never be patched).
   OT0_PATCHES=<slug>,<slug>,... picks them (the launcher sets it from the settings; unset or empty: none). */
static int hexbytes(const char *s, uint8_t *out, size_t cap, size_t *n) {
    size_t k = 0;
    for (; s[0] && s[1] && s[0] != '\t' && s[0] != '\n'; s += 2) {
        unsigned v; if (k == cap || sscanf(s, "%2x", &v) != 1) return 0;
        out[k++] = (uint8_t)v;
    }
    *n = k; return k > 0;
}
static void apply_file(Module *m, const char *slug) {
    char path[512]; snprintf(path, sizeof path, "patches/" TOTK_GAME_DIR "/%s.tsv", slug);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[ot0] patch %s: %s missing (scripts/prepare_patches.py)\n", slug, path); runtime_fail("game patch table missing"); }
    char line[4096]; unsigned applied = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        uint64_t off = strtoull(line, NULL, 0); char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        uint8_t now[1024], was[1024]; size_t nn, nw;
        if (!a || !b || !hexbytes(a + 1, now, sizeof now, &nn) || !hexbytes(b + 1, was, sizeof was, &nw) || nn != nw)
            runtime_fail("malformed game patch table");
        if (off + nn > m->text_end && off < m->ro_off) runtime_fail("game patch in the rewrite area");
        if (off + nn > m->file_size) runtime_fail("game patch beyond main");
        if (memcmp(m->base + off, was, nw)) { fprintf(stderr, "[ot0] patch %s at main+%#llx: original bytes differ\n", slug, (unsigned long long)off); runtime_fail("game patch does not match this game version"); }
        memcpy(m->base + off, now, nn); sys_icache_invalidate(m->base + off, nn); applied++;
    }
    fclose(f);
    fprintf(stderr, "[ot0] game patch %s: %u sites\n", slug, applied);
}
static void max_fps_patch(Module *m) {
    const char *e = getenv("OT0_MAX_FPS");
    int fps = e && *e ? atoi(e) : 30;
    if (fps < 30 || fps > 240) runtime_fail("OT0_MAX_FPS must be 30 to 240");
    if (fps == 30) { fprintf(stderr, "[ot0] frame rate cap: the game's own (30)\n"); return; }
    const uint64_t off = GAME_MAX_TICK_RATE;
    const uint32_t was[3] = {0xa9bf7bfd, 0x910003fd, 0x941b35ae};
    union { float f; uint32_t u; } v = {.f = (float)fps};
    const uint32_t now[3] = {0x52a00008 | (v.u >> 16) << 5, 0x1e270100, 0xd65f03c0};
    if (memcmp(m->base + off, was, sizeof was)) runtime_fail("frame rate patch does not match this game version");
    memcpy(m->base + off, now, sizeof now); sys_icache_invalidate(m->base + off, sizeof now);
    fprintf(stderr, "[ot0] frame rate cap: %d (UGameEngine::GetMaxTickRate at main+%#llx)\n", fps, (unsigned long long)off);
}
void runtime_game_patches(Module *main_module) {
    max_fps_patch(main_module);
    const char *list = getenv("OT0_PATCHES");
    if (!list || !*list) { fprintf(stderr, "[ot0] game patches: none (OT0_PATCHES)\n"); return; }
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *s = strtok_r(buf, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
        for (const char *c = s; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) runtime_fail("OT0_PATCHES: bad patch name");
        apply_file(main_module, s);
    }
}

/* The TotK host hooks the shared graphics layer and the input keys call keep their names and answer "off". */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
