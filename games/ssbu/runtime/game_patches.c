#include "game_patches.h"
#include "game_version.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

/* Super Smash Bros. Ultimate 13.0.2: the docked render size (Q45, below) is the only game-code patch. Mario's fork patched
   its render size, render target pool and scene size pick at Mario addresses and found the camera for key 4 by Mario's
   vtable (games/smo/runtime/game_patches.c, smo_aspect.c); none of that applies here, so the other hooks the shared
   graphics layer, the input keys and the panel call answer "off" until Smash has its own. Every patch checks the original
   instruction before it writes, as CONTRIBUTING.md asks. */
/* GPU paths the graphics layer keeps isolated until their standalone probe passes (nvn_mtl.m gpu_path_on). Smash needs
   these; each one's probe passed on this host before it was switched on here (an explicit value in the environment wins):
     TOTK_GPU_MIDCLEAR  probe-gpu-midclear 23.10.: 9,216 checks (masked, scissored colour clears inside a pass, two
                        colour targets and depth); Smash clears scissored inside its passes from frame 2 on. */
static const char *const probed_gpu_paths[] = {"TOTK_GPU_MIDCLEAR"};

/* Q45, docked render size (TOTK_RENDER_RES=<w>x<h>; unset: 1920x1080 as shipped). Where 13.0.2 takes its sizes from
   (disassembly + TOTK_SIZE_CHAIN, 2026-10-23):
   A  main+0x3743f48 puts 1920 | 1920 << 32 into the configuration (+4, +8) that main+0x35a3d30 hands to the render
      pipeline setup main+0x36124a0; all twelve 1920x1080 textures made at boot come from there (scene targets and the
      three swapchain textures, chain ... <- main+0x3615e10 <- main+0x35a3fbc <- main+0x3743f64). The height follows from
      the width. The constant is a bitmask immediate that 2560 is not, so the three instructions that store it become
      mov w8 + two 32-bit stores (x9, which the shipped code used as the address, is not read afterwards).
   B  the screen size the frame is drawn at, docked: main+0x3614ad4 (pipeline setup) and main+0x37cb7c4 (the game's
      OperationModeChanged handler, message 0x1e) write width | height << 32 to the graphics object (+0x30) and send
      event 0xe with the same size (main+0x3614b08, main+0x37cb7f8); main+0x3741444 does the same; main+0x374384c gives
      nuGraphics its first size and main+0x37438d4 stores 1920.0f / 1080.0f into four globals. In handheld mode (measured
      with TOTK_OPERATION_MODE=handheld) the targets stay 1920x1080 and the final copy reads a 1280x720 part of them
      (c3[21] = 0.6667), so this size and the target size above are separate.
   C  the window crop (main+0x374795c / 0x3747968; nvnWindowSetCrop at main+0x3747994), 1920x1080 docked.
   D  main+0x363c5c4 turns the screen width into an index (1280 -> 1, 1920 -> 2); compared with the new width instead.
   E  other docked 1920x1080 values: main+0x378e8a8 is the UI's size when the byte main+0x593b8d4 is set (else it takes
      the screen size from main+0x523d480); main+0x36e4cd8 and main+0x35b407c are object defaults whose use is not traced.
   SSBU_RES_SITES=<letters> (diagnostics) patches only those groups; default all. Every word is checked first. */
static unsigned g_res_w, g_res_h;   /* 0 = shipped */
static uint8_t *g_main_base;
/* G  the other render targets (80 of the 92 the pipeline makes at boot, the game picture 7883 among them) take their size
      from framebuffer records of the loaded render pipeline data ({name, width +8, height +0xc, ...}: FB_MAIN, DEPTH_MAIN,
      OffscreenFB 1280x720, FB_BLEND_64 240x135 ...). Measured 23.10. (TOTK_SIZE_CHAIN=all, docked): 1920x1080 x8,
      960x540 x10, 480x270 x15, 240x135 x5, 720x405, 1280x720 x2, 120x67 x2, 120x62 x2, 480x240, 60x33, 30x16, squares.
      With the screen at 2560x1440 and these at 1920x1080 the title was drawn enlarged and cut off (run 452). Scaling the
      copy the target is made from (a vtable shim on main+0x37db450, run 454) grew the targets, but the passes kept their
      960x540 / 1920x1080 viewports; so the records themselves are scaled. Three record kinds copy width and height into
      the descriptor, each with four instructions whose scratch register is written first and dead afterwards:
      main+0x3821ee0 (record x22, scratch w9), main+0x3822010 (x19, w8), main+0x38220d0 (x19, w9). Each becomes
      ldr xS, #8; br xS; .quad shim. The shim keeps the caller-saved registers, flags and vector registers, has
      ssbu_rec_scale scale the record once (a record seen again is not scaled twice), replays the four instructions and
      jumps back. */
