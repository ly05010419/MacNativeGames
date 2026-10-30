#include "game_patches.h"
#include "game_version.h"
#include "totk_ui_lang.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Odyssey 1.3.0 / 1.4.1 (the same executables): the render size patches below (key 3). Each site is checked against the
   original instruction before it is written, as CONTRIBUTING.md asks. Key 4 writes camera fields at the present instead
   (odyssey_aspect.c). */
void odyssey_aspect_install(const Module *main_module);
void odyssey_aspect_present(void);

/* Docked render size (TOTK_RENDER_RES=<w>x<h>; unset: render-res.txt of the overlay folder, else 1600x900 as shipped).
   Three places hold the docked size (disassembly 2026-10-22): main+0x4bf2a4 puts 1600 | 900 << 32 into the configuration
   of the object made at main+0x624460, from which the window and its three 1600x900 swapchain textures take their size
   (main+0x4bf32c -> main+0x94175c -> nvnTextureBuilderSetSize2D); main+0x4bf34c passes 1600 x 900 and 1280 x 720 to
   main+0x624ad0; main+0x4e27bc gives the window crop (main+0x94dfe0: 0, 0, w, h) 1600 x 900 when docked, 1280 x 720
   otherwise. Patching the last two alone laid the UI out for 1920x1080 on 1600x900 textures: the right of the picture was
   cut off. All docked values are replaced; each instruction is checked before any is written. */
static unsigned g_render_w, g_render_h;
/* Without TOTK_RENDER_RES the size comes from the overlay folder: scripts/make_render_res_overlay.py writes render-res.txt
   next to the scaled ProjectGraphicsQuality.szs, so the code sizes below and the data table always agree (the app carries
   both in its romfs-patches folder, TOTK_ROMFS_PATCHES; 0: neither). */
static const char *render_res_from_overlay(char *buf, size_t n, const char **from) {
    const char *dir = getenv("TOTK_ROMFS_PATCHES");
    if (dir && !strcmp(dir, "0")) return NULL;
    static char path[4096];
    snprintf(path, sizeof path, "%s/render-res.txt", dir && *dir ? dir : "extracted/" TOTK_GAME_DIR "/romfs-patches");
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    char *r = fgets(buf, (int)n, f); fclose(f);
    if (!r) return NULL;
    buf[strcspn(buf, "\r\n")] = 0; *from = path;
    return buf;
}
/* The docked render size asked for, read once (0 = as shipped): patch_render_res and the memory sizes below use it, and
   services.c asks for the memory before the main module is patched. */
static int render_size(unsigned *w, unsigned *h, const char **from) {
    static int done; static unsigned sw, sh; static const char *src = "TOTK_RENDER_RES";
    if (!done) {
        done = 1;
        const char *e = getenv("TOTK_RENDER_RES"); static char buf[32];
        if (!e) e = render_res_from_overlay(buf, sizeof buf, &src);
        if (e && *e && strcmp(e, "0") && strcmp(e, "1600x900")) {
            char tail;
            if (sscanf(e, "%ux%u%c", &sw, &sh, &tail) != 2 || sw < 640 || sh < 360 || sw > 3840 || sh > 2160)
                runtime_fail("TOTK_RENDER_RES must be <w>x<h> between 640x360 and 3840x2160");
        }
    }
    *w = sw; *h = sh; if (from) *from = src;
    return sw != 0;
}
/* Render targets come from a 100 MiB compressible pool (main+0x624b7c, inside a 170 MiB heap made at main+0x624b4c, both
   handed to main+0x983ab0). Its allocator (main+0x9a5440) returns NULL when no free block fits, and the frame buffer that
   wanted it then crashes reading +0x128 (main+0x9917f8). Measured on the title: a 3D scene of 2112x1188 fits, 2176x1224
   does not. Above 1920x1080 the pool grows with the scene area and the heap by the same amount; the application pool
   reported to the game grows with them (services.c). */
