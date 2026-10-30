#include "game_patches.h"
#include "game_version.h"
#include <libkern/OSCacheControl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The player's exefs patches for this game version, applied to main after relocation and before its text is sealed. scripts/prepare_patches.py turns each .pchtxt into patches/<version>/<slug>.tsv ("offset new orig",
   hex), already checked against the original image and the rewrite; here every site is checked again against the loaded
   bytes, and a mismatch stops the start (a wrong version or a rewritten word must never be patched).
   XC3_PATCHES=<slug>,<slug>,... picks them (the launcher sets it from the settings; unset or empty: none). */
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
    if (!f) { fprintf(stderr, "[xc3] patch %s: %s missing (scripts/prepare_patches.py)\n", slug, path); runtime_fail("game patch table missing"); }
    char line[4096]; unsigned applied = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        uint64_t off = strtoull(line, NULL, 0); char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        uint8_t now[1024], was[1024]; size_t nn, nw;
        if (!a || !b || !hexbytes(a + 1, now, sizeof now, &nn) || !hexbytes(b + 1, was, sizeof was, &nw) || nn != nw)
            runtime_fail("malformed game patch table");
        if (off + nn > m->text_end && off < m->ro_off) runtime_fail("game patch in the rewrite area");
        if (off + nn > m->file_size) runtime_fail("game patch beyond main");
        if (memcmp(m->base + off, was, nw)) { fprintf(stderr, "[xc3] patch %s at main+%#llx: original bytes differ\n", slug, (unsigned long long)off); runtime_fail("game patch does not match this game version"); }
        memcpy(m->base + off, now, nn); sys_icache_invalidate(m->base + off, nn); applied++;
    }
    fclose(f);
    fprintf(stderr, "[xc3] game patch %s: %u sites\n", slug, applied);
}
/* Render scale (stage 13; 2.2.1 only, sites in game_version.h): "original" keeps the game's scene at half the output
   size (960x544 of 1920x1080), "full" draws it at the output size, "2k" at 1.328 times it (2552x1440; the game scales it
   down to its 1920x1080 output). XC3_RENDER_SCALE=original|full|2k decides first, then render-scale.txt next to
   controls.txt (the settings panel writes it); the default is original. Read once at start, since the game sets its
   scale only when its renderer starts. Above 1.33 the GPU fails (2.0, 3840x2160: Metal stops taking the game's work). */
static const struct { const char *word; uint16_t upper; } render_scales[] = {{"original", 0}, {"full", 0x3f80}, {"2k", 0x3faa}};
static int render_scale_choice = -1;   /* -1 not read yet, else an index of render_scales */
int runtime_render_scale_offered(void) {
#ifdef XC3_RENDER_SCALE_SITES
    return 1;
#else
    return 0;
#endif
}
int runtime_render_scale_saved(void) {
    char word[32] = {0}; const char *e = getenv("XC3_RENDER_SCALE");
    if (e && *e) snprintf(word, sizeof word, "%s", e);
    else if (!runtime_setting_word("render-scale.txt", word, sizeof word)) return 0;
    word[strcspn(word, " \t\r\n")] = 0;
    for (int i = 0; i < (int)(sizeof render_scales / sizeof *render_scales); i++) if (!strcmp(word, render_scales[i].word)) return i;
    fprintf(stderr, "[xc3] render scale '%s' not known; original kept\n", word);
    return 0;
}
void runtime_render_scale_save(int choice) {
    if (choice >= 0 && choice < (int)(sizeof render_scales / sizeof *render_scales))
        runtime_setting_save("render-scale.txt", render_scales[choice].word, "render resolution");
}
int runtime_render_scale(void) { return render_scale_choice > 0 ? render_scale_choice : 0; }
static void render_scale_apply(Module *m) {
    render_scale_choice = runtime_render_scale_offered() ? runtime_render_scale_saved() : 0;
    if (!render_scale_choice) { fprintf(stderr, "[xc3] render scale: original\n"); return; }
#ifndef XC3_RENDER_SCALE_SITES
    (void)m;
#else
    static const struct { uint64_t off; uint32_t was, upper; } sites[] = XC3_RENDER_SCALE_SITES;
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        uint32_t w; memcpy(&w, m->base + sites[i].off, 4);
        if (sites[i].off + 4 > m->text_end || w != sites[i].was) {
            fprintf(stderr, "[xc3] render scale at main+%#llx: %08x, not %08x\n", (unsigned long long)sites[i].off, w, sites[i].was);
            runtime_fail("render scale patch does not match this game version");
        }
    }
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        uint32_t imm = sites[i].upper ? render_scales[render_scale_choice].upper : 0;
        uint32_t now = (sites[i].was & ~(0xffffu << 5)) | imm << 5;   /* mov/movk: imm16 in bits 5-20 */
        memcpy(m->base + sites[i].off, &now, 4); sys_icache_invalidate(m->base + sites[i].off, 4);
    }
    fprintf(stderr, "[xc3] render scale: %s (%u sites)\n", render_scales[render_scale_choice].word, (unsigned)(sizeof sites / sizeof *sites));
#endif
}

void runtime_game_patches(Module *main_module) {
    render_scale_apply(main_module);
    const char *list = getenv("XC3_PATCHES");
    if (!list || !*list) { fprintf(stderr, "[xc3] game patches: none (XC3_PATCHES)\n"); return; }
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *s = strtok_r(buf, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
        for (const char *c = s; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) runtime_fail("XC3_PATCHES: bad patch name");
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
