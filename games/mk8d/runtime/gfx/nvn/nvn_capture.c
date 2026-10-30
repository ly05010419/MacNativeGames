// NVN frame capture: the record field walk and the chunk file (see nvn_capture.h). Plain C, shared by the host, the replayer and the unit test.
#include "nvn_capture.h"
#include <stdlib.h>
#include <string.h>
#include <compression.h>
__asm__(".linker_option \"-lcompression\"");   // libcompression (LZFSE) links itself: the host, the iOS app and the tests that compile this file need no link flag

// The walk below names every pointer field of every payload by its offset. A change to a payload struct in nvn_obj.h must come here too: these asserts stop the build
// instead of letting a capture carry a host pointer or relocate the wrong bytes. The numbers are the layout the walk was written against.
#define CAP_SIZE(T, n) _Static_assert(sizeof(T) == (n), "nvn_capture.c: " #T " changed size: update cap_rec_fields and cap_layout")
#define CAP_OFF(T, f, n) _Static_assert(offsetof(T, f) == (n), "nvn_capture.c: " #T "." #f " moved: update cap_rec_fields and cap_layout")
CAP_SIZE(tex_t, 128); CAP_OFF(tex_t, mem, 88); CAP_OFF(tex_t, pool, 96); CAP_OFF(tex_t, serial, 120);
CAP_SIZE(pool_t, 40); CAP_SIZE(dpool_t, 24); CAP_SIZE(prog_t, 112); CAP_SIZE(sync_t, 16); CAP_SIZE(samp_par, 40); CAP_SIZE(view_t, 16);
CAP_SIZE(p_set_rt, 240); CAP_OFF(p_set_rt, n, 0); CAP_OFF(p_set_rt, color, 8); CAP_OFF(p_set_rt, depth, 208);
CAP_SIZE(p_bind_program, 16); CAP_OFF(p_bind_program, prog, 0);
CAP_SIZE(NVNbufferRange, 16); CAP_SIZE(p_bind_vbuf, 264); CAP_OFF(p_bind_vbuf, n, 4); CAP_OFF(p_bind_vbuf, r, 8);
CAP_SIZE(p_bind_vbuf1, 16); CAP_OFF(p_bind_vbuf1, size, 4); CAP_OFF(p_bind_vbuf1, addr, 8);
CAP_SIZE(p_bind_buf, 24); CAP_OFF(p_bind_buf, addr, 8); CAP_OFF(p_bind_buf, size, 16);
CAP_SIZE(p_bind_ubo_small, 16); CAP_OFF(p_bind_ubo_small, size, 4); CAP_OFF(p_bind_ubo_small, addr, 8);
CAP_SIZE(p_bind_handles, 280); CAP_OFF(p_bind_handles, n, 8); CAP_OFF(p_bind_handles, defer, 12); CAP_OFF(p_bind_handles, src, 16);
CAP_SIZE(p_bind_tex1, 24);
CAP_SIZE(p_draw, 48); CAP_OFF(p_draw, ind, 24); CAP_OFF(p_draw, mdi_cnt, 32);
CAP_SIZE(p_draw_idx, 56); CAP_OFF(p_draw_idx, ibuf, 24); CAP_OFF(p_draw_idx, ind, 32); CAP_OFF(p_draw_idx, mdi_cnt, 40);
CAP_SIZE(p_dispatch, 32); CAP_OFF(p_dispatch, ind, 24);
CAP_SIZE(p_set_pool, 8); CAP_OFF(p_set_pool, pool, 0);
CAP_SIZE(p_copy_b2t, 64); CAP_OFF(p_copy_b2t, src, 0); CAP_OFF(p_copy_b2t, tex, 8);
CAP_SIZE(p_copy_t2t, 104); CAP_OFF(p_copy_t2t, src, 0); CAP_OFF(p_copy_t2t, dst, 8);
CAP_SIZE(p_clear_tex, 72); CAP_OFF(p_clear_tex, tex, 0);
CAP_SIZE(p_clear_buf, 24); CAP_OFF(p_clear_buf, addr, 0); CAP_OFF(p_clear_buf, size, 8);
CAP_SIZE(p_tf_bind, 24); CAP_OFF(p_tf_bind, addr, 8); CAP_OFF(p_tf_bind, size, 16);
CAP_SIZE(p_tf_state, 16); CAP_OFF(p_tf_state, ctl, 8);
CAP_SIZE(p_render_cond, 16); CAP_OFF(p_render_cond, mode, 0); CAP_OFF(p_render_cond, addr, 8);
_Static_assert(OP_COUNT_ == 51, "nvn_capture.c: a record op was added or removed: give it a case in cap_rec_fields");