void *ssbu_rec_thunk, *ssbu_rec_back[3];
#define REC_SHIM(n, rec, s) \
    ".p2align 2\n.globl _ssbu_rec_shim" #n "\n_ssbu_rec_shim" #n ":\n" \
    "  sub sp, sp, #0x220\n  stp x0, x1, [sp]\n  stp x2, x3, [sp, #0x10]\n  stp x4, x5, [sp, #0x20]\n  stp x6, x7, [sp, #0x30]\n" \
    "  stp x8, x9, [sp, #0x40]\n  stp x10, x11, [sp, #0x50]\n  stp x12, x13, [sp, #0x60]\n  stp x14, x15, [sp, #0x70]\n" \
    "  stp x16, x17, [sp, #0x80]\n  mrs x0, nzcv\n  stp x30, x0, [sp, #0x90]\n" \
    "  stp q0, q1, [sp, #0xa0]\n  stp q2, q3, [sp, #0xc0]\n  stp q4, q5, [sp, #0xe0]\n  stp q6, q7, [sp, #0x100]\n" \
    "  stp q16, q17, [sp, #0x120]\n  stp q18, q19, [sp, #0x140]\n  stp q20, q21, [sp, #0x160]\n  stp q22, q23, [sp, #0x180]\n" \
    "  stp q24, q25, [sp, #0x1a0]\n  stp q26, q27, [sp, #0x1c0]\n  stp q28, q29, [sp, #0x1e0]\n  stp q30, q31, [sp, #0x200]\n" \
    "  mov x0, " #rec "\n  adrp x16, _ssbu_rec_thunk@PAGE\n  ldr x16, [x16, _ssbu_rec_thunk@PAGEOFF]\n  blr x16\n" \
    "  ldp q30, q31, [sp, #0x200]\n  ldp q28, q29, [sp, #0x1e0]\n  ldp q26, q27, [sp, #0x1c0]\n  ldp q24, q25, [sp, #0x1a0]\n" \
    "  ldp q22, q23, [sp, #0x180]\n  ldp q20, q21, [sp, #0x160]\n  ldp q18, q19, [sp, #0x140]\n  ldp q16, q17, [sp, #0x120]\n" \
    "  ldp q6, q7, [sp, #0x100]\n  ldp q4, q5, [sp, #0xe0]\n  ldp q2, q3, [sp, #0xc0]\n  ldp q0, q1, [sp, #0xa0]\n" \
    "  ldp x30, x0, [sp, #0x90]\n  msr nzcv, x0\n  ldp x16, x17, [sp, #0x80]\n  ldp x14, x15, [sp, #0x70]\n" \
    "  ldp x12, x13, [sp, #0x60]\n  ldp x10, x11, [sp, #0x50]\n  ldp x8, x9, [sp, #0x40]\n  ldp x6, x7, [sp, #0x30]\n" \
    "  ldp x4, x5, [sp, #0x20]\n  ldp x2, x3, [sp, #0x10]\n  ldp x0, x1, [sp]\n  add sp, sp, #0x220\n" \
    "  ldr w" #s ", [" #rec ", #8]\n  str w" #s ", [sp, #0x4b8]\n  ldr w" #s ", [" #rec ", #0xc]\n  str w" #s ", [sp, #0x4bc]\n" \
    "  adrp x" #s ", _ssbu_rec_back@PAGE\n  add x" #s ", x" #s ", _ssbu_rec_back@PAGEOFF\n  ldr x" #s ", [x" #s ", #" #n "*8]\n  br x" #s "\n"
__asm__(".text\n" REC_SHIM(0, x22, 9) REC_SHIM(1, x19, 8) REC_SHIM(2, x19, 9));
void ssbu_rec_shim0(void), ssbu_rec_shim1(void), ssbu_rec_shim2(void);
static const uint32_t *g_rec_seen[512];
static unsigned g_rec_n, g_rec_scaled, g_rec_kept;
static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;
/* SSBU_NRPD_DUMP=<file> (diagnostic): at the first record, before it is scaled, the loaded pipeline data written to <file>:
   from 0x10 before an "NRPD" magic found within 8 MiB before the record, else (as in 13.0.2, run 459) from 1 MiB before
   it, about 2 MiB, read with mach_vm_read_overwrite up to the first unreadable page. */