#define RT_POOL 0x6400000u
#define RT_HEAP 0xaa00000u
static uint32_t rt_pool_size(void) {
    unsigned w, h;
    if (!render_size(&w, &h, NULL) || (uint64_t)w * h <= 1920u * 1080u) return RT_POOL;
    /* 100 MiB holds a 2112x1188 scene and not 2176x1224, so the pool grows from that point with 8 % on top (2560x1440:
       160 MiB, which ran the Wooded Kingdom). Growing more costs memory elsewhere: with 256 MiB (+156) loading the Wooded
       Kingdom failed an allocation in another fixed heap and stopped with an SDK Break, also with the memory heap grown by
       the same amount; +60 left enough. */
    uint64_t pool = (uint64_t)RT_POOL * w * h * 108 / (2112u * 1188u * 100u);
    if (pool < RT_POOL) pool = RT_POOL;
    return (uint32_t)((pool + 0xffffff) & ~0xffffffULL);   /* 16 MiB steps; a movz with lsl 16 holds it */
}
uint64_t runtime_render_pool_extra(void) { return rt_pool_size() - RT_POOL; }
static void patch_rt_pool(Module *m) {
    uint32_t pool = rt_pool_size();
    if (pool == RT_POOL) return;
    uint32_t heap = RT_HEAP + (pool - RT_POOL);
    /* The game's whole memory heap is fixed too: nninitStartup asks for 0xc3000000 (main+0x4e321c, then
       nn::os::SetMemoryHeapSize), whatever the application pool is. Growing only the render target heap inside it left
       156 MiB less for the rest, and loading the Wooded Kingdom then failed an allocation (main+0x6af5dc) and stopped with
       an SDK Break; a bigger application pool alone did not change that. So it grows by the same amount, in the 2 MiB
       steps SetMemoryHeapSize takes. */
    uint32_t memheap = 0xc3000000u + ((pool - RT_POOL + 0x1fffffu) & ~0x1fffffu);
    const struct { uint32_t off, shipped, value; } site[] = {
        {0x624b4c, 0x52a15400, 0x52a00000 | (heap >> 16) << 5 | 0},      /* mov w0, #heap */
        {0x624b7c, 0x52a0c808, 0x52a00000 | (pool >> 16) << 5 | 8},      /* mov w8, #pool */
        {0x4e321c, 0x52b86000, 0x52a00000 | (memheap >> 16) << 5 | 0},   /* mov w0, #memory heap size */
    };
    for (size_t i = 0; i < 3; i++) {
        uint32_t word; memcpy(&word, m->base + site[i].off, 4);
        if (word != site[i].shipped) {
            fprintf(stderr, "[odyssey] render target pool main+%#x: found %08x expected %08x\n", site[i].off, word, site[i].shipped);
            runtime_fail("the render target pool size needs the instructions it replaces");
        }
    }
    for (size_t i = 0; i < 3; i++) memcpy(m->base + site[i].off, &site[i].value, 4);
    fprintf(stderr, "[odyssey] render target pool %u MiB (shipped 100), its heap %u MiB (shipped 170), memory heap %#x (shipped 0xc3000000)\n",
            pool >> 20, heap >> 20, memheap);
}
static void patch_render_res(Module *m) {
    const char *from; unsigned w, h;
    if (!render_size(&w, &h, &from)) return;
    enum { MOVZ_W, MOVZ_X, MOVK_X32 };
    static const struct { uint32_t off; unsigned rd, shipped, kind; int is_w; } site[] = {
        {0x4bf2a4, 9, 1600, MOVZ_X, 1}, {0x4bf2a8, 9, 900, MOVK_X32, 0},   // configuration: docked width | height << 32
        {0x4bf34c, 4, 1600, MOVZ_W, 1}, {0x4bf350, 5, 900, MOVZ_W, 0},     // main+0x624ad0: docked width, height
        {0x4e27bc, 3, 1600, MOVZ_W, 1}, {0x4e27c0, 4, 900, MOVZ_W, 0},     // window crop when docked
    };
    #define ENC(kind, rd, imm) ((kind) == MOVZ_W ? 0x52800000u : (kind) == MOVZ_X ? 0xd2800000u : 0xf2c00000u) | ((uint32_t)(imm) << 5) | (rd)
    for (size_t i = 0; i < sizeof site / sizeof *site; i++) {
        uint32_t word, want = ENC(site[i].kind, site[i].rd, site[i].shipped); memcpy(&word, m->base + site[i].off, 4);
        if (word != want) {
            fprintf(stderr, "[odyssey] render size main+%#x: found %08x expected %08x\n", site[i].off, word, want);
            runtime_fail("TOTK_RENDER_RES needs the instructions it replaces");
        }
    }
    /* The 3D scene is not set here: the game's dynamic resolution takes its largest docked size (1600x900) and its steps
       from SystemData/ProjectGraphicsQuality.szs, which scripts/make_render_res_overlay.py scales into a RomFS overlay.
       Setting the scene size in code instead (main+0x5efbb0 fixed, or main+0x5efc20 taking the requested side) drew light
       in blocks on the ground, because buffers sized for the largest step stayed 1600x900. */
    for (size_t i = 0; i < sizeof site / sizeof *site; i++) {
        uint32_t word = ENC(site[i].kind, site[i].rd, site[i].is_w ? w : h);
        memcpy(m->base + site[i].off, &word, 4);
    }
    #undef ENC
    g_render_w = w; g_render_h = h;
    fprintf(stderr, "[odyssey] docked render size %ux%u (shipped: 1600x900; from %s)\n", w, h, from);
}