const char *cap_layout(void) {
    // sizes and pointer offsets as the walk knows them (the asserts above hold them to nvn_obj.h); bump this text with any change to the walk
    return "format 1\nops 51\n"
           "tex_t 128 mem 88 pool 96 serial 120\npool_t 40\ndpool_t 24\nprog_t 112\nsync_t 16\nsamp_par 40\nview_t 16\n"
           "p_set_rt 240 color 8 depth 208\np_bind_program 16\np_bind_vbuf 264 r 8\np_bind_vbuf1 16 addr 8\np_bind_buf 24 addr 8\np_bind_ubo_small 16 addr 8\n"
           "p_bind_handles 280 src 16\np_bind_tex1 24\np_draw 48 ind 24 mdi_cnt 32\np_draw_idx 56 ibuf 24 ind 32 mdi_cnt 40\np_dispatch 32 ind 24\np_set_pool 8\n"
           "p_copy_b2t 64 src 0 tex 8\np_copy_t2t 104 src 0 dst 8\np_clear_tex 72 tex 0\np_clear_buf 24 addr 0\np_tf_bind 24 addr 8\np_tf_state 16 ctl 8\np_render_cond 16 addr 8\n"
           "signal_event 16 val 0\nreport_counter 16 addr 8\ncopy_b2b 24 src 0 dst 8\nupdate_ub dst 0\nfence_sync 8 sync 0\njump 8\ncall 8\n";
}