static void nrpd_dump(const uint32_t *rec) {
    const char *path = getenv("SSBU_NRPD_DUMP"); if (!path) return;
    static uint8_t buf[1 << 16];
    uintptr_t at = (uintptr_t)rec & ~(uintptr_t)0xffff, hbss = 0; int k = 0;
    for (; k < 128 && !hbss; k++, at -= sizeof buf) {
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), at, sizeof buf, (mach_vm_address_t)(uintptr_t)buf, &got) != KERN_SUCCESS) break;
        for (size_t j = (sizeof buf - 0x14) / 4 + 1; j-- > 0; ) {   /* 4-aligned offsets, highest first */
            size_t i = j * 4;
            if (at + i <= (uintptr_t)rec && !memcmp(buf + i, "NRPD", 4)) { hbss = at + i >= 0x10 ? at + i - 0x10 : at + i; break; }
        }
    }
    if (!hbss) {   /* no magic: the window from 1 MiB before the record */
        fprintf(stderr, "[ssbu] NRPD dump: no NRPD magic in %d KiB readable before %p; dumping from -1 MiB\n", k * 64, (void *)rec);
        hbss = ((uintptr_t)rec & ~(uintptr_t)0xffff) - (1u << 20);
    }
    FILE *f = fopen(path, "wb"); if (!f) return;
    size_t total = 0;
    for (uintptr_t p = hbss; total < (2u << 20) + 0x10000; p += sizeof buf) {
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), p, sizeof buf, (mach_vm_address_t)(uintptr_t)buf, &got) != KERN_SUCCESS || !got) break;
        fwrite(buf, 1, got, f); total += got;
    }
    fclose(f);
    fprintf(stderr, "[ssbu] NRPD dump: header at %p, first record at +%#lx, %zu bytes -> %s\n", (void *)hbss, (unsigned long)((uintptr_t)rec - hbss), total, path);
}
/* The framebuffer sizes that follow the screen: every non-square size from 16x8 to 1920x1080 (measured list above plus
   480x240 and 120x62, which the game's own scaled viewports fill, see H). Squares (shadow, reflection, result portraits)
   and the 10x1 / 4x4 helpers keep their size. */
static int screen_shape(double w, double h) {
    return w != h && w >= 16 && h >= 8 && w <= 1920 && h <= 1080;
}
/* H  viewports: the passes draw at viewports stored as float records in the same data, {name, x, y, w, h, 0.0, 1.0,
      u32 +0x20, byte +0x24} (dump of run 459: 306; 1920x1080, 960x540, 1280x720 x109, 640,0,640,720 for the right half
      of the 1280x720 offscreen target, 512x512 shadows ...). main+0x3834fbc makes a nu::ViewportPlug from a record
      (scale 1.0 at +0xd0); its bind (main+0x386d1f0) multiplies the rectangle by that scale. Plugs that the game scales
      (I below) and plugs it never scales both draw into the same scaled targets, so with I making the game's scale 1.0
      at the new size, every record whose x+w by y+h follows the screen (screen_shape) is scaled like the framebuffer
      records (runs 461/462: scaling all of them without I gave 3413x1920 for the game-scaled ones, scaling only some
      left 960x540 viewports in 1280x720 targets). Searched once, in the 1 MiB after the first framebuffer record (they
      lay at +0x61b0 .. +0xad170 of it), readable pages only.
   I  the game's own viewport scale: main+0x3611f40 sets plug scale = screen size / (1920.0, 1080.0) (rodata
      main+0x4471d58 / main+0x44718ac, read by eight other places too) for the plugs it keeps (11 of 14 MainViewport,
      FinalViewport, Blend16Viewport ...: 1.333 at 2560x1440, SSBU_VP_LOG run 465). Its two divisors become the new size:
      the adrp / ldr s1 pairs at main+0x3611f60 / 0x3611f68 and 0x3611f6c / 0x3611f74 turn into mov w10, #<float>;
      fmov s1, w10 (x10 is loaded anew at main+0x3611f90). The other scale, main+0x3618af0 (screen / the float globals
      that B sets, times the dynamic-resolution factor), is 1.0 already. */