/* Key 3 at a bigger render size: the 3D scene back to 1600x900 while playing, without a restart. The game's dynamic
   resolution picks the scene size each frame at main+0x5efc20 (called once, from main+0x6f144c): the smaller of the
   requested size (x2) and the limit (x0), written to x1, width then height. That leaf function is replaced by a call into
   odyssey_scene_pick, which does the same and then holds the result under a cap. A smaller scene inside the buffers
   sized for the largest step is how the game's own lower steps draw, so it stays clean. The window and the HUD stay at
   the patched size. */
static _Atomic uint32_t g_scene_cap, g_scene_pick;   /* width | height << 16; cap 0 = none */
/* Key 3's states: original 1600x900, the same with MetalFX, then native 1920x1080 and, at a bigger render size, native at
   that size. The game starts as shipped (3D 1600x900); window and HUD keep the patched size in every state, because they
   are only set when the game starts. */
enum { SCENE_ORIGINAL, SCENE_ORIGINAL_MFX, SCENE_1080, SCENE_MAX };
static int g_scene_mode = SCENE_ORIGINAL;
static int scene_states(void) { return g_render_w * g_render_h > 1920u * 1080u ? 4 : 3; }   /* SCENE_1080 is the max at 1080p */
static uint32_t scene_size(int mode) {
    return mode == SCENE_1080 && scene_states() == 4 ? 1920u | 1080u << 16 : mode >= SCENE_1080 ? g_render_w | g_render_h << 16 : 1600u | 900u << 16;
}
/* The shortcut box's key 3 line (frontend.m); NULL without a bigger render size (key 3 is then MetalFX on / off). */
const char *runtime_picture_name(void) {
    if (!g_render_w) return NULL;
    static char native[64];
    uint32_t s = scene_size(g_scene_mode);
    switch (g_scene_mode) {
    case SCENE_ORIGINAL: return TOTK_UI("原版 1600×900", "original 1600×900");
    case SCENE_ORIGINAL_MFX: return TOTK_UI("1600×900，MetalFX 放大到窗口", "1600×900, MetalFX to the window");
    default: snprintf(native, sizeof native, TOTK_UI("原生 %u×%u", "native %u×%u"), s & 0xffff, s >> 16); return native;
    }
}
static void odyssey_scene_pick(const int32_t *limit, int32_t *out, const int32_t *want) {
    int32_t w = want[0] < limit[0] ? want[0] : limit[0], h = want[1] < limit[1] ? want[1] : limit[1];
    uint32_t cap = atomic_load_explicit(&g_scene_cap, memory_order_relaxed);
    if (cap) { if (w > (int32_t)(cap & 0xffff)) w = (int32_t)(cap & 0xffff); if (h > (int32_t)(cap >> 16)) h = (int32_t)(cap >> 16); }
    out[0] = w; out[1] = h;
    atomic_store_explicit(&g_scene_pick, (uint32_t)w | (uint32_t)h << 16, memory_order_relaxed);
}
static void patch_scene_pick(Module *m) {
    static const uint32_t shipped[] = {   /* ldr w8,[x2]; ldr w9,[x0]; cmp; csel lt; str w8,[x1]; the same at +4; ret */
        0xb9400048, 0xb9400009, 0x6b09011f, 0x1a89b108, 0xb9000028, 0xb9400448, 0xb9400409, 0x6b09011f, 0x1a89b108, 0xb9000428, 0xd65f03c0};
    for (size_t i = 0; i < sizeof shipped / sizeof *shipped; i++) {
        uint32_t word; memcpy(&word, m->base + 0x5efc20 + 4 * i, 4);
        if (word != shipped[i]) {
            fprintf(stderr, "[odyssey] scene size pick main+%#zx: found %08x expected %08x\n", (size_t)(0x5efc20 + 4 * i), word, shipped[i]);
            runtime_fail("key 3 scene size needs the instructions it replaces");
        }
    }
    uint64_t target = (uint64_t)(uintptr_t)runtime_hle_wrap_named((void *)odyssey_scene_pick, "odyssey_scene_pick");
    const uint32_t veneer[2] = {0x58000051, 0xd61f0220};   /* ldr x17, #8; br x17 (the 8-byte target follows) */
    memcpy(m->base + 0x5efc20, veneer, sizeof veneer);
    memcpy(m->base + 0x5efc28, &target, sizeof target);
    /* SCENE_ORIGINAL until key 3; a render size picked in the settings window (the launcher sets ODYSSEY_SCENE_START=max,
       after PR #910) starts at that size instead */
    const char *start = getenv("ODYSSEY_SCENE_START");
    if (start && !strcmp(start, "max")) { g_scene_mode = scene_states() - 1; atomic_store(&g_scene_cap, 0); }
    else atomic_store(&g_scene_cap, 1600u | 900u << 16);
}
/* Present thread: one line when the picked scene size changes (the first 40 changes, then every 500th), so a test log says
   which size the game draws after key 3. */