// One 8-byte field at byte offset off of the payload, present only when the record is long enough to hold it
#define HAS(off) ((uint64_t)(off) + 8 <= plen)
#define F(off) ((uint64_t *)(payload + (off)))   // records are 8-byte aligned and so are these fields
static uint64_t u64_at(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int32_t i32_at(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

int cap_tex_fields(tex_t *t, const cap_ops *ops, void *ctx) {
    uint64_t *mem = (uint64_t *)((uint8_t *)t + offsetof(tex_t, mem)), *pool = (uint64_t *)((uint8_t *)t + offsetof(tex_t, pool));
    // the texture's storage: its whole extent when the layout knows it (a linear or buffer texture is read in place)
    int ok = ops->addr(ctx, mem, t->L.size, 0);
    return ops->obj(ctx, CAP_K_POOL, pool, NULL, 0) && ok;
}

int cap_rec_fields(uint32_t op, uint8_t *payload, uint32_t size, const cap_ops *ops, void *ctx) {
    uint64_t plen = size >= 8 ? size - 8 : 0; int ok = 1;
    switch (op) {
        case OP_END: case OP_BEGIN: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_STATE: case OP_BIND_VATTR:
        case OP_BIND_VSTREAM: case OP_BIND_TEX1: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BARRIER: case OP_COPY_STRIDE: case OP_BLEND_COLOR: case OP_RESET_COUNTER:
        case OP_DEPTH_CLAMP: case OP_RAST_DISCARD: case OP_VP_SWIZZLE: case OP_PRIM_RESTART:
            return 1;
        case OP_JUMP: case OP_CALL: return HAS(0) && ops->obj(ctx, CAP_K_SEG, F(0), payload, (uint32_t)plen);
        case OP_SIGNAL_EVENT: return HAS(0) && ops->addr(ctx, F(0), 4, 1);   // volatile uint32_t *val, mode, value
        case OP_REPORT_COUNTER: return HAS(8) && ops->addr(ctx, F(8), 16, 1);  // type, address of the 16-byte report
        case OP_COPY_B2B: { if (!HAS(16)) return 0; uint64_t n = u64_at(payload + 16); return ops->addr(ctx, F(0), n, 0) & ops->addr(ctx, F(8), n, 1); }
        case OP_UPDATE_UB: { if (!HAS(8)) return 0; return ops->addr(ctx, F(0), u64_at(payload + 8), 1); }   // dst, n, inline data
        case OP_FENCE_SYNC: return HAS(0) && ops->obj(ctx, CAP_K_SYNC, F(0), payload, (uint32_t)plen);
        case OP_SET_RT: {
            int32_t n = HAS(0) ? i32_at(payload) : -1; if (n < 0 || n > 8 || !HAS(offsetof(p_set_rt, depth))) return 0;
            for (int i = 0; i < n; i++) ok &= ops->obj(ctx, CAP_K_TEX, F(offsetof(p_set_rt, color) + 8 * (size_t)i), payload, (uint32_t)plen);
            return ok & ops->obj(ctx, CAP_K_TEX, F(offsetof(p_set_rt, depth)), payload, (uint32_t)plen);
        }
        case OP_BIND_PROGRAM: return HAS(0) && ops->obj(ctx, CAP_K_PROG, F(0), payload, (uint32_t)plen);
        case OP_BIND_VBUF: {
            int32_t n = HAS(0) ? i32_at(payload + 4) : -1; if (n < 0 || n > 16) return 0;
            for (int i = 0; i < n; i++) { size_t o = offsetof(p_bind_vbuf, r) + 16 * (size_t)i; if (!HAS(o + 8)) return 0; ok &= ops->addr(ctx, F(o), u64_at(payload + o + 8), 0); }
            return ok;
        }
        case OP_BIND_VBUF1: { if (!HAS(8)) return 0; uint32_t n; memcpy(&n, payload + 4, 4); return ops->addr(ctx, F(8), n, 0); }
        case OP_BIND_UBO: case OP_BIND_SSBO: { if (!HAS(16)) return 0; return ops->addr(ctx, F(8), u64_at(payload + 16), op == OP_BIND_SSBO); }
        case OP_BIND_UBO_SMALL: { if (!HAS(8)) return 0; uint32_t n; memcpy(&n, payload + 4, 4); return ops->addr(ctx, F(8), n, 0); }
        case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: {   // deferred handles are read from src at replay: the 8 * n bytes there are part of the frame's input
            if (!HAS(16)) return 0;
            int32_t n = i32_at(payload + 8), defer = i32_at(payload + 12);
            if (!defer || !u64_at(payload + 16)) return 1;
            return ops->addr(ctx, F(16), n > 0 && n <= 32 ? 8 * (uint64_t)n : 0, 0);
        }
        case OP_DRAW: if (HAS(24)) ok &= ops->addr(ctx, F(24), 16, 0); if (HAS(32)) ok &= ops->addr(ctx, F(32), 4, 0); return ok;
        case OP_DRAW_IDX: if (!HAS(24)) return 0; ok &= ops->addr(ctx, F(24), 0, 0); if (HAS(32)) ok &= ops->addr(ctx, F(32), 20, 0); if (HAS(40)) ok &= ops->addr(ctx, F(40), 4, 0); return ok;
        case OP_DISPATCH: return !HAS(24) || ops->addr(ctx, F(24), 12, 0);   // caller (+8) is a code address, kept as it is
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: return HAS(0) && ops->obj(ctx, CAP_K_DPOOL, F(0), payload, (uint32_t)plen);
        case OP_COPY_B2T: case OP_COPY_T2B: {   // src: the buffer address (the destination for T2B)
            if (!HAS(8)) return 0;
            return ops->addr(ctx, F(0), 0, op == OP_COPY_T2B) & ops->obj(ctx, CAP_K_TEX, F(8), payload, (uint32_t)plen);
        }
        case OP_COPY_T2T: if (!HAS(8)) return 0; return ops->obj(ctx, CAP_K_TEX, F(0), payload, (uint32_t)plen) & ops->obj(ctx, CAP_K_TEX, F(8), payload, (uint32_t)plen);
        case OP_CLEAR_TEX: return HAS(0) && ops->obj(ctx, CAP_K_TEX, F(0), payload, (uint32_t)plen);
        case OP_CLEAR_BUF: return HAS(8) && ops->addr(ctx, F(0), u64_at(payload + 8), 1);
        case OP_TF_BIND: return HAS(16) && ops->addr(ctx, F(8), u64_at(payload + 16), 1);
        case OP_TF_STATE: return HAS(8) && ops->addr(ctx, F(8), 32, 1);
        case OP_RENDER_COND: if (!HAS(8)) return 0; return i32_at(payload) >= 2 || ops->addr(ctx, F(8), 32, 0);
    }
    return 0;
}

// ---------- Chunk file ----------
int cap_open(cap_writer *w, const char *path) {
    memset(w, 0, sizeof *w);
    if (!(w->f = fopen(path, "wb"))) return 0;
    uint32_t v[2] = {CAP_VERSION, 0};
    fwrite(CAP_MAGIC, 1, 8, w->f); fwrite(v, 4, 2, w->f);
    return 1;
}
void cap_put(cap_writer *w, uint32_t tag, uint32_t submit, const void *data, uint64_t n, int compress) {
    if (!w->f) return;
    cap_chunk c = {tag, CAP_RAW, n, n, submit, 0};
    uint8_t *z = NULL;
    if (compress && n >= 256) {
        size_t cap = (size_t)n + n / 16 + 4096;
        if ((z = malloc(cap))) {
            size_t zn = compression_encode_buffer(z, cap, data, (size_t)n, NULL, COMPRESSION_LZFSE);
            if (zn && zn < n) { c.codec = CAP_LZFSE; c.stored = zn; } else { free(z); z = NULL; }
        }
    }
    if (fwrite(&c, sizeof c, 1, w->f) != 1 || (c.stored && fwrite(z ? z : data, 1, (size_t)c.stored, w->f) != c.stored)) w->err = 1;
    w->raw += n; w->stored += c.stored + sizeof c;
    free(z);
}
int cap_close(cap_writer *w) {
    if (!w->f) return 0;
    cap_put(w, CAP_END_, 0, NULL, 0, 0);
    int ok = !w->err && fclose(w->f) == 0; w->f = NULL; return ok;
}
int cap_load(cap_reader *r, const char *path) {
    memset(r, 0, sizeof *r);
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseeko(f, 0, SEEK_END); off_t n = ftello(f); fseeko(f, 0, SEEK_SET);
    if (n < 16 || !(r->buf = malloc((size_t)n)) || fread(r->buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(r->buf); r->buf = NULL; return 0; }
    fclose(f); r->n = (size_t)n;
    uint32_t ver; memcpy(&ver, r->buf + 8, 4);
    if (memcmp(r->buf, CAP_MAGIC, 8) || ver != CAP_VERSION) { free(r->buf); r->buf = NULL; return -1; }
    r->pos = 16; return 1;
}
int cap_next(cap_reader *r, cap_chunk *c, uint8_t **data) {
    *data = NULL;
    if (r->pos + sizeof *c > r->n) return -1;
    memcpy(c, r->buf + r->pos, sizeof *c); r->pos += sizeof *c;
    if (c->stored > r->n - r->pos) return -1;
    if (c->tag == CAP_END_) return 0;
    uint8_t *d = malloc(c->raw ? (size_t)c->raw : 1); if (!d) return -1;
    if (c->codec == CAP_RAW) { if (c->stored != c->raw) { free(d); return -1; } memcpy(d, r->buf + r->pos, (size_t)c->raw); }
    else if (c->codec == CAP_LZFSE) { if (compression_decode_buffer(d, (size_t)c->raw, r->buf + r->pos, (size_t)c->stored, NULL, COMPRESSION_LZFSE) != c->raw) { free(d); return -1; } }
    else { free(d); return -1; }
    r->pos += c->stored; *data = d; return 1;
}
void cap_unload(cap_reader *r) { free(r->buf); memset(r, 0, sizeof *r); }
void cap_buf_put(cap_buf *b, const void *data, size_t n) {
    if (b->n + n > b->cap) { size_t c = b->cap ? b->cap : 4096; while (c < b->n + n) c *= 2; b->p = realloc(b->p, c); b->cap = c; }
    memcpy(b->p + b->n, data, n); b->n += n;
}