static unsigned g_vp_scaled, g_vp_kept;
static void scale_viewports(const uint32_t *rec0) {
    static uint8_t buf[1 << 20];
    uintptr_t from = (uintptr_t)rec0 & ~(uintptr_t)0xffff; size_t len = 0;
    while (len < sizeof buf) {   /* readable 64 KiB pieces, up to the first that is not */
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), from + len, 1 << 16, (mach_vm_address_t)(uintptr_t)(buf + len), &got) != KERN_SUCCESS || got != 1 << 16) break;
        len += 1 << 16;
    }
    for (size_t i = 0; i + 0x28 <= len; i += 8) {
        int64_t rel; float f[6]; memcpy(&rel, buf + i, 8); memcpy(f, buf + i + 8, 24);
        if (rel <= 0 || rel >= 0x400 || f[4] != 0.0f || f[5] != 1.0f) continue;
        if (f[0] < 0 || f[1] < 0 || f[2] <= 0 || f[3] <= 0 || f[0] + f[2] > 4096 || f[1] + f[3] > 4096) continue;
        if (f[0] != (float)(int)f[0] || f[1] != (float)(int)f[1] || f[2] != (float)(int)f[2] || f[3] != (float)(int)f[3]) continue;
        size_t k = 0;   /* the name: printable and ended inside the window */
        if (i + rel + 64 > len) continue;
        while (k < 64 && buf[i + rel + k] >= 32 && buf[i + rel + k] < 127) k++;
        if (!k || buf[i + rel + k]) continue;
        if (!screen_shape(f[0] + f[2], f[1] + f[3])) { g_vp_kept++; continue; }
        float *v = (float *)(from + i + 8), sx = (float)g_res_w / 1920.0f, sy = (float)g_res_h / 1080.0f;
        v[0] *= sx; v[1] *= sy; v[2] *= sx; v[3] *= sy; g_vp_scaled++;
        if (getenv("SSBU_RES_LOG") && g_vp_scaled <= 400) fprintf(stderr, "[ssbu] viewport %s %g,%g %gx%g -> %g,%g %gx%g\n", (const char *)buf + i + rel, f[0], f[1], f[2], f[3], v[0], v[1], v[2], v[3]);
    }
    fprintf(stderr, "[ssbu] viewports: %u scaled, %u kept (records after %p)\n", g_vp_scaled, g_vp_kept, (const void *)rec0);
}
static void ssbu_rec_scale(uint32_t *rec) {
    { static int dumped; if (!dumped) { dumped = 1; nrpd_dump(rec); if (strchr(getenv("SSBU_RES_SITES") ? getenv("SSBU_RES_SITES") : "H", 'H')) scale_viewports(rec); } }
    pthread_mutex_lock(&g_rec_lock);
    unsigned i = 0; while (i < g_rec_n && g_rec_seen[i] != rec) i++;
    if (i < g_rec_n || g_rec_n == 512) { pthread_mutex_unlock(&g_rec_lock); if (i == 512) runtime_fail("render size: more than 512 pipeline records"); return; }
    g_rec_seen[g_rec_n++] = rec;
    uint32_t w = rec[2], h = rec[3];
    int screen = screen_shape(w, h);
    if (screen) { rec[2] = (w * g_res_w + 960) / 1920; rec[3] = (h * g_res_h + 540) / 1080; g_rec_scaled++; } else g_rec_kept++;
    pthread_mutex_unlock(&g_rec_lock);
    if (getenv("SSBU_RES_LOG")) fprintf(stderr, "[ssbu] pipeline record %p %ux%u %s %ux%u\n", (void *)rec, w, h, screen ? "->" : "kept", rec[2], rec[3]);
}
/* SSBU_VP_LOG=1 (diagnostic): nu::ViewportPlug's bind (vtable 0x526f6e8 slot +0x10, main+0x386d1f0) logs each plug once:
   name (libc++ string at +0x68), rectangle (+0xa0), kind (+0xc8) and the scale it is multiplied by (+0xd0). */
