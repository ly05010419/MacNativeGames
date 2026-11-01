#pragma once
/* Odyssey's live picture settings, shared by the prelaunch window and in-game settings panel.
   ODYSSEY_GFX_SETTINGS stores aa/cas/upscaler/textures/resolution/frame_generation. Render size changes stay within
   the maximum allocated at launch; the game logic remains 60 Hz. Picture filters work with interpolation on or off.
   ODYSSEY_AA / _CAS / _UPSCALER / _TEXTURES and the older ODYSSEY_POST override saved picture choices. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "odyssey_graphics.h"
static const char *const gx_title[GX_N] = {"Anti-aliasing", "Sharpen (CAS)", "Upscaler", "Texture filtering", "Resolution", "Frame generation"};
static const char *const gx_key[GX_N] = {"aa", "cas", "upscaler", "textures", "resolution", "frame_generation"};
static const char *const gx_token[GX_N][4] = {
    {"off", "fxaa", "smaa", "taa"}, {"off", "on"}, {"metalfx", "fsr1", "lanczos", "bilinear"},
    {"game", "4x", "8x", "16x"}, {"1600x900", "1920x1080", "2240x1260", "2560x1440"}, {"off", "on"}};
static const char *const gx_label[GX_N][4] = {
    {"off", "FXAA", "SMAA", "TAA"}, {"off", "on"}, {"MetalFX", "FSR 1", "Lanczos", "bilinear"},
    {"game default", "4x anisotropic", "8x anisotropic", "16x anisotropic"}, {"1600x900", "1920x1080", "2240x1260", "2560x1440"}, {"off", "on"}};
static const int gx_ready[GX_N] = {3, 2, 4, 4, 4, 2};   // supported choices (TAA joins when it exists)
static _Atomic int g_gx[GX_N];
static _Atomic int g_samp_flush;                      // rebuild the sampler cache with the new filtering
static pthread_mutex_t g_gx_file_mu = PTHREAD_MUTEX_INITIALIZER;

static int gx_find(int w, const char *v) {
    for (int i = 0; i < 4 && gx_token[w][i]; i++) if (!strcasecmp(v, gx_token[w][i])) return i;
    return -1;
}
static void gx_init(void) {
    const char *path = getenv("ODYSSEY_GFX_SETTINGS");
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
    static const char *const env[GX_N] = {"ODYSSEY_AA", "ODYSSEY_CAS", "ODYSSEY_UPSCALER", "ODYSSEY_TEXTURES", NULL, NULL};
    for (int w = 0; w < GX_N; w++) { const char *e = env[w] ? getenv(env[w]) : NULL; int i = e ? gx_find(w, e) : -1; if (i >= 0 && i < gx_ready[w]) atomic_store(&g_gx[w], i); }
}
static pthread_once_t g_gx_once = PTHREAD_ONCE_INIT;
static inline int gx_get(int w) { pthread_once(&g_gx_once, gx_init); return atomic_load_explicit(&g_gx[w], memory_order_relaxed); }
/* Picture choices apply to ordinary 60 FPS play as well as generated frames. Defaults retain the plain picture. */
static int gx_live(void) { return 1; }
static inline int gx_eff(int w) { return gx_get(w); }
static int gx_aniso(void) { static const int a[4] = {0, 4, 8, 16}; return a[gx_get(GX_TEX) & 3]; }
static void gx_save(void) {
    const char *path = getenv("ODYSSEY_GFX_SETTINGS"); if (!path) return;
    pthread_mutex_lock(&g_gx_file_mu);
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char keep[2048] = ""; size_t kn = 0;   // preserve launcher and other unknown lines
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
    if (w == GX_RES) {
        extern int runtime_scene_resolution_set(unsigned, unsigned);
        unsigned width, height;
        if (sscanf(gx_token[w][v], "%ux%u", &width, &height) != 2 || !runtime_scene_resolution_set(width, height)) return;
    }
    atomic_store(&g_gx[w], v);
    if (w == GX_FRAME) mfx_set(v ? MFX_HALF : MFX_OFF);
    if (w == GX_TEX) atomic_store(&g_samp_flush, 1);
    gx_save();
    fprintf(stderr, "[dm] [nvn-mtl] [gfx] %s: %s%s%s\n", gx_title[w], gx_label[w][v],
            "", "");
}
int dm_gfx_opt_cycle(int w) { dm_gfx_opt_set(w, -1); return w >= 0 && w < GX_N ? gx_get(w) : -1; }
int dm_gfx_opt_get(int w) { return w >= 0 && w < GX_N ? gx_get(w) : -1; }
