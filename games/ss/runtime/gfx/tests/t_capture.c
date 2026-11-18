// DM_NVN_CAPTURE's format and record relocation without a GPU or game files (CI): synthetic records as hle_nvn.c writes them (pointers to pools, objects, snapshots
// inside the record, calls and jumps between segments) are encoded with the field walk, written as chunks, read back, decoded against regions at fresh addresses and checked
// field by field, and every byte the walk does not name must come back unchanged.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "../nvn/nvn_capture.h"

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

// ---- the "live" side: two pools, a heap word list, objects, three segments ----
static uint8_t *pool_a, *pool_b; static uint64_t heap_handles[4] = {0x100001, 0x200002, 0x300003, 0x400004};
static tex_t game_tex; static prog_t game_prog; static dpool_t game_dpool; static sync_t game_sync; static pool_t game_pool;
typedef struct { uint8_t p[4096]; size_t n; } seg;
static void *emit(seg *s, uint32_t op, size_t payload) {   // as hle_nvn.c rec(): header, zeroed payload, 8-byte aligned
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7); op_hdr h = {op, sz};
    memcpy(s->p + s->n, &h, 8); memset(s->p + s->n + 8, 0, sz - 8); void *r = s->p + s->n + 8; s->n += sz; return r;
}

// ---- encode: what nvn_capture.inc does, over plain tables ----
typedef struct { uint64_t lo, size; } rg; static rg regs[4]; static int nregs;
static const void *objs[CAP_K_N][8]; static int nobj[CAP_K_N];
static const uint8_t *orig_payload; static seg *segs_by_addr[3];
static int enc_addr(void *ctx, uint64_t *f, uint64_t len, int w) {
    (void)ctx; (void)w; uint64_t v = *f; if (!v) return 1;
    for (int i = 0; i < nregs; i++) if (v >= regs[i].lo && v < regs[i].lo + regs[i].size) { *f = cap_addr((uint32_t)i, v - regs[i].lo); return 1; }
    if (len == sizeof heap_handles && v == (uint64_t)(uintptr_t)heap_handles) { regs[nregs] = (rg){v, len}; *f = cap_addr((uint32_t)nregs++, 0); return 1; }   // a blob region
    return 1;   // outside every region: left as it is
}
static int enc_obj(void *ctx, int kind, uint64_t *f, uint8_t *payload, uint32_t size);
static const cap_ops enc = {enc_addr, enc_obj};
static int enc_obj(void *ctx, int kind, uint64_t *f, uint8_t *payload, uint32_t size) {
    (void)ctx; uint64_t v = *f; if (!v) return 1;
    if (kind == CAP_K_SEG) { for (int i = 0; i < 3; i++) if (v == (uint64_t)(uintptr_t)segs_by_addr[i]->p) { *f = cap_obj(CAP_K_SEG, (uint64_t)i); return 1; } return 0; }
    if (kind == CAP_K_TEX && v >= (uint64_t)(uintptr_t)orig_payload && v + sizeof(tex_t) <= (uint64_t)(uintptr_t)orig_payload + size) {
        uint64_t off = v - (uint64_t)(uintptr_t)orig_payload; cap_tex_fields((tex_t *)(payload + off), &enc, NULL); *f = cap_obj(CAP_K_TEXIN, off); return 1;
    }
    for (int i = 0; i < nobj[kind]; i++) if (objs[kind][i] == (const void *)(uintptr_t)v) { *f = cap_obj(kind, (uint64_t)i); return 1; }
    objs[kind][nobj[kind]] = (const void *)(uintptr_t)v; *f = cap_obj(kind, (uint64_t)nobj[kind]++); return 1;
}