void *ssbu_vp_orig, *ssbu_vp_log_thunk;
__asm__(".text\n.p2align 2\n.globl _ssbu_vp_shim\n_ssbu_vp_shim:\n"
        "  stp x29, x30, [sp, #-48]!\n  mov x29, sp\n  stp x0, x1, [sp, #16]\n  str x8, [sp, #32]\n"
        "  adrp x16, _ssbu_vp_log_thunk@PAGE\n  ldr x16, [x16, _ssbu_vp_log_thunk@PAGEOFF]\n  blr x16\n"
        "  ldp x0, x1, [sp, #16]\n  ldr x8, [sp, #32]\n  ldp x29, x30, [sp], #48\n"
        "  adrp x16, _ssbu_vp_orig@PAGE\n  ldr x16, [x16, _ssbu_vp_orig@PAGEOFF]\n  br x16\n");
void ssbu_vp_shim(void);
static void ssbu_vp_log(const uint8_t *plug) {
    static struct { const void *p; float rs[6]; } seen[1024]; static unsigned n, lines; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    float rs[6]; memcpy(rs, plug + 0xa0, 16); memcpy(rs + 4, plug + 0xd0, 8);
    pthread_mutex_lock(&mu); unsigned i = 0; while (i < n && seen[i].p != plug) i++;
    int changed = i == n ? n < 1024 : memcmp(seen[i].rs, rs, sizeof rs) != 0;
    if (changed) { if (i == n) seen[n++].p = plug; memcpy(seen[i].rs, rs, sizeof rs); }
    int say = changed && lines < 800; if (say) lines++;
    pthread_mutex_unlock(&mu);
    if (!say) return;
    char name[64] = ""; uint8_t b0 = plug[0x68];
    if (!(b0 & 1)) { size_t l = b0 >> 1; if (l > 22) l = 22; memcpy(name, plug + 0x69, l); name[l] = 0; }
    else { const char *p; uint64_t l; memcpy(&l, plug + 0x70, 8); memcpy(&p, plug + 0x78, 8); if (l > 63) l = 63; if (p) { memcpy(name, p, l); name[l] = 0; } }
    uint32_t kind; memcpy(&kind, plug + 0xc8, 4);
    extern uint64_t g_frame_pub;
    fprintf(stderr, "[ssbu] viewport plug F%llu %p %s kind %u rect %g,%g %gx%g scale %g x %g\n", (unsigned long long)g_frame_pub, (const void *)plug, name, kind, rs[0], rs[1], rs[2], rs[3], rs[4], rs[5]);
}
static void patch_render_res(Module *m) {
    if (getenv("SSBU_VP_LOG")) {
        uint64_t slot; memcpy(&slot, m->base + 0x526f6f8, 8);
        if (slot != (uint64_t)(uintptr_t)(m->base + 0x386d1f0)) runtime_fail("SSBU_VP_LOG needs the 13.0.2 viewport plug vtable");
        ssbu_vp_orig = m->base + 0x386d1f0; ssbu_vp_log_thunk = runtime_hle_wrap_named((void *)ssbu_vp_log, "ssbu viewport plug log");
        uint64_t shim = (uint64_t)(uintptr_t)ssbu_vp_shim; memcpy(m->base + 0x526f6f8, &shim, 8);
    }
    const char *e = getenv("TOTK_RENDER_RES"); char tail;
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "1920x1080")) return;
    unsigned w, h;
    if (sscanf(e, "%ux%u%c", &w, &h, &tail) != 2 || w < 1280 || h < 720 || w > 3840 || h > 2160 || w * 9 != h * 16)
        runtime_fail("TOTK_RENDER_RES must be a 16:9 <w>x<h> between 1280x720 and 3840x2160");
    union { float f; uint32_t u; } fw = {(float)w}, fh = {(float)h};
    if ((fw.u & 0xffff) || (fh.u & 0xffff)) runtime_fail("TOTK_RENDER_RES: width and height must be exact in the upper half of a float (2560x1440 is)");
    enum { MOVZ_W, MOVZ_X, MOVK_X32, MOVZ_W16, CMP_W, STR_W_SP, FMOV_S_W };
    enum { V_W, V_H, V_FW, V_FH, V_SP354, V_SP358 };
    static const struct { char group; uint32_t off, shipped; unsigned kind, reg, val; } site[] = {
        {'A', 0x3743f48, 0xb2190fe8, MOVZ_W, 8, V_W},        /* mov x8, #0x78000000780 -> mov w8, #w */
        {'A', 0x3743f4c, 0x910957e9, STR_W_SP, 8, V_SP354},  /* add x9, sp, #0x255     -> str w8, [sp, #0x354] */
        {'A', 0x3743f5c, 0xf80ff128, STR_W_SP, 8, V_SP358},  /* stur x8, [x9, #0xff]   -> str w8, [sp, #0x358] */
        {'B', 0x3614ad4, 0xd280f008, MOVZ_X, 8, V_W}, {'B', 0x3614ad8, 0xf2c08708, MOVK_X32, 8, V_H},
        {'B', 0x3614b08, 0x52808717, MOVZ_W, 23, V_H}, {'B', 0x3614b0c, 0xf2c0f016, MOVK_X32, 22, V_W},
        {'B', 0x37cb7c4, 0xd280f008, MOVZ_X, 8, V_W}, {'B', 0x37cb7c8, 0xf2c08708, MOVK_X32, 8, V_H},
        {'B', 0x37cb7f8, 0x52808716, MOVZ_W, 22, V_H}, {'B', 0x37cb7fc, 0xf2c0f015, MOVK_X32, 21, V_W},
        {'B', 0x3741444, 0x52808715, MOVZ_W, 21, V_H}, {'B', 0x3741448, 0x32190ff6, MOVZ_W, 22, V_W},
        {'B', 0x374384c, 0xd280f008, MOVZ_X, 8, V_W}, {'B', 0x3743860, 0xf2c08708, MOVK_X32, 8, V_H},
        {'B', 0x37438d4, 0x52a89e09, MOVZ_W16, 9, V_FW}, {'B', 0x37438e0, 0x52a890ea, MOVZ_W16, 10, V_FH},
        {'C', 0x374795c, 0x32190fe9, MOVZ_W, 9, V_W}, {'C', 0x3747968, 0x52808709, MOVZ_W, 9, V_H},
        {'D', 0x363c5c4, 0x711e011f, CMP_W, 8, V_W},
        {'E', 0x378e8a8, 0x32190fe8, MOVZ_W, 8, V_W}, {'E', 0x378e8bc, 0x52808708, MOVZ_W, 8, V_H},
        {'E', 0x36e4cd8, 0x32190fe8, MOVZ_W, 8, V_W}, {'E', 0x36e4cdc, 0x52808709, MOVZ_W, 9, V_H},
        {'E', 0x35b407c, 0xd280f00b, MOVZ_X, 11, V_W}, {'E', 0x35b4080, 0xf2c0870b, MOVK_X32, 11, V_H},
        {'I', 0x3611f60, 0x9000730a, MOVZ_W16, 10, V_FW}, {'I', 0x3611f68, 0xbd4d5941, FMOV_S_W, 1, 10},   /* adrp x10; ldr s1, [x10, #0xd58] */
        {'I', 0x3611f6c, 0x9000730a, MOVZ_W16, 10, V_FH}, {'I', 0x3611f74, 0xbd48ad41, FMOV_S_W, 1, 10},   /* adrp x10; ldr s1, [x10, #0x8ac] */
    };
    const char *groups = getenv("SSBU_RES_SITES"); if (!groups || !*groups) groups = "ABCDEGHIJ";
    static const struct { uint32_t off, shipped[4]; unsigned scratch; void (*shim)(void); } rec_site[] = {
        {0x3821ee0, {0xb9400ac9, 0xb904bbe9, 0xb9400ec9, 0xb904bfe9}, 9, ssbu_rec_shim0},   /* ldr w9,[x22,#8]; str w9,[sp,#0x4b8]; ldr w9,[x22,#0xc]; str w9,[sp,#0x4bc] */
        {0x3822010, {0xb9400a68, 0xb904bbe8, 0xb9400e68, 0xb904bfe8}, 8, ssbu_rec_shim1},   /* the same with x19, w8 */
        {0x38220d0, {0xb9400a69, 0xb904bbe9, 0xb9400e69, 0xb904bfe9}, 9, ssbu_rec_shim2},   /* the same with x19, w9 */
    };
    /* J  the UI's own factor for the screen: three places choose 1.0 when the UI width (main+0x523d480, the screen width)
          is 1920 or the byte main+0x593b8d4 is set, else 0.6667 (rodata main+0x4470ed0, the handheld 1280/1920). At 2560
          that gave 0.6667: the title logo's stencil mask (drawn from the UI's pixel rectangle) covered only the top-left
          quarter, so "SUPER SMASH BROS." ended at x = 1280 (runs 452-468), and a hidden element's empty scissor sat at
          640,360 instead of 1280,720 (title466). The value is computed instead, equal to the shipped one at 1920 and 1280:
          main+0x378ed28 (UI pixel rectangles, scissor and mask): width / 1920; main+0x1534e18 and main+0x153c348 (shader
          constants: rectangle * factor / 1920.0, a coordinate in the allocated buffers): width / the new width. */
    uint32_t hw = fw.u >> 16;
    struct { uint32_t off; unsigned n; uint32_t shipped[9], code[9]; } ui_site[] = {
        {0x378ed28, 7, {0x1e2e1001, 0x711e017f, 0x1a9f17eb, 0x2a0b014a, 0x7200015f, 0xd000670a, 0xbd4ed140},
                       /* scvtf s0, w11; mov w11, #1920.0f; fmov s1, w11; fdiv s0, s0, s1; fmov s1, #1.0; tst w10, #1; nop */
                       {0x1e220160, 0x52a89e0b, 0x1e270161, 0x1e211800, 0x1e2e1001, 0x7200015f, 0xd503201f}},
        {0x1534e18, 9, {0x900179eb, 0xbd4ed160, 0x711e015f, 0xb00179ea, 0xbd4d5944, 0xb00179ea, 0x1e2e1001, 0xbd48ad45, 0x1a9f17ea},
                       /* scvtf s0, w10; mov w11, #<w>.0f; fmov s3, w11; (adrp x10; ldr s4); fdiv s0, s0, s3; (fmov s1, #1.0; ldr s5); mov w10, wzr */
                       {0x1e220140, 0x52a0000b | hw << 5, 0x1e270163, 0xb00179ea, 0xbd4d5944, 0x1e231800, 0x1e2e1001, 0xbd48ad45, 0x2a1f03ea}},
        {0x153c348, 9, {0x900179ab, 0xbd4ed160, 0x711e015f, 0xb00179aa, 0xbd4d5944, 0xb00179aa, 0x1e2e1001, 0xbd48ad45, 0x1a9f17ea},
                       {0x1e220140, 0x52a0000b | hw << 5, 0x1e270163, 0xb00179aa, 0xbd4d5944, 0x1e231800, 0x1e2e1001, 0xbd48ad45, 0x2a1f03ea}},
    };
    for (size_t i = 0; i < 3; i++)
        if (memcmp(m->base + ui_site[i].off, ui_site[i].shipped, 4 * ui_site[i].n)) {
            fprintf(stderr, "[ssbu] render size: the UI factor at main+%#x is not the 13.0.2 code\n", ui_site[i].off);
            runtime_fail("TOTK_RENDER_RES needs the 13.0.2 instructions it replaces");
        }
    for (size_t i = 0; i < 3; i++)
        if (memcmp(m->base + rec_site[i].off, rec_site[i].shipped, 16)) {
            fprintf(stderr, "[ssbu] render size: pipeline record copy at main+%#x is not the 13.0.2 code\n", rec_site[i].off);
            runtime_fail("TOTK_RENDER_RES needs the 13.0.2 instructions it replaces");
        }
    for (size_t i = 0; i < sizeof site / sizeof *site; i++) {
        uint32_t word; memcpy(&word, m->base + site[i].off, 4);
        if (word != site[i].shipped) {
            fprintf(stderr, "[ssbu] render size main+%#x: found %08x expected %08x\n", site[i].off, word, site[i].shipped);
            runtime_fail("TOTK_RENDER_RES needs the 13.0.2 instructions it replaces");
        }
    }
    if (strchr(groups, 'G')) {
        ssbu_rec_thunk = runtime_hle_wrap_named((void *)ssbu_rec_scale, "ssbu pipeline record scale");
        for (size_t i = 0; i < 3; i++) {
            uint32_t s = rec_site[i].scratch, code[2] = {0x58000040u | s, 0xd61f0000u | s << 5};   /* ldr xS, #8; br xS */
            uint64_t to = (uint64_t)(uintptr_t)rec_site[i].shim;
            ssbu_rec_back[i] = m->base + rec_site[i].off + 16;
            memcpy(m->base + rec_site[i].off, code, 8); memcpy(m->base + rec_site[i].off + 8, &to, 8);
        }
    }
    unsigned n = 0;
    if (strchr(groups, 'J'))
        for (size_t i = 0; i < 3; i++) { memcpy(m->base + ui_site[i].off, ui_site[i].code, 4 * ui_site[i].n); n += ui_site[i].n; }
    for (size_t i = 0; i < sizeof site / sizeof *site; i++) {
        if (!strchr(groups, site[i].group)) continue;
        unsigned v = site[i].kind == FMOV_S_W ? site[i].val : site[i].val == V_W ? w : site[i].val == V_H ? h : site[i].val == V_FW ? fw.u >> 16
                   : site[i].val == V_FH ? fh.u >> 16 : site[i].val == V_SP354 ? 0x354 : 0x358, r = site[i].reg;
        uint32_t word = site[i].kind == MOVZ_W ? 0x52800000u | v << 5 | r : site[i].kind == MOVZ_X ? 0xd2800000u | v << 5 | r
                      : site[i].kind == MOVK_X32 ? 0xf2c00000u | v << 5 | r : site[i].kind == MOVZ_W16 ? 0x52a00000u | v << 5 | r
                      : site[i].kind == CMP_W ? 0x7100001fu | v << 10 | r << 5 : site[i].kind == FMOV_S_W ? 0x1e270000u | v << 5 | r
                      : 0xb90003e0u | (v / 4) << 10 | r;
        memcpy(m->base + site[i].off, &word, 4); n++;
    }
    g_res_w = w; g_res_h = h;
    fprintf(stderr, "[ssbu] docked render size %ux%u (shipped 1920x1080; TOTK_RENDER_RES), groups %s, %u instructions\n", w, h, groups, n);
}

