// DM_NVN_CAPTURE end to end on the GPU, without game files: two frames of synthetic records through the capture hooks, the second one captured; `make check_t_capture_live`
// then replays the file with nvn_replay, which must give the same digest. The captured frame depends on everything a capture has to carry: a render target's content
// from the frame before (texture readback), pool bytes the CPU wrote before the first submit and between the two submits (regions, block changes), a copy from a pool
// into the target and a buffer fill (relocated addresses), a call into a second segment, a texture object outside the records, a combined program the replay
// layer makes for itself (its serial must not be a captured one) and a registered texture whose memory pool was finalized.
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
    // the programs below have no stages, but resolving one loads the shader map: an empty one beside the capture (the capture's header carries it to the replayer)
    char shmap[1200]; snprintf(shmap, sizeof shmap, "%s.shmap.tsv", argv[1]); FILE *sf = fopen(shmap, "w"); if (!sf) return 2; fclose(sf); setenv("DM_NVN_SHMAP", shmap, 1);
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
    // a texture on a pool of its own, registered, then the pool finalized as nvnMemoryPoolFinalize does and the pool object's memory reused: the ID stays registered with its
    // storage gone, and the capture must not read the dead pool object (Pokemon Scarlet: a texture's bytes there read as a sparse pool's mode, or as addresses outside every pool)
    static pool_t p2; size_t p2sz = 64 << 10; uint8_t *mem2 = NULL; if (posix_memalign((void **)&mem2, (size_t)getpagesize(), p2sz)) return 2;
    mtl_pool_add(mem2, p2sz); p2 = (pool_t){MAGIC('P', 'O', 'O', 'L'), 0, mem2, p2sz, NULL, 0};
    tex_t t2 = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial(), .pool = &p2, .mem = mem2};
    t2.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 16, .height = 16, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t2.d, &t2.L); mtl_tex_init(&t2); mtl_reg_tex(5, &t2, NULL);
    mtl_pool_del(mem2); mtl_alias_drop_pool(&p2); p2 = (pool_t){0, 0x24924924, (uint8_t *)0x2492492492497f80ull, 0x2492492492497f80ull, NULL, 2};
    // frame 2: two submits; the CPU writes the pool before each, the second copy reads what was written between them
    for (int i = 0; i < 16 * 16; i++) { pool[0x10000 + 4 * i] = 200; pool[0x10000 + 4 * i + 1] = (uint8_t)(3 * i); pool[0x10000 + 4 * i + 2] = 90; pool[0x10000 + 4 * i + 3] = 255; }
    chain a = {{0}, 0};
    // two programs bound to the vertex and fragment stages apart: the replay layer makes a combined program and gives it a serial of its own (bind_stages). The target
    // above keeps serial 1 in the capture, so a replay counting from 1 gave that serial to the combined program and the target resolved to it (Pokemon Scarlet)
    static prog_t pv = {.magic = MAGIC('P', 'R', 'O', 'G')}, pf = {.magic = MAGIC('P', 'R', 'O', 'G')}; pv.serial = nvn_new_serial(); pf.serial = nvn_new_serial();
    { p_bind_program *p = emit(&a, OP_BIND_PROGRAM, sizeof *p); *p = (p_bind_program){&pv, 0x1}; }
    { p_bind_program *p = emit(&a, OP_BIND_PROGRAM, sizeof *p); *p = (p_bind_program){&pf, 0x2}; }
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
    if (g_cap_unmapped) { fprintf(stderr, "FAIL: %llu addresses outside every pool (the finalized pool's object read)\n", (unsigned long long)g_cap_unmapped); return 1; }
    if (*(uint32_t *)(pool + 0x30000) != 0x11223344) { fprintf(stderr, "FAIL: the called segment's buffer fill did not run\n"); return 1; }
    // a shader's interface hash is of its binding values, not of nvn_refl.h's struct bytes: pinned, so a field widened or added there (682fdd26: nvn_rbind.n, soff) does not
    // change it, which made the replayer refuse every capture although no binding had changed
    { NvShader *sh = [NvShader new]; sh->rf.nb = 2; sh->rf.nv = 1; sh->rf.tg[0] = 64;
      sh->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEXA, .reg = 3, .aid = 1, .off = 0x40, .n = 300, .soff = 0x80}; sh->rf.b[1] = (nvn_rbind){.kind = 1, .rw = 1, .reg = 2};
      sh->rf.v[0].loc = 4; sh->rf.v[0].nvn = 2; sh->rf.v[0].sh[0] = sh->rf.v[0].sh[1] = sh->rf.v[0].sh[2] = 0xff;
      NvProg *pr = [NvProg new]; pr->sh[0] = sh; uint64_t h[6]; cap_iface(pr, h);
      if (h[0] != 0xa0e0f743879fbedfull || h[1]) { fprintf(stderr, "FAIL: interface hash %#llx, pinned %#llx (cap_iface must hash binding values, not struct bytes)\n", (unsigned long long)h[0], 0xa0e0f743879fbedfull); return 1; } }
    printf("t_capture_live: frame 2 captured to %s\n", argv[1]);
    return 0;
} }