// ---- decode: what nvn_replay does ----
static uint8_t *new_base[4]; static void *new_obj[CAP_K_N][8]; static uint8_t *new_seg[3];
static int dec_addr(void *ctx, uint64_t *f, uint64_t len, int w) { (void)ctx; (void)len; (void)w; if (cap_is_addr(*f)) *f = (uint64_t)(uintptr_t)(new_base[cap_addr_region(*f)] + cap_addr_off(*f)); return 1; }
static int dec_obj(void *ctx, int kind, uint64_t *f, uint8_t *payload, uint32_t size);
static const cap_ops dec = {dec_addr, dec_obj};
static int dec_obj(void *ctx, int kind, uint64_t *f, uint8_t *payload, uint32_t size) {
    (void)ctx; (void)size; uint64_t v = *f; if (!v) return 1;
    int k = cap_obj_kind(v); uint64_t i = cap_obj_index(v);
    if (k == CAP_K_TEXIN) { cap_tex_fields((tex_t *)(payload + i), &dec, NULL); *f = (uint64_t)(uintptr_t)(payload + i); return 1; }
    if (k != kind) return 0;
    *f = (uint64_t)(uintptr_t)(k == CAP_K_SEG ? (void *)new_seg[i] : new_obj[k][i]); return 1;
}
// marks the bytes of every field the walk names, to check that all the others come back unchanged
static uint8_t *mark_base, *mark; static int mark_addr(void *ctx, uint64_t *f, uint64_t len, int w) { (void)ctx; (void)len; (void)w; memset(mark + ((uint8_t *)f - mark_base), 1, 8); return 1; }
static int mark_obj(void *ctx, int kind, uint64_t *f, uint8_t *payload, uint32_t size) {
    (void)ctx; (void)size; memset(mark + ((uint8_t *)f - mark_base), 1, 8);
    if (kind == CAP_K_TEX && *f >= (uint64_t)(uintptr_t)payload && *f < (uint64_t)(uintptr_t)payload + size) { tex_t *t = (tex_t *)(uintptr_t)*f; memset(mark + ((uint8_t *)&t->mem - mark_base), 1, 8); memset(mark + ((uint8_t *)&t->pool - mark_base), 1, 8); }
    return 1;
}
static void walk(seg *s, uint8_t *bytes, const cap_ops *ops, const uint8_t *orig) {
    for (size_t o = 0; o < s->n;) { op_hdr h; memcpy(&h, bytes + o, 8); orig_payload = orig ? orig + o + 8 : NULL;
        CHECK(cap_rec_fields(h.op, bytes + o + 8, h.size, ops, NULL), "every op of the synthetic segments is walked"); o += h.size; }
}