void runtime_game_patches(Module *main_module) {
    g_main_base = main_module->base;
    for (size_t i = 0; i < sizeof probed_gpu_paths / sizeof *probed_gpu_paths; i++) setenv(probed_gpu_paths[i], "1", 0);
    patch_render_res(main_module);
    fprintf(stderr, "[ssbu] game patches: render size %s; probed GPU paths on: %zu\n", g_res_w ? "on" : "off", sizeof probed_gpu_paths / sizeof *probed_gpu_paths);
}

void runtime_frame_step_pin(double ms) { (void)ms; }
/* SSBU_UI_LOG=<present> (diagnostic): at that present, the sizes the UI and the scales read: main+0x523d480 (int w, h),
   main+0x593b8d4 (byte), main+0x5238178 (float x, y, the dynamic-resolution pair), main+0x744701c .. 0x7447028 (floats). */
void runtime_frame_step_update(uint64_t now_ns) {
    (void)now_ns;
    static long at = -2, n; if (at == -2) { const char *e = getenv("SSBU_UI_LOG"); at = e ? atol(e) : -1; }
    if (at < 0 || ++n != at || !g_main_base) return;
    int32_t ui[2]; uint8_t flag; float drs[2], base[4];
    memcpy(ui, g_main_base + 0x523d480, 8); memcpy(&flag, g_main_base + 0x593b8d4, 1); memcpy(drs, g_main_base + 0x5238178, 8); memcpy(base, g_main_base + 0x744701c, 16);
    fprintf(stderr, "[ssbu] ui log at present %ld: ui size %dx%d, flag 0x593b8d4 %u, drs %g x %g, floats 0x744701c.. %g %g %g %g\n", n, ui[0], ui[1], flag, drs[0], drs[1], base[0], base[1], base[2], base[3]);
}
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return g_res_w ? (uint64_t)g_res_h << 32 | g_res_w : (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* no scene size override for the heaps (romfs.c) */
uint64_t runtime_render_pool_extra(void) { return 0; }
const char *runtime_picture_name(void) { return NULL; }   /* the shared wording for key 3 */
const char *runtime_interp_name(void) { return NULL; }    /* the shared wording for key 2 */

/* Key 3 (runtime/input.c): MetalFX upscales the finished picture to the window, on / off (Mario's key 3 without a bigger
   render size: it works at the output stage only, so it needs nothing from the game). */
int runtime_output_mode_cycle(void) {
    extern int mfxup_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    int next = !mfxup_enabled();
    dm_gfx_set_flag("mfx_output", next);
    dm_gfx_set_flag("mfx_upscale", next);
    fprintf(stderr, "[ssbu] key 3: %s\n", next ? "MetalFX upscales the picture to the window" : "MetalFX off");
    return 1;
}

/* Key 4 (picture shape) needs the game's camera projection; not found for Smash yet, so the panel shows it unavailable. */
int runtime_aspect_toggle(void) { return 0; }
int runtime_aspect_step(void) { return -1; }
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
float runtime_aspect_now(void) { return 0; }