static void scene_log(void) {
    static uint32_t last; static unsigned changes;
    uint32_t now = atomic_load_explicit(&g_scene_pick, memory_order_relaxed);
    if (!now || now == last) return;
    last = now; changes++;
    if (changes <= 40 || changes % 500 == 0)
        fprintf(stderr, "[odyssey] 3D scene %ux%u (change %u)\n", now & 0xffff, now >> 16, changes);
}

void runtime_game_patches(Module *main_module) {
    patch_render_res(main_module);
    if (g_render_w) { patch_scene_pick(main_module); patch_rt_pool(main_module); }
    fprintf(stderr, "[odyssey] game patches: %s\n", g_render_w ? "docked render size, key 3 scene size" : "none");
    odyssey_aspect_install(main_module);
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; odyssey_aspect_present(); scene_log(); }   /* key 4, every present */
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
/* Key 3 (runtime/input.c). At a bigger render size: original 1600x900 -> with MetalFX -> native 1920x1080 -> native at that
   size. Without one: MetalFX upscaling of the picture to the window, on / off (the app starts it on, Odyssey Data/no-metalfx
   off). Odyssey draws its docked picture straight into the canvas the present shows: MetalFX works at the output stage
   (nvn_mtl.m output_upscale; nothing happens while the window is not bigger than the picture). */
int runtime_output_mode_cycle(void) {
    extern int mfxup_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    if (g_render_w) {
        g_scene_mode = (g_scene_mode + 1) % scene_states();
        atomic_store(&g_scene_cap, g_scene_mode == scene_states() - 1 ? 0 : scene_size(g_scene_mode));   /* the largest: no cap */
        dm_gfx_set_flag("mfx_output", g_scene_mode == SCENE_ORIGINAL_MFX);
        dm_gfx_set_flag("mfx_upscale", g_scene_mode == SCENE_ORIGINAL_MFX);
        fprintf(stderr, "[odyssey] key 3: %s\n", runtime_picture_name());
        return 1;
    }
    int next = !mfxup_enabled();
    dm_gfx_set_flag("mfx_output", next);
    dm_gfx_set_flag("mfx_upscale", next);
    fprintf(stderr, "[odyssey] key 3: %s\n", next ? "MetalFX upscales the picture to the window" : "MetalFX off");
    return 1;
}