int main(void) {
    char path[1024]; const char *tmp = getenv("TMPDIR"); snprintf(path, sizeof path, "%s/t_capture_XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(path); CHECK(fd >= 0, "temporary file"); close(fd);
    pool_a = calloc(1, 65536); pool_b = calloc(1, 16384);
    for (int i = 0; i < 65536; i += 64) pool_a[i] = (uint8_t)i;
    game_pool = (pool_t){MAGIC('P', 'O', 'O', 'L'), 0x8, pool_a, 65536, NULL, 0};
    game_tex = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .mem = pool_a + 0x2000, .pool = &game_pool, .off = 0x2000, .serial = 77}; game_tex.L.size = 0x1000;
    game_prog = (prog_t){.magic = MAGIC('P', 'R', 'O', 'G'), .nstage = 2, .serial = 5};
    game_dpool = (dpool_t){&game_pool, 0x8000, 256};
    seg s0 = {{0}, 0}, s1 = {{0}, 0}, s2 = {{0}, 0}; segs_by_addr[0] = &s0; segs_by_addr[1] = &s1; segs_by_addr[2] = &s2;
    {   // SET_RT with two snapshots inside the record and one object pointer (a texture invalid at record time, read at submit)
        p_set_rt *r = emit(&s0, OP_SET_RT, ((sizeof(p_set_rt) + 7) & ~7ul) + 2 * sizeof(tex_t)); r->n = 2;
        tex_t *in = (tex_t *)((uint8_t *)r + ((sizeof(p_set_rt) + 7) & ~7ul)); in[0] = game_tex; in[0].serial = 78; in[1] = game_tex; in[1].mem = pool_a + 0x4000; in[1].serial = 79;
        r->color[0] = &in[0]; r->color[1] = &game_tex; r->depth = &in[1];
    }
    p_bind_program *bp = emit(&s0, OP_BIND_PROGRAM, sizeof *bp); bp->prog = &game_prog; bp->stages = 3;
    p_set_pool *sp = emit(&s0, OP_SET_TEX_POOL, sizeof *sp); sp->pool = &game_dpool;
    p_bind_vbuf *vb = emit(&s0, OP_BIND_VBUF, offsetof(p_bind_vbuf, r) + 2 * sizeof(NVNbufferRange)); vb->n = 2; vb->r[0] = (NVNbufferRange){(uint64_t)(uintptr_t)pool_a + 0x100, 0x200}; vb->r[1] = (NVNbufferRange){(uint64_t)(uintptr_t)pool_b + 0x40, 0x10};
    p_bind_buf *ub = emit(&s0, OP_BIND_UBO, sizeof *ub); *ub = (p_bind_buf){1, 3, (uint64_t)(uintptr_t)pool_b + 0x800, 0x100};
    p_bind_ubo_small *us = emit(&s0, OP_BIND_UBO_SMALL, sizeof *us); *us = (p_bind_ubo_small){0, 2, 64, (uint64_t)(uintptr_t)pool_b + 0x1000};
    p_bind_handles *bh = emit(&s0, OP_BIND_TEX, offsetof(p_bind_handles, h) + 4 * 8); bh->stage = 1; bh->n = 4; bh->defer = 1; bh->src = (uint64_t)(uintptr_t)heap_handles;
    p_draw_idx *di = emit(&s0, OP_DRAW_IDX, sizeof *di); di->prim = 4; di->count = 3; di->ibuf = (uint64_t)(uintptr_t)pool_a + 0x3000; di->ind = (uint64_t)(uintptr_t)pool_a + 0x3100;
    p_draw *dr = emit(&s0, OP_DRAW, offsetof(p_draw, ind)); dr->count = 3;   // a plain draw stops before ind (hle_nvn.c rec_draw)
    uint64_t *ev = emit(&s0, OP_SIGNAL_EVENT, 16); ev[0] = (uint64_t)(uintptr_t)pool_b + 0x10; ev[1] = 0x0000000700000001ull;
    uint64_t *ext = emit(&s0, OP_REPORT_COUNTER, 16); ext[0] = 2; ext[1] = 0x12345678;   // an address outside every pool: kept as it is
    p_copy_t2t *ct = emit(&s0, OP_COPY_T2T, ((sizeof(p_copy_t2t) + 7) & ~7ul) + sizeof(tex_t)); { tex_t *in = (tex_t *)((uint8_t *)ct + ((sizeof(p_copy_t2t) + 7) & ~7ul)); *in = game_tex; in->serial = 80; ct->src = in; ct->dst = &game_tex; }
    p_dispatch *dp = emit(&s0, OP_DISPATCH, sizeof *dp); dp->x = dp->y = dp->z = 1; dp->caller = 0x7100001234; dp->ind = 0;
    uint64_t *call = emit(&s0, OP_CALL, 8); *call = (uint64_t)(uintptr_t)s1.p;
    uint64_t *jump = emit(&s0, OP_JUMP, 8); *jump = (uint64_t)(uintptr_t)s2.p;
    p_clear_buf *cb = emit(&s1, OP_CLEAR_BUF, sizeof *cb); *cb = (p_clear_buf){(uint64_t)(uintptr_t)pool_a + 0x8000, 0x100, 0xdeadbeef, 0};
    emit(&s1, OP_END, 0);
    uint64_t *fs = emit(&s2, OP_FENCE_SYNC, 8); *fs = (uint64_t)(uintptr_t)&game_sync;
    p_render_cond *rc = emit(&s2, OP_RENDER_COND, sizeof *rc); rc->mode = 1; rc->addr = (uint64_t)(uintptr_t)pool_b + 0x2000;
    emit(&s2, OP_END, 0);

    // capture: regions (pool a, pool b; the heap words join as a blob region), encoded copies of the segments, the objects
    regs[0] = (rg){(uint64_t)(uintptr_t)pool_a, 65536}; regs[1] = (rg){(uint64_t)(uintptr_t)pool_b, 16384}; nregs = 2;
    seg *all[3] = {&s0, &s1, &s2}; uint8_t enc_bytes[3][4096];
    for (int i = 0; i < 3; i++) { memcpy(enc_bytes[i], all[i]->p, all[i]->n); walk(all[i], enc_bytes[i], &enc, all[i]->p); }
    CHECK(nregs == 3, "the deferred handles outside the pools became a region of their own");
    CHECK(nobj[CAP_K_TEX] == 1 && nobj[CAP_K_PROG] == 1 && nobj[CAP_K_DPOOL] == 1 && nobj[CAP_K_SYNC] == 1, "each object interned once");
    { uint64_t f; memcpy(&f, enc_bytes[0] + ((uint8_t *)&ext[1] - s0.p), 8); CHECK(f == 0x12345678, "an address outside every region stays as it is"); }
    { uint64_t f; memcpy(&f, enc_bytes[0] + ((uint8_t *)&dp->caller - s0.p), 8); CHECK(f == 0x7100001234, "a dispatch's caller (code address) is not relocated"); }
    int host_ptrs = 0;   // no host pointer to the game's memory may stay in an encoded segment
    for (int i = 0; i < 3; i++) for (size_t o = 0; o + 8 <= all[i]->n; o += 8) { uint64_t v; memcpy(&v, enc_bytes[i] + o, 8);
        for (int r = 0; r < 2; r++) host_ptrs += v >= regs[r].lo && v < regs[r].lo + regs[r].size;
        host_ptrs += v == (uint64_t)(uintptr_t)&game_tex || v == (uint64_t)(uintptr_t)&game_prog || v == (uint64_t)(uintptr_t)s1.p || v == (uint64_t)(uintptr_t)s2.p; }
    CHECK(!host_ptrs, "no host pointer left in the encoded records");

    cap_writer w; CHECK(cap_open(&w, path), "open for writing");
    const char *lay = cap_layout(); cap_put(&w, CAP_LAYT, 0, lay, strlen(lay), 0);
    for (int r = 0; r < nregs; r++) { uint8_t b[sizeof(cap_regn) + 65536]; cap_regn h = {(uint32_t)r, r == 2, regs[r].lo, regs[r].size};
        memcpy(b, &h, sizeof h); memcpy(b + sizeof h, (const void *)(uintptr_t)regs[r].lo, (size_t)regs[r].size); cap_put(&w, CAP_REGN, 1, b, sizeof h + regs[r].size, 1); }
    for (int i = 0; i < 3; i++) { uint8_t b[8 + 4096]; uint32_t hd[2] = {(uint32_t)i, (uint32_t)all[i]->n}; memcpy(b, hd, 8); memcpy(b + 8, enc_bytes[i], all[i]->n); cap_put(&w, CAP_SEGM, 1, b, 8 + all[i]->n, 1); }
    { tex_t t = game_tex; cap_tex_fields(&t, &enc, NULL); cap_put(&w, CAP_TOBJ, 1, &t, sizeof t, 0); }
    uint64_t raw = w.raw, stored = w.stored;
    CHECK(cap_close(&w), "close");
    CHECK(stored < raw, "the mostly-empty pools compress (LZFSE)");

    // replay side: read the chunks, regions at fresh addresses, objects rebuilt, segments decoded
    cap_reader rd; CHECK(cap_load(&rd, path) == 1, "load");
    cap_chunk c; uint8_t *d; int nseg = 0, nreg = 0, layout_ok = 0; static uint8_t segbuf[3][4096 + 8]; size_t seglen[3] = {0};
    tex_t new_tex = {0}; pool_t new_pool = {0}; prog_t new_prog = {0}; dpool_t new_dp = {0}; sync_t new_sync = {0};
    while (cap_next(&rd, &c, &d) == 1) {
        if (c.tag == CAP_LAYT) layout_ok = c.raw == strlen(cap_layout()) && !memcmp(d, cap_layout(), c.raw);
        if (c.tag == CAP_REGN) { cap_regn h; memcpy(&h, d, sizeof h); new_base[h.index] = malloc((size_t)h.size + 64) + 32; memcpy(new_base[h.index], d + sizeof h, (size_t)h.size);
            CHECK(!memcmp(new_base[h.index], (const void *)(uintptr_t)h.old_lo, (size_t)h.size), "region bytes round-trip"); nreg++; }
        if (c.tag == CAP_SEGM) { uint32_t i, n; memcpy(&i, d, 4); memcpy(&n, d + 4, 4); memcpy(segbuf[i], d + 8, n); seglen[i] = n; new_seg[i] = segbuf[i]; nseg++; }
        if (c.tag == CAP_TOBJ) memcpy(&new_tex, d, sizeof new_tex);
        free(d);
    }
    cap_unload(&rd);
    CHECK(layout_ok && nreg == 3 && nseg == 3, "every chunk read back");
    new_pool = game_pool; new_pool.mem = new_base[0];
    new_obj[CAP_K_TEX][0] = &new_tex; new_obj[CAP_K_PROG][0] = &new_prog; new_obj[CAP_K_DPOOL][0] = &new_dp; new_obj[CAP_K_SYNC][0] = &new_sync; new_obj[CAP_K_POOL][0] = &new_pool;
    cap_tex_fields(&new_tex, &dec, NULL);
    CHECK(new_tex.mem == new_base[0] + 0x2000 && new_tex.pool == &new_pool && new_tex.serial == 77, "a texture object relocated");
    for (int i = 0; i < 3; i++) walk(all[i], segbuf[i], &dec, NULL);
    // every field: the same offset in the fresh region, or the rebuilt object
#define AT(i, ptr, T) (*(T *)(segbuf[i] + ((const uint8_t *)(ptr) - all[i]->p)))
    const p_set_rt *rt = (const p_set_rt *)(segbuf[0] + 8);
    CHECK(rt->n == 2 && rt->color[1] == &new_tex, "a texture object pointer → the rebuilt object");
    CHECK((const uint8_t *)rt->color[0] > segbuf[0] && (const uint8_t *)rt->color[0] < segbuf[0] + seglen[0], "a snapshot inside the record → the same place in the copy");
    CHECK(rt->color[0]->mem == new_base[0] + 0x2000 && rt->color[0]->pool == &new_pool && rt->color[0]->serial == 78, "the snapshot's storage relocated");
    CHECK(rt->depth->mem == new_base[0] + 0x4000 && rt->depth->serial == 79, "the depth snapshot relocated");
    CHECK(AT(0, bp, p_bind_program).prog == &new_prog && AT(0, bp, p_bind_program).stages == 3, "program pointer");
    CHECK(AT(0, sp, p_set_pool).pool == &new_dp, "descriptor pool pointer");
    CHECK(AT(0, vb, p_bind_vbuf).r[0].address == (uint64_t)(uintptr_t)new_base[0] + 0x100 && AT(0, vb, p_bind_vbuf).r[1].address == (uint64_t)(uintptr_t)new_base[1] + 0x40 && AT(0, vb, p_bind_vbuf).r[1].size == 0x10, "vertex buffers relocated");
    CHECK(AT(0, ub, p_bind_buf).addr == (uint64_t)(uintptr_t)new_base[1] + 0x800, "uniform buffer relocated");
    CHECK(AT(0, us, p_bind_ubo_small).addr == (uint64_t)(uintptr_t)new_base[1] + 0x1000, "compact uniform buffer relocated");
    CHECK(AT(0, bh, p_bind_handles).src == (uint64_t)(uintptr_t)new_base[2] && !memcmp(new_base[2], heap_handles, sizeof heap_handles), "deferred handles: their own region, relocated");
    CHECK(AT(0, di, p_draw_idx).ibuf == (uint64_t)(uintptr_t)new_base[0] + 0x3000 && AT(0, di, p_draw_idx).ind == (uint64_t)(uintptr_t)new_base[0] + 0x3100, "index buffer and indirect arguments");
    CHECK(AT(0, ev, uint64_t) == (uint64_t)(uintptr_t)new_base[1] + 0x10, "event word relocated");
    CHECK(AT(0, ct, p_copy_t2t).dst == &new_tex && AT(0, ct, p_copy_t2t).src->serial == 80 && AT(0, ct, p_copy_t2t).src->mem == new_base[0] + 0x2000, "texture copy ends");
    CHECK(AT(0, call, uint64_t) == (uint64_t)(uintptr_t)segbuf[1] && AT(0, jump, uint64_t) == (uint64_t)(uintptr_t)segbuf[2], "call and jump → the copied segments");
    CHECK(AT(1, cb, p_clear_buf).addr == (uint64_t)(uintptr_t)new_base[0] + 0x8000 && AT(1, cb, p_clear_buf).value == 0xdeadbeef, "buffer clear");
    CHECK(AT(2, fs, uint64_t) == (uint64_t)(uintptr_t)&new_sync, "sync object");
    CHECK(AT(2, rc, p_render_cond).addr == (uint64_t)(uintptr_t)new_base[1] + 0x2000, "render condition address");
    // every byte the walk does not name came back as it was recorded
    int other = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t m[4096] = {0}; mark_base = all[i]->p; mark = m; cap_ops mk = {mark_addr, mark_obj};   // marking only reads the original
        for (size_t o = 0; o < all[i]->n;) { op_hdr h; memcpy(&h, all[i]->p + o, 8); cap_rec_fields(h.op, all[i]->p + o + 8, h.size, &mk, NULL); o += h.size; }
        for (size_t o = 0; o < all[i]->n; o++) other += !m[o] && segbuf[i][o] != all[i]->p[o];
    }
    CHECK(!other, "bytes outside the named fields are unchanged");
    // the walk refuses what it does not know; a truncated file is reported, not read past
    uint8_t junk[16] = {0}; CHECK(!cap_rec_fields(OP_COUNT_, junk, 16, &enc, NULL), "an unknown op is refused");
    CHECK(!cap_rec_fields(OP_SET_RT, junk, 16, &enc, NULL), "a record too short for its fields is refused");
    FILE *f = fopen(path, "r+b"); fseeko(f, 0, SEEK_END); off_t n = ftello(f); fclose(f); truncate(path, n - 40);
    CHECK(cap_load(&rd, path) == 1, "a truncated capture still opens"); int r, bad = 0; while ((r = cap_next(&rd, &c, &d)) == 1) free(d); bad = r < 0; cap_unload(&rd);
    CHECK(bad, "a truncated capture ends in an error");
    unlink(path);
    printf("t_capture: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
