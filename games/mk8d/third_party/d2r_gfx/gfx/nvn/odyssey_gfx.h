#pragma once
/* Odyssey picture settings (user, 26 October): set in the settings window before the game (after PR #910 and #1076 keys
   3-7 are TOTK's: picture size, aspect ratio, MetalFX edge smoothing, controller type, mouse) and shown in the HUD
   (nvn_mtl.m includes this once). The list below is the order of the settings, no longer keys:
     3  anti-aliasing  off / FXAA / SMAA / TAA       at the game picture's size, before the upscale (TAA: not yet)
     4  sharpen (CAS)  off / on                      on the final picture, after the upscale
     5  upscaler       MetalFX / FSR 1 / Lanczos / bilinear   the game picture to the window
     6  textures       game / 4x / 8x / 16x          anisotropic filtering of the game's mipmapped samplers (the game's
                                                     texture sizes are fixed by its data; this is how sharp they look at an angle)
     7  resolution     1600x900 / 1920x1080 / 2240x1260   the game's render size, at the next launch (runtime/game_patches.c
                                                     patch_render_res plus the quality table; the launcher reads the file).
                                                     2400x1350 and 2560x1440 stop the game in its SDK (26 October): a limit
                                                     not yet found
   Keys 3-5 act while 60+60 is on (the user's choice); with it off the picture stays the default (MetalFX, no filter).
   ODYSSEY_GFX_ALWAYS=1 lifts that (measurements). ODYSSEY_GFX_SETTINGS=<file> keeps the choices across launches as
   "name=value" lines (aa, cas, upscaler, textures, resolution); ODYSSEY_AA / _CAS / _UPSCALER / _TEXTURES override them,
   ODYSSEY_POST=cas|fxaa|both is the older name. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { GX_AA, GX_CAS, GX_UP, GX_TEX, GX_RES, GX_N };
enum { GX_AA_OFF, GX_AA_FXAA, GX_AA_SMAA, GX_AA_TAA };
enum { GX_UP_METALFX, GX_UP_FSR1, GX_UP_LANCZOS, GX_UP_BILINEAR };
static const char *const gx_title[GX_N] = {"Anti-aliasing", "Sharpen (CAS)", "Upscaler", "Texture filtering", "Resolution"};
static const char *const gx_key[GX_N] = {"aa", "cas", "upscaler", "textures", "resolution"};
static const char *const gx_token[GX_N][4] = {
    {"off", "fxaa", "smaa", "taa"}, {"off", "on"}, {"metalfx", "fsr1", "lanczos", "bilinear"},
    {"game", "4x", "8x", "16x"}, {"1600x900", "1920x1080", "2240x1260"}};
static const char *const gx_label[GX_N][4] = {
    {"off", "FXAA", "SMAA", "TAA"}, {"off", "on"}, {"MetalFX", "FSR 1", "Lanczos", "bilinear"},
    {"game default", "4x anisotropic", "8x anisotropic", "16x anisotropic"}, {"1600x900", "1920x1080", "2240x1260"}};
static const int gx_ready[GX_N] = {3, 2, 4, 4, 3};   // values a key cycles through (TAA joins when it exists)
static _Atomic int g_gx[GX_N];
static int g_gx_res_now;                             // the resolution this launch runs at (TOTK_RENDER_RES, set by the launcher)
static _Atomic int g_samp_flush;                      // key 6: the sampler cache rebuilds with the new filtering
static pthread_mutex_t g_gx_file_mu = PTHREAD_MUTEX_INITIALIZER;

static int gx_find(int w, const char *v) {
    for (int i = 0; i < 4 && gx_token[w][i]; i++) if (!strcasecmp(v, gx_token[w][i])) return i;
    return -1;
}
static void gx_init(void) {
    const char *path = getenv("ODYSSEY_GFX_SETTINGS"), *rr = getenv("TOTK_RENDER_RES");
    int now = rr ? gx_find(GX_RES, rr) : 0; g_gx_res_now = now < 0 ? 0 : now; atomic_store(&g_gx[GX_RES], g_gx_res_now);
    FILE *f = path ? fopen(path, "r") : NULL;
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *v = eq + 1; v[strcspn(v, " \r\n")] = 0;
        for (int w = 0; w < GX_N; w++) if (!strcmp(line, gx_key[w])) { int i = gx_find(w, v); if (i >= 0 && i < gx_ready[w]) atomic_store(&g_gx[w], i); }
    }
    if (f) fclose(f);
    const char *post = getenv("ODYSSEY_POST");
    if (post && (!strcmp(post, "fxaa") || !strcmp(post, "both"))) atomic_store(&g_gx[GX_AA], GX_AA_FXAA);
    if (post && (!strcmp(post, "cas") || !strcmp(post, "both"))) atomic_store(&g_gx[GX_CAS], 1);
    static const char *const env[GX_N] = {"ODYSSEY_AA", "ODYSSEY_CAS", "ODYSSEY_UPSCALER", "ODYSSEY_TEXTURES", NULL};
    for (int w = 0; w < GX_N; w++) { const char *e = env[w] ? getenv(env[w]) : NULL; int i = e ? gx_find(w, e) : -1; if (i >= 0 && i < gx_ready[w]) atomic_store(&g_gx[w], i); }
}
static pthread_once_t g_gx_once = PTHREAD_ONCE_INIT;
static inline int gx_get(int w) { pthread_once(&g_gx_once, gx_init); return atomic_load_explicit(&g_gx[w], memory_order_relaxed); }
static int gx_live(void) {   // keys 3-5 apply: 60+60 on (or ODYSSEY_GFX_ALWAYS=1)
    static int always = -1; if (always < 0) { const char *e = getenv("ODYSSEY_GFX_ALWAYS"); always = e && *e == '1'; }
    extern int mfx_enabled(void);
    return always || mfx_enabled() != 0;
}
static inline int gx_eff(int w) { int v = gx_get(w); return (w == GX_AA || w == GX_CAS || w == GX_UP) && !gx_live() ? 0 : v; }
static int gx_aniso(void) { static const int a[4] = {0, 4, 8, 16}; return a[gx_get(GX_TEX) & 3]; }
static void gx_save(void) {
    const char *path = getenv("ODYSSEY_GFX_SETTINGS"); if (!path) return;
    pthread_mutex_lock(&g_gx_file_mu);
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char keep[2048] = ""; size_t kn = 0;   // the launcher's own lines (frame_generation, launcher) stay as they are
    FILE *in = fopen(path, "r"); char line[128];
    while (in && fgets(line, sizeof line, in)) {
        int ours = 0; for (int w = 0; w < GX_N; w++) { size_t l = strlen(gx_key[w]); if (!strncmp(line, gx_key[w], l) && line[l] == '=') ours = 1; }
        if (!ours && strchr(line, '=') && kn + strlen(line) + 2 < sizeof keep) { kn += (size_t)snprintf(keep + kn, sizeof keep - kn, "%s%s", line, line[strlen(line) - 1] == '\n' ? "" : "\n"); }
    }
    if (in) fclose(in);
    FILE *f = fopen(tmp, "w");
    if (f) {
        for (int w = 0; w < GX_N; w++) fprintf(f, "%s=%s\n", gx_key[w], gx_token[w][gx_get(w)]);
        fputs(keep, f);
        if (!fclose(f)) rename(tmp, path);
    }
    pthread_mutex_unlock(&g_gx_file_mu);
}
void dm_gfx_opt_set(int w, int v) {
    if (w < 0 || w >= GX_N) return;
    gx_get(w);
    if (v < 0) v = (gx_get(w) + 1) % gx_ready[w];
    if (v >= gx_ready[w]) return;
    atomic_store(&g_gx[w], v);
    if (w == GX_TEX) atomic_store(&g_samp_flush, 1);
    gx_save();
    fprintf(stderr, "[dm] [nvn-mtl] [gfx] %s: %s%s%s\n", gx_title[w], gx_label[w][v],
            w == GX_RES && v != g_gx_res_now ? " (next launch)" : "", w <= GX_UP && !gx_live() ? " (applies while 60+60 is on)" : "");
}
int dm_gfx_opt_cycle(int w) { dm_gfx_opt_set(w, -1); return w >= 0 && w < GX_N ? gx_get(w) : -1; }
int dm_gfx_opt_get(int w) { return w >= 0 && w < GX_N ? gx_get(w) : -1; }
