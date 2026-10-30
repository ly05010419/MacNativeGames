// DM_NVN_CAPTURE end to end on the GPU, without game files: two frames of synthetic records through the capture hooks, the second one captured; `make check_t_capture_live`
// then replays the file with nvn_replay, which must give the same digest. The captured frame depends on everything a capture has to carry: a render target's content
// from the frame before (texture readback), pool bytes the CPU wrote before the first submit and between the two submits (regions, block changes), a copy from a pool
// into the target and a buffer fill (relocated addresses), a call into a second segment and a texture object outside the records.
//   ./t_capture_live <capture path>
#import <Foundation/Foundation.h>
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void crash_install(void);   // host crash.c: the fault handler that also takes the dirty tracker's write faults on pool pages (nvn_dirty.c)

typedef struct { uint8_t p[4096]; size_t n; } chain;
static void *emit(chain *c, unsigned op, size_t n) {
    size_t sz = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)sz}; memcpy(c->p + c->n, &h, 8); memset(c->p + c->n + 8, 0, sz - 8);
    void *r = c->p + c->n + 8; c->n += sz; return r;
}
static void submit(const uint8_t *h0) {   // as nvnQueueSubmitCommands, with the capture hook
    uint64_t h = (uint64_t)(uintptr_t)h0;
    nvn_cap_submit(1, &h);
    mtl_sub_begin(); mtl_sub_index(&h, 1); mtl_exec_rest(NULL, 0); mtl_exec(h0); mtl_exec_rest(NULL, 0); mtl_sub_commit(); mtl_finish();
}
static void copy_block(chain *c, const tex_t *t, uint64_t src, int x, int y) {
    p_copy_b2t *b = emit(c, OP_COPY_B2T, sizeof *b); b->src = src; b->tex = t; b->r = (NVNcopyRegion){x, y, 0, 16, 16, 1};
}

int main(int argc, char **argv) { @autoreleasepool {
    crash_install();   // the CPU writes pool pages the dirty tracker protects, as in the host
    if (argc != 2) { fprintf(stderr, "usage: %s <capture path>\n", argv[0]); return 2; }
    char env[1200]; snprintf(env, sizeof env, "2:1:%s", argv[1]); setenv("DM_NVN_CAPTURE", env, 1);
    mtl_init();
    size_t psz = 256 << 10; uint8_t *pool = NULL; if (posix_memalign((void **)&pool, (size_t)getpagesize(), psz)) return 2;
    memset(pool, 0, psz); mtl_pool_add(pool, psz); nvn_dirty_add_range(pool, psz);
    tex_t rt = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    rt.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 64, .height = 64, .depth = 1, .levels = 1, .samples = 1, .flags = NVN_TEXTURE_FLAGS_COMPRESSIBLE};
    nvn_tex_layout(&rt.d, &rt.L); mtl_tex_init(&rt);
    chain sub = {{0}, 0}; p_clear_buf *cb = emit(&sub, OP_CLEAR_BUF, sizeof *cb); *cb = (p_clear_buf){(uint64_t)(uintptr_t)pool + 0x30000, 0x400, 0x11223344, 0}; emit(&sub, OP_END, 0);
    // frame 1: the target cleared red, with a gradient block copied in (the content frame 2 starts from)
    for (int i = 0; i < 16 * 16; i++) { pool[4 * i] = (uint8_t)i; pool[4 * i + 1] = (uint8_t)(255 - i); pool[4 * i + 2] = 7; pool[4 * i + 3] = 255; }
    chain f1 = {{0}, 0};
    { p_set_rt *r = emit(&f1, OP_SET_RT, sizeof *r); r->n = 1; r->color[0] = &rt; }
    { p_clear_color *c = emit(&f1, OP_CLEAR_COLOR, sizeof *c); *c = (p_clear_color){0, 0xf, {1, 0, 0, 1}}; }
    copy_block(&f1, &rt, (uint64_t)(uintptr_t)pool, 8, 8); emit(&f1, OP_END, 0);
    submit(f1.p); nvn_cap_present(&rt, NULL);
    // frame 2: two submits; the CPU writes the pool before each, the second copy reads what was written between them
    for (int i = 0; i < 16 * 16; i++) { pool[0x10000 + 4 * i] = 200; pool[0x10000 + 4 * i + 1] = (uint8_t)(3 * i); pool[0x10000 + 4 * i + 2] = 90; pool[0x10000 + 4 * i + 3] = 255; }
    chain a = {{0}, 0};
    { p_set_rt *r = emit(&a, OP_SET_RT, sizeof *r); r->n = 1; r->color[0] = &rt; }
    copy_block(&a, &rt, (uint64_t)(uintptr_t)pool + 0x10000, 32, 8);
    { uint64_t *c = emit(&a, OP_CALL, 8); *c = (uint64_t)(uintptr_t)sub.p; }
    emit(&a, OP_END, 0);
    submit(a.p);
    for (int i = 0; i < 16 * 16; i++) { pool[0x20000 + 4 * i] = (uint8_t)(i * 5); pool[0x20000 + 4 * i + 1] = 40; pool[0x20000 + 4 * i + 2] = (uint8_t)(255 - i); pool[0x20000 + 4 * i + 3] = 255; }
    chain b = {{0}, 0}; copy_block(&b, &rt, (uint64_t)(uintptr_t)pool + 0x20000, 8, 40); emit(&b, OP_END, 0);
    submit(b.p);
    nvn_cap_present(&rt, NULL);
    if (!g_cap_done || g_cap_failed) { fprintf(stderr, "FAIL: capture %s\n", g_cap_done ? "incomplete" : "not written"); return 1; }
    if (*(uint32_t *)(pool + 0x30000) != 0x11223344) { fprintf(stderr, "FAIL: the called segment's buffer fill did not run\n"); return 1; }
    printf("t_capture_live: frame 2 captured to %s\n", argv[1]);
    return 0;
} }
