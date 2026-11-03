// DM_NVN_CAPTURE=<frame>:1:<path>: capture one frame at the replay boundary (nvnQueueSubmitCommands → mtl_exec) into one file, for dm/gfx/tests/nvn_replay to replay against
// the current replay code. Included at the end of nvn_mtl.m: it reads the replay layer's tables (pools, texture and sampler registries, host textures). File format: nvn_capture.h.
//
// From the frame's first submit until its present: every live pool's bytes, the registries, the programs' stage keys, the contents of every texture whose content lives only
// in Metal that the frame can reach (registered or named by a record), and each submission's record bytes with pointers written as region offsets and object indices. Before
// each later submit of the frame, the pool bytes that changed (GPU writes included) and the registry entries that changed. At the present: the presented texture's SHA-256,
// read back the same way the replayer reads its own (cap_digest). Each capture step first waits for the GPU (mtl_finish), so what is read is what the next submit sees; that
// changes the frame's timing, not what it renders. Unset, the hooks cost one load and a branch per submit and present.
#include "nvn_capture.h"
#include <crt_externs.h>

static int g_cap_frame = -1;            // -1: not parsed yet, 0: off
static char g_cap_path[1024];
static int g_cap_active, g_cap_done, g_cap_failed;
static uint32_t g_cap_submit;           // submits captured in the frame so far
static uint64_t g_cap_presents;         // presents seen (the frame number DM_NVN_SHOT uses)
static cap_writer g_cap_w;
static uint64_t g_cap_t0, g_cap_ns, g_cap_unmapped, g_cap_segs, g_cap_tex_bytes, g_cap_tex_n, g_cap_tex_skip;
typedef struct { uint64_t lo, size; uint32_t kind; uint64_t *hash; } cap_region;   // hash: one per 4 KiB block, for the changes before later submits
static cap_region *g_cap_reg; static uint32_t g_cap_nreg;
static nvn_hmap g_cap_tex = {.klen = sizeof(tex_t)}, g_cap_prog = {.klen = 16}, g_cap_dpol = {.klen = 8 + sizeof(dpool_t)}, g_cap_sync = {.klen = 8}, g_cap_pool = {.klen = 8 + sizeof(pool_t)};
static nvn_hmap g_cap_seg = {.klen = 8}, g_cap_read = {.klen = 8};
static int g_cap_no_read;   // at the present: the frame is rendered, nothing may be read back as an input any more
static uint32_t g_cap_ntex, g_cap_nprog, g_cap_ndpol, g_cap_nsync, g_cap_npool;
static cap_buf g_cap_tobj, g_cap_pobj, g_cap_progb, g_cap_dpolb, g_cap_kind;   // objects interned since the last flush
static treg_t *g_cap_treg0; static int g_cap_ntreg0; static samp_par *g_cap_sreg0; static uint8_t *g_cap_sreg0_ok; static int g_cap_nsreg0;   // the registries as last written

static int nvn_cap_enabled(void) {
    if (g_cap_frame >= 0) return g_cap_frame > 0 && !g_cap_done;
    g_cap_frame = 0;
    const char *e = getenv("DM_NVN_CAPTURE"); int f = 0, n = 0, used = 0;
    if (!e || !*e) return 0;
    if (sscanf(e, "%d:%d:%n", &f, &n, &used) < 2 || !used || f < 1 || !e[used]) { MLOG("[RED] DM_NVN_CAPTURE=%s: expected <frame>:1:<path>; capture off", e); return 0; }
    if (n != 1) MLOG("[RED] DM_NVN_CAPTURE: %d frames asked, step 1 captures one; capturing frame %d only", n, f);
    strlcpy(g_cap_path, e + used, sizeof g_cap_path); g_cap_frame = f;
    MLOG("DM_NVN_CAPTURE: capturing frame %d into %s", f, g_cap_path);
    return 1;
}

static uint64_t cap_hash_block(const uint8_t *p, size_t n) {   // a fast 64-bit hash of a 4 KiB block (only compared with itself between submits)
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, v; size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); h = (h ^ v) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static void cap_fail(const char *why) { if (!g_cap_failed++) MLOG("[RED] DM_NVN_CAPTURE: %s: the capture is incomplete", why); }

// ---------- Regions and the address encoding ----------
static int cap_reg_find(uint64_t a) {   // the region holding address a (regions are sorted and disjoint)
    int lo = 0, hi = (int)g_cap_nreg - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < g_cap_reg[m].lo) hi = m - 1; else if (a >= g_cap_reg[m].lo + g_cap_reg[m].size) lo = m + 1; else return m; }
    return -1;
}
static void cap_reg_hash(cap_region *r) {
    size_t nb = (size_t)((r->size + 4095) / 4096); r->hash = realloc(r->hash, nb * 8);
    for (size_t b = 0; b < nb; b++) { uint64_t o = (uint64_t)b * 4096; r->hash[b] = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, (size_t)MIN(4096, r->size - o)); }
}
// Regions are written once their index is fixed; a later region gets the next index, so the sorted order is kept in a separate index table
static uint32_t *g_cap_rid; static uint32_t g_cap_nrid;   // g_cap_rid[sorted position] = region number in the file
static int cap_reg_add(uint64_t lo, uint64_t size, uint32_t kind, uint32_t submit) {   // a new disjoint region; returns its sorted position
    int pos = 0; while (pos < (int)g_cap_nreg && g_cap_reg[pos].lo < lo) pos++;
    g_cap_reg = realloc(g_cap_reg, (g_cap_nreg + 1) * sizeof *g_cap_reg); g_cap_rid = realloc(g_cap_rid, (g_cap_nreg + 1) * sizeof *g_cap_rid);
    memmove(g_cap_reg + pos + 1, g_cap_reg + pos, (g_cap_nreg - pos) * sizeof *g_cap_reg); memmove(g_cap_rid + pos + 1, g_cap_rid + pos, (g_cap_nreg - pos) * sizeof *g_cap_rid);
    g_cap_reg[pos] = (cap_region){lo, size, kind, NULL}; g_cap_rid[pos] = g_cap_nrid++; g_cap_nreg++;
    uint32_t file_no = g_cap_rid[pos]; cap_region *r = &g_cap_reg[pos]; cap_regn hh = {file_no, r->kind, r->lo, r->size};
    uint8_t *b = malloc(sizeof hh + r->size); if (!b) { cap_fail("out of memory for a region"); return pos; }
    memcpy(b, &hh, sizeof hh); memcpy(b + sizeof hh, (const void *)(uintptr_t)r->lo, (size_t)r->size);
    cap_put(&g_cap_w, CAP_REGN, submit, b, sizeof hh + r->size, 1); free(b);
    cap_reg_hash(r);
    return pos;
}
static uint32_t g_cap_cur_submit;
static int cap_enc_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; (void)writable;
    uint64_t v = *field; if (!v) return 1;
    int i = cap_reg_find(v);
    if (i < 0 && len && len <= (1u << 20) && nvn_host_readable(v, len)) {   // memory outside the pools a record reads (deferred handles, event words): a region of its own
        for (uint32_t k = 0; k < g_cap_nreg; k++) if (g_cap_reg[k].lo < v + len && v < g_cap_reg[k].lo + g_cap_reg[k].size) { cap_fail("a record's memory straddles a region"); return 1; }
        i = cap_reg_add(v, len, 1, g_cap_cur_submit);
    }
    if (i < 0) { if (g_cap_unmapped++ < 8) MLOG("[RED] DM_NVN_CAPTURE: a record names %#llx (%llu bytes), outside every pool: kept as it is (the replay resolves it as the live run does, to nothing)", (unsigned long long)v, (unsigned long long)len); return 1; }
    *field = cap_addr(g_cap_rid[i], v - g_cap_reg[i].lo);
    return 1;
}

// The interface a program's shaders were translated with, as the replay layer resolved it: per stage a hash of its bindings, vertex inputs and thread group size (the
// replayer computes the same from its own libraries and compares)
static void cap_iface(NvProg *o, uint64_t out[6]) {
    for (int st = 0; st < 6; st++) {
        NvShader *sh = o ? o->sh[st] : nil; out[st] = 0; if (!sh) continue;
        const nvn_refl *r = &sh->rf; int nb = MAX(0, MIN(r->nb, NVN_REFL_MAXB)), nv = MAX(0, MIN(r->nv, NVN_REFL_MAXV));
        CC_SHA256_CTX c; uint8_t md[32]; CC_SHA256_Init(&c);
        CC_SHA256_Update(&c, &nb, sizeof nb); CC_SHA256_Update(&c, r->b, (CC_LONG)(nb * sizeof *r->b));
        CC_SHA256_Update(&c, &nv, sizeof nv); CC_SHA256_Update(&c, r->v, (CC_LONG)(nv * sizeof *r->v)); CC_SHA256_Update(&c, r->tg, sizeof r->tg);
        CC_SHA256_Final(md, &c); memcpy(&out[st], md, 8); out[st] |= 1;
    }
}
// ---------- Objects ----------
static int cap_enc_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
static const cap_ops g_cap_enc = {cap_enc_addr, cap_enc_obj};
static void cap_tex_read(const tex_t *t, int rec);
static uint64_t cap_pool_obj(const pool_t *p) {
    if (!p) return 0;
    uint8_t k[8 + sizeof(pool_t)]; memcpy(k, &p, 8); memcpy(k + 8, p, sizeof *p);
    void *have = nvn_hm_get(&g_cap_pool, k); if (have) return cap_obj(CAP_K_POOL, (uint64_t)(uintptr_t)have - 1);
    uint32_t idx = g_cap_npool++; nvn_hm_put(&g_cap_pool, k, (void *)(uintptr_t)(idx + 1));
    cap_pobj o = {idx, p->flags, (uint64_t)(uintptr_t)p->mem, p->size, p->vmode, 0};
    if (p->vmode == 2) cap_fail("a sparse virtual pool (step 1 does not capture page tables)");
    cap_enc_addr(NULL, &o.mem, 0, 0); cap_buf_put(&g_cap_pobj, &o, sizeof o);
    return cap_obj(CAP_K_POOL, idx);
}
static uint64_t cap_tex_obj(const tex_t *src) {   // a texture snapshot (by content), relocated
    tex_t t = *src;
    void *have = nvn_hm_get(&g_cap_tex, &t); if (have) return cap_obj(CAP_K_TEX, (uint64_t)(uintptr_t)have - 1);
    uint32_t idx = g_cap_ntex++; nvn_hm_put(&g_cap_tex, &t, (void *)(uintptr_t)(idx + 1));
    cap_tex_read(&t, 1);
    cap_tex_fields(&t, &g_cap_enc, NULL);
    uint32_t hd[2] = {idx, 0}; cap_buf_put(&g_cap_tobj, hd, 8); cap_buf_put(&g_cap_tobj, &t, sizeof t);
    return cap_obj(CAP_K_TEX, idx);
}
static const uint8_t *g_cap_orig;   // the record's payload in the game's memory (the copy is walked)
static uint64_t cap_seg_get(uint64_t addr);
static int cap_enc_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)ctx; uint64_t v = *field; if (!v && kind != CAP_K_SEG) return 1;
    switch (kind) {
        case CAP_K_SEG: *field = cap_obj(CAP_K_SEG, cap_seg_get(v)); return 1;
        case CAP_K_POOL: *field = cap_pool_obj((const pool_t *)(uintptr_t)v); return 1;
        case CAP_K_TEX:
            if (g_cap_orig && v >= (uint64_t)(uintptr_t)g_cap_orig && v + sizeof(tex_t) <= (uint64_t)(uintptr_t)g_cap_orig + size) {   // the snapshot inside the record
                uint64_t off = v - (uint64_t)(uintptr_t)g_cap_orig; tex_t *in = (tex_t *)(payload + off);
                cap_tex_read(in, 1); cap_tex_fields(in, &g_cap_enc, NULL); *field = cap_obj(CAP_K_TEXIN, off); return 1;
            }
            *field = cap_tex_obj((const tex_t *)(uintptr_t)v); return 1;
        case CAP_K_PROG: {
            const prog_t *p = (const prog_t *)(uintptr_t)v; uint64_t k[2] = {v, p->serial};
            void *have = nvn_hm_get(&g_cap_prog, k); if (have) { *field = cap_obj(CAP_K_PROG, (uint64_t)(uintptr_t)have - 1); return 1; }
            uint32_t idx = g_cap_nprog++; nvn_hm_put(&g_cap_prog, k, (void *)(uintptr_t)(idx + 1));
            cap_prog o = {idx, p->nstage, p->serial, {{0}}, 0, {0}, {0}};
            for (unsigned s = 0; s < p->nstage && s < 6; s++) if (prog_key(p, s, o.key[s])) o.ok |= (uint8_t)(1u << s);
            cap_iface(prog_get(p), o.iface);
            cap_buf_put(&g_cap_progb, &o, sizeof o); *field = cap_obj(CAP_K_PROG, idx); return 1;
        }
        case CAP_K_DPOOL: {
            const dpool_t *d = (const dpool_t *)(uintptr_t)v; uint8_t k[8 + sizeof(dpool_t)]; memcpy(k, &v, 8); memcpy(k + 8, d, sizeof *d);
            void *have = nvn_hm_get(&g_cap_dpol, k); if (have) { *field = cap_obj(CAP_K_DPOOL, (uint64_t)(uintptr_t)have - 1); return 1; }
            uint32_t idx = g_cap_ndpol++; nvn_hm_put(&g_cap_dpol, k, (void *)(uintptr_t)(idx + 1));
            cap_dpol o = {idx, d->n, cap_pool_obj(d->pool), d->off, nvn_dpool_base(d, 1), nvn_dpool_base(d, 0)};
            cap_buf_put(&g_cap_dpolb, &o, sizeof o); *field = cap_obj(CAP_K_DPOOL, idx); return 1;
        }
        case CAP_K_SYNC: {
            void *have = nvn_hm_get(&g_cap_sync, &v); uint32_t idx = have ? (uint32_t)(uintptr_t)have - 1 : g_cap_nsync++;
            if (!have) nvn_hm_put(&g_cap_sync, &v, (void *)(uintptr_t)(idx + 1));
            *field = cap_obj(CAP_K_SYNC, idx); return 1;
        }
    }
    return 0;
}

// ---------- Segments ----------
static uint64_t *g_cap_segq; static uint32_t g_cap_nseg, g_cap_segq_cap;
static uint64_t cap_seg_get(uint64_t addr) {   // a segment's index in this submit (queued to be copied)
    void *have = nvn_hm_get(&g_cap_seg, &addr); if (have) return (uint64_t)(uintptr_t)have - 1;
    uint32_t idx = g_cap_nseg++; nvn_hm_put(&g_cap_seg, &addr, (void *)(uintptr_t)(idx + 1));
    if (g_cap_nseg > g_cap_segq_cap) { g_cap_segq_cap = g_cap_segq_cap ? g_cap_segq_cap * 2 : 256; g_cap_segq = realloc(g_cap_segq, g_cap_segq_cap * 8); }
    g_cap_segq[idx] = addr; return idx;
}
static void cap_seg_write(uint32_t idx) {   // copy the records from the segment start to its END or JUMP, then encode them in the copy
    const uint8_t *p = (const uint8_t *)(uintptr_t)g_cap_segq[idx]; size_t n = 0;
    for (;;) {
        op_hdr h; memcpy(&h, p + n, 8);
        if (h.size < 8 || (h.size & 7) || h.size > (64u << 20) || h.op >= OP_COUNT_) { cap_fail("a corrupt record in a submitted segment"); MLOG("[RED] DM_NVN_CAPTURE: segment %p offset %zu: op %u size %u", p, n, h.op, h.size); break; }
        n += h.size;
        if (h.op == OP_END || h.op == OP_JUMP) break;
        if (n > (256u << 20)) { cap_fail("a segment longer than 256 MiB without END"); break; }
    }
    uint8_t *b = malloc(8 + n); uint32_t hd[2] = {idx, (uint32_t)n}; memcpy(b, hd, 8); memcpy(b + 8, p, n);
    for (size_t o = 0; o < n;) {
        op_hdr h; memcpy(&h, b + 8 + o, 8);
        g_cap_orig = p + o + 8;
        if (!cap_rec_fields(h.op, b + 8 + o + 8, h.size, &g_cap_enc, NULL)) { cap_fail("a record the capture walk does not know"); MLOG("[RED] DM_NVN_CAPTURE: op %u size %u not walked", h.op, h.size); }
        o += h.size;
    }
    g_cap_orig = NULL;
    cap_put(&g_cap_w, CAP_SEGM, g_cap_cur_submit, b, 8 + n, 1); free(b); g_cap_segs++;
}

// ---------- Textures whose content lives only in Metal ----------
static int cap_pf_bpp(MTLPixelFormat f, int plane) {   // bytes per pixel of a plane (0 = not readable as plain texels)
    if (f == MTLPixelFormatDepth32Float_Stencil8) return plane ? 1 : 4;
    if (plane) return 0;
    if (f == MTLPixelFormatA8Unorm || (f >= MTLPixelFormatR8Unorm && f <= MTLPixelFormatR8Sint) || f == MTLPixelFormatStencil8) return 1;
    if ((f >= MTLPixelFormatR16Unorm && f <= MTLPixelFormatR16Float) || (f >= MTLPixelFormatRG8Unorm && f <= MTLPixelFormatRG8Sint) || (f >= MTLPixelFormatB5G6R5Unorm && f <= MTLPixelFormatBGR5A1Unorm) || f == MTLPixelFormatDepth16Unorm) return 2;
    if ((f >= MTLPixelFormatR32Uint && f <= MTLPixelFormatR32Float) || (f >= MTLPixelFormatRG16Unorm && f <= MTLPixelFormatRG16Float) || (f >= MTLPixelFormatRGBA8Unorm && f <= MTLPixelFormatRGBA8Sint)
        || f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB || (f >= MTLPixelFormatRGB10A2Unorm && f <= MTLPixelFormatBGR10A2Unorm) || f == MTLPixelFormatDepth32Float) return 4;
    if ((f >= MTLPixelFormatRG32Uint && f <= MTLPixelFormatRG32Float) || (f >= MTLPixelFormatRGBA16Unorm && f <= MTLPixelFormatRGBA16Float)) return 8;
    if (f >= MTLPixelFormatRGBA32Uint && f <= MTLPixelFormatRGBA32Float) return 16;
    return 0;
}
// bytes per block of a plane and the block's size in texels (1x1 for plain texels; BC formats 4x4); 0 = a format the capture cannot read back
static int cap_pf_block(MTLPixelFormat f, int plane, uint32_t *bw, uint32_t *bh) {
    *bw = *bh = 1;
    int b = cap_pf_bpp(f, plane); if (b || plane) return b;
    *bw = *bh = 4;
    if (f == MTLPixelFormatBC1_RGBA || f == MTLPixelFormatBC1_RGBA_sRGB || f == MTLPixelFormatBC4_RUnorm || f == MTLPixelFormatBC4_RSnorm) return 8;
    if ((f >= MTLPixelFormatBC2_RGBA && f <= MTLPixelFormatBC3_RGBA_sRGB) || f == MTLPixelFormatBC5_RGUnorm || f == MTLPixelFormatBC5_RGSnorm
        || (f >= MTLPixelFormatBC6H_RGBFloat && f <= MTLPixelFormatBC7_RGBAUnorm_sRGB)) return 16;
    *bw = *bh = 1; return 0;
}
static int cap_tex_planes(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8 ? 2 : 1; }
static MTLBlitOption cap_plane_opt(MTLPixelFormat f, int plane) { return f != MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionNone : plane ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil; }
// Read every level and slice of a texture into one shared buffer (the replayer fills its texture from the same layout); returns the subresource table
static id<MTLBuffer> cap_tex_readback(id<MTLTexture> t, cap_tsub **subs, uint32_t *nsub) {
    uint32_t slices = (uint32_t)(t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 * t.arrayLength : t.arrayLength), np = (uint32_t)cap_tex_planes(t.pixelFormat);
    *nsub = 0; *subs = calloc((size_t)t.mipmapLevelCount * slices * np, sizeof **subs); uint64_t total = 0;
    for (uint32_t pl = 0; pl < np; pl++) for (uint32_t lv = 0; lv < t.mipmapLevelCount; lv++) for (uint32_t sl = 0; sl < slices; sl++) {
        uint32_t w = (uint32_t)MAX(t.width >> lv, 1u), h = (uint32_t)MAX(t.height >> lv, 1u), d = t.textureType == MTLTextureType3D ? (uint32_t)MAX(t.depth >> lv, 1u) : 1;
        uint32_t bw, bh; uint64_t bpb = (uint64_t)cap_pf_block(t.pixelFormat, (int)pl, &bw, &bh), row = (uint64_t)((w + bw - 1) / bw) * bpb, img = row * ((h + bh - 1) / bh);
        (*subs)[(*nsub)++] = (cap_tsub){lv, sl, w, h, d, pl, row, img, img * d}; total += img * d;
    }
    id<MTLBuffer> b = [g_dev newBufferWithLength:MAX(total, 16u) options:MTLResourceStorageModeShared];
    if (!b) return nil;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder]; uint64_t off = 0;
    for (uint32_t i = 0; i < *nsub; i++) { cap_tsub *s = &(*subs)[i];
        [e copyFromTexture:t sourceSlice:s->slice sourceLevel:s->level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(s->w, s->h, s->d) toBuffer:b destinationOffset:off
            destinationBytesPerRow:s->row destinationBytesPerImage:s->img options:cap_plane_opt(t.pixelFormat, (int)s->plane)];
        off += s->bytes; }
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return b;
}
static nvn_hmap g_cap_mtex = {.klen = 8};   // MTLTextures already read (several serials can share one)
static void cap_tex_read(const tex_t *t, int rec) {   // rec: named by a record (a target, a copy or clear end), not only registered   // the content of a texture the frame can reach, if it lives only in Metal (a render target, a copy destination): read it now, before this submit runs
    if (g_cap_no_read || !t || !t->serial || t->magic != MAGIC('T', 'E', 'X', 0)) return;
    uintptr_t seen = (uintptr_t)nvn_hm_get(&g_cap_read, &t->serial);   // 1: done; 2: registered without a host texture (a record naming it later still looks for shared storage)
    if (seen == 1 || (seen == 2 && !rec)) return;
    id o = tex_get(t->serial);
    if (!o) {   // no host texture for this serial yet: its first use takes the host texture of an earlier one with the same storage and shape, content included (tex_mtl,
                // the storage-sharing table). Breath of the Wild records its transient render targets under new serials every frame and registers them to sample them;
                // their content from the frame before was not captured, the replay made them empty, and the G-buffer differed from the live run in about 50 pixels (and
                // a second replay from the first). Only for textures a record names: through the registry (even only those the GPU writes, or
                // only those shared within the last 16 submissions) it reached about 19,500 textures (18.5 GB) in Breath of the Wild
        alias_key ak; memset(&ak, 0, sizeof ak); ak.pool = (uint64_t)(uintptr_t)t->pool; ak.off = t->off; ak.d = t->d; ak.d.flags &= ~NVN_TEXTURE_FLAGS_IMAGE;
        if (rec) { pthread_mutex_lock(&g_smu); o = hm_get_id(&g_alias, &ak); pthread_mutex_unlock(&g_smu); }
    }
    nvn_hm_put(&g_cap_read, &t->serial, (void *)(uintptr_t)(!o && !rec ? 2 : 1));
    if (!seen) {   // how the replay layer holds it
        pthread_mutex_lock(&g_smu); int live = nvn_hm_get(&g_tex_live, &t->serial) != NULL; pthread_mutex_unlock(&g_smu);
        int cpu = tex_cpu_written(t->serial) || ([o isKindOfClass:[NvPkg class]] && !t->has_layout);
        cap_tknd k = {*t, (live ? CAP_TK_LIVE : 0) | (cpu ? CAP_TK_CPU : 0), 0};
        if (k.kind) { cap_tex_fields(&k.t, &g_cap_enc, NULL); cap_buf_put(&g_cap_kind, &k, sizeof k); }
    }
    if (!o || [o isKindOfClass:[NvPkg class]] || ![o conformsToProtocol:@protocol(MTLTexture)]) return;   // made at first use (as live, from the pools), or uploaded from the pools
    id<MTLTexture> mt = o; if (mt.parentTexture) mt = mt.parentTexture;
    if (mt.buffer || mt.storageMode == MTLStorageModeMemoryless) return;   // reads pool memory in place, or has no content
    uint64_t key = (uint64_t)(uintptr_t)(__bridge void *)mt; int again = nvn_hm_get(&g_cap_mtex, &key) != NULL;
    uint32_t bw_, bh_;
    if (mt.sampleCount > 1 || !cap_pf_block(mt.pixelFormat, 0, &bw_, &bh_)) {
        if (!again) { g_cap_tex_skip++; if (g_cap_tex_skip <= 8) MLOG("[RED] DM_NVN_CAPTURE: texture ser=%llu %lux%lu format %lu samples %lu: content not captured (%s)", (unsigned long long)t->serial,
            (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)mt.pixelFormat, (unsigned long)mt.sampleCount, mt.sampleCount > 1 ? "multisampled" : "format the capture cannot read back"); nvn_hm_put(&g_cap_mtex, &key, (void *)1); }
        return;
    }
    nvn_hm_put(&g_cap_mtex, &key, (void *)1);
    cap_tsub *subs; uint32_t nsub; id<MTLBuffer> b = cap_tex_readback(mt, &subs, &nsub);
    if (!b) { free(subs); cap_fail("a texture readback buffer could not be made"); return; }
    cap_tpix h = {*t, (uint32_t)mt.pixelFormat, (uint32_t)mt.textureType, (uint32_t)mt.width, (uint32_t)mt.height, (uint32_t)mt.depth, (uint32_t)mt.mipmapLevelCount,
                  (uint32_t)(mt.textureType == MTLTextureTypeCube || mt.textureType == MTLTextureTypeCubeArray ? 6 * mt.arrayLength : mt.arrayLength), (uint32_t)mt.sampleCount, nsub, 0};
    cap_tex_fields(&h.t, &g_cap_enc, NULL);
    cap_buf c = {0}; cap_buf_put(&c, &h, sizeof h); cap_buf_put(&c, subs, nsub * sizeof *subs);
    uint64_t total = 0; for (uint32_t i = 0; i < nsub; i++) total += subs[i].bytes;
    cap_buf_put(&c, b.contents, (size_t)total);
    cap_put(&g_cap_w, CAP_TPIX, g_cap_cur_submit, c.p, c.n, 1);
    g_cap_tex_bytes += total; g_cap_tex_n++;
    cap_buf_free(&c); free(subs);
}

// ---------- Registries ----------
static void cap_registries(uint32_t submit) {   // the entries that differ from the last written ones (all of them at submit 1)
    cap_buf tb = {0}, sb = {0}; tex_t *reads = NULL; int nreads = 0;
    os_unfair_lock_lock(&g_rmu);
    if (g_cap_ntreg0 < g_ntreg) { g_cap_treg0 = realloc(g_cap_treg0, g_ntreg * sizeof *g_cap_treg0); memset(g_cap_treg0 + g_cap_ntreg0, 0, (g_ntreg - g_cap_ntreg0) * sizeof *g_cap_treg0); g_cap_ntreg0 = g_ntreg; }
    reads = malloc((size_t)MAX(g_ntreg, 1) * sizeof *reads);
    for (int i = 0; i < g_ntreg; i++) {
        treg_t *r = &g_treg[i], *o = &g_cap_treg0[i];
        if (r->ok == o->ok && (!r->ok || (r->has_v == o->has_v && !memcmp(&r->t, &o->t, sizeof r->t) && (!r->has_v || !memcmp(&r->v, &o->v, sizeof r->v))))) continue;
        cap_treg e = {i, (uint8_t)r->ok, (uint8_t)r->has_v, {0}, r->t, r->v}; o->ok = r->ok; o->has_v = r->has_v; o->t = r->t; o->v = r->v;
        if (r->ok) reads[nreads++] = r->t;
        cap_buf_put(&tb, &e, sizeof e);
    }
    if (g_cap_nsreg0 < g_nsreg) { g_cap_sreg0 = realloc(g_cap_sreg0, g_nsreg * sizeof *g_cap_sreg0); g_cap_sreg0_ok = realloc(g_cap_sreg0_ok, g_nsreg); memset(g_cap_sreg0_ok + g_cap_nsreg0, 0, g_nsreg - g_cap_nsreg0); g_cap_nsreg0 = g_nsreg; }
    for (int i = 0; i < g_nsreg; i++) {
        if (!g_sreg_ok[i] || (g_cap_sreg0_ok[i] && !memcmp(&g_sreg[i], &g_cap_sreg0[i], sizeof g_sreg[i]))) continue;
        cap_sreg e = {i, 0, g_sreg[i]}; g_cap_sreg0[i] = g_sreg[i]; g_cap_sreg0_ok[i] = 1; cap_buf_put(&sb, &e, sizeof e);
    }
    os_unfair_lock_unlock(&g_rmu);
    for (size_t o = 0; o < tb.n; o += sizeof(cap_treg)) { cap_treg *e = (cap_treg *)(tb.p + o); if (e->ok) cap_tex_fields(&e->t, &g_cap_enc, NULL); }
    for (int i = 0; i < nreads; i++) cap_tex_read(&reads[i], 0);   // outside the registry lock: q_cb and the readback take other locks
    free(reads);
    if (tb.n) cap_put(&g_cap_w, CAP_TREG, submit, tb.p, tb.n, 1);
    if (sb.n) cap_put(&g_cap_w, CAP_SREG, submit, sb.p, sb.n, 1);
    cap_buf_free(&tb); cap_buf_free(&sb);
}

// ---------- The frame ----------
static void cap_head(void) {
    cap_buf h = {0}; char line[2048];
#define KV(...) do { int n_ = snprintf(line, sizeof line, __VA_ARGS__); cap_buf_put(&h, line, (size_t)MIN(n_, (int)sizeof line - 1)); } while (0)
    char exe[1024]; uint32_t en = sizeof exe; char commit[128] = "unknown";
    if (!_NSGetExecutablePath(exe, &en)) { char cmd[1400], *d = dirname(exe); snprintf(cmd, sizeof cmd, "git -C '%s' describe --always --dirty --abbrev=12 2>/dev/null", d);
        FILE *p = popen(cmd, "r"); if (p) { if (fgets(commit, sizeof commit, p)) commit[strcspn(commit, "\n")] = 0; else strlcpy(commit, "unknown", sizeof commit); pclose(p); } }
    KV("format\t%u\n", CAP_VERSION); KV("chain\t%s\n", DMSH_CHAIN); KV("commit\t%s\n", commit); KV("title\t%s\n", DM_GAME_ID); KV("build\t%s\n", dm_main_build());
    KV("frame\t%d\n", g_cap_frame); KV("frames\t1\n"); KV("mtl_frame\t%llu\n", (unsigned long long)atomic_load(&g_cur_frame));   // the replay layer's frame number: the replayer takes it, so frame-keyed diagnostics (DM_NVN_TEXDUMP, DM_NVN_PASSDUMP) select the same frame
    for (char **e = *_NSGetEnviron(); *e; e++) if (!strncmp(*e, "DM_", 3) && strncmp(*e, "DM_NVN_CAPTURE=", 15)) KV("env\t%s\n", *e);
#undef KV
    cap_put(&g_cap_w, CAP_HEAD, 0, h.p, h.n, 0); cap_buf_free(&h);
    const char *lay = cap_layout(); cap_put(&g_cap_w, CAP_LAYT, 0, lay, strlen(lay), 0);
    snprintf(line, sizeof line, "tex_pool_n\t%d\norigin_ll\t%d\nqueue_origin_ll\t%d\ndepth_zero\t%d\n", g_tpool_n, atomic_load(&g_origin_ll), g_queue_origin_ll, atomic_load(&g_depth_zero));
    cap_put(&g_cap_w, CAP_STAT, 0, line, strlen(line), 0);
}
static void cap_pools(uint32_t submit) {   // regions for pools not covered yet (all of them at submit 1), and the replay layer's pool list
    pthread_mutex_lock(&g_mu);
    int n = g_npool; pool_ent *ps = malloc((size_t)MAX(n, 1) * sizeof *ps); memcpy(ps, g_pools, (size_t)n * sizeof *ps);
    pthread_mutex_unlock(&g_mu);
    qsort_b(ps, (size_t)n, sizeof *ps, ^int(const void *a, const void *b) { uintptr_t x = ((const pool_ent *)a)->lo, y = ((const pool_ent *)b)->lo; return x < y ? -1 : x > y; });
    for (int i = 0; i < n;) {   // overlapping pools make one region
        uint64_t lo = ps[i].lo, hi = ps[i].hi; int j = i + 1;
        while (j < n && ps[j].lo < hi) { hi = MAX(hi, ps[j].hi); j++; }
        int k = cap_reg_find(lo);
        if (k < 0 || g_cap_reg[k].lo + g_cap_reg[k].size < hi) {
            if (k >= 0 || cap_reg_find(hi - 1) >= 0) cap_fail("a pool grew over an existing region during the frame");
            else cap_reg_add(lo, hi - lo, 0, submit);
        }
        i = j;
    }
    cap_buf b = {0};
    for (int i = 0; i < n; i++) { cap_padd a = {ps[i].lo, ps[i].hi - ps[i].lo}; cap_enc_addr(NULL, &a.mem, 0, 0); cap_buf_put(&b, &a, sizeof a); }
    cap_put(&g_cap_w, CAP_PADD, submit, b.p, b.n, 1); cap_buf_free(&b); free(ps);
}
static void cap_diffs(uint32_t submit) {   // the 4 KiB blocks of every region that changed since they were last written
    cap_buf b = {0};
    for (uint32_t i = 0; i < g_cap_nreg; i++) {
        cap_region *r = &g_cap_reg[i]; size_t nb = (size_t)((r->size + 4095) / 4096);
        for (size_t k = 0; k < nb;) {
            uint64_t o = (uint64_t)k * 4096; size_t len = (size_t)MIN(4096, r->size - o);
            uint64_t h = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, len);
            if (h == r->hash[k]) { k++; continue; }
            size_t k1 = k; uint64_t end = o;
            while (k1 < nb) { uint64_t o1 = (uint64_t)k1 * 4096; size_t l1 = (size_t)MIN(4096, r->size - o1); uint64_t h1 = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o1, l1);
                if (h1 == r->hash[k1]) break; r->hash[k1] = h1; end = o1 + l1; k1++; }
            struct { uint32_t region, pad; uint64_t off, len; } d = {g_cap_rid[i], 0, o, end - o};
            cap_buf_put(&b, &d, sizeof d); cap_buf_put(&b, (const uint8_t *)(uintptr_t)r->lo + o, (size_t)(end - o));
            k = k1;
        }
    }
    if (b.n) cap_put(&g_cap_w, CAP_PDIF, submit, b.p, b.n, 1);
    cap_buf_free(&b);
}
static void cap_flush_objs(uint32_t submit) {
    if (g_cap_pobj.n) cap_put(&g_cap_w, CAP_POBJ, submit, g_cap_pobj.p, g_cap_pobj.n, 1);
    if (g_cap_tobj.n) cap_put(&g_cap_w, CAP_TOBJ, submit, g_cap_tobj.p, g_cap_tobj.n, 1);
    if (g_cap_progb.n) cap_put(&g_cap_w, CAP_PROG, submit, g_cap_progb.p, g_cap_progb.n, 1);
    if (g_cap_dpolb.n) cap_put(&g_cap_w, CAP_DPOL, submit, g_cap_dpolb.p, g_cap_dpolb.n, 1);
    if (g_cap_kind.n) cap_put(&g_cap_w, CAP_TKND, submit, g_cap_kind.p, g_cap_kind.n, 1);
    g_cap_pobj.n = g_cap_tobj.n = g_cap_progb.n = g_cap_dpolb.n = g_cap_kind.n = 0;
}
// Hook: nvnQueueSubmitCommands, under the submit lock, before the submission is replayed
void nvn_cap_submit(int n, const uint64_t *h) {
    if (!nvn_cap_enabled() || g_cap_presents + 1 != (uint64_t)g_cap_frame) return;
    uint64_t t0 = now_ns();
    mtl_finish();   // what the previous submissions wrote is in memory now: the pools and textures are read as this submit will see them
    g_cap_cur_submit = ++g_cap_submit;
    if (!g_cap_active) {
        if (!cap_open(&g_cap_w, g_cap_path)) { MLOG("[RED] DM_NVN_CAPTURE: cannot write %s: capture off", g_cap_path); g_cap_done = 1; return; }
        g_cap_active = 1; g_cap_t0 = t0;
        cap_head();
    } else cap_diffs(g_cap_cur_submit);
    cap_pools(g_cap_cur_submit);
    cap_registries(g_cap_cur_submit);
    nvn_hm_clear(&g_cap_seg, NULL); g_cap_nseg = 0;
    uint32_t *top = malloc((size_t)MAX(n, 1) * 4);
    for (int i = 0; i < n; i++) top[i] = (uint32_t)cap_seg_get(h[i]);
    for (uint32_t i = 0; i < g_cap_nseg; i++) cap_seg_write(i);   // segments called or jumped to are queued while the earlier ones are written
    cap_flush_objs(g_cap_cur_submit);
    if (g_cap_nsync) { uint32_t ns = g_cap_nsync; cap_put(&g_cap_w, CAP_SYNC, g_cap_cur_submit, &ns, 4, 0); }
    cap_buf s = {0}; uint32_t nn = (uint32_t)n; cap_buf_put(&s, &nn, 4); cap_buf_put(&s, top, (size_t)n * 4);
    cap_put(&g_cap_w, CAP_SUBM, g_cap_cur_submit, s.p, s.n, 0); cap_buf_free(&s); free(top);
    g_cap_ns += now_ns() - t0;
}

// The presented texture's digest: wait for the GPU, read level 0 slice 0 into a tightly packed buffer, SHA-256 of those bytes; pix (optional) receives the bytes too, for the
// pixel comparison. The replayer calls the same function
static int cap_digest(const tex_t *t, uint8_t md[32], uint64_t *bytes, uint32_t *w, uint32_t *hh, uint32_t *pf, const char *png, cap_buf *pix) {
    mtl_finish();
    id o = t && t->serial ? tex_get(t->serial) : nil; if ([o isKindOfClass:[NvPkg class]]) o = ((NvPkg *)o)->tex;
    id<MTLTexture> mt = o; if (!mt) return 0;
    int bpp = cap_pf_bpp(mt.pixelFormat, 0); if (!bpp) return 0;
    size_t row = mt.width * (size_t)bpp, n = row * mt.height;
    id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:mt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(mt.width, mt.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:n];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CC_SHA256(b.contents, (CC_LONG)n, md);
    if (pix) cap_buf_put(pix, b.contents, n);
    *bytes = n; *w = (uint32_t)mt.width; *hh = (uint32_t)mt.height; *pf = (uint32_t)mt.pixelFormat;
    if (png) shot(mt, png, 0, NULL);
    return 1;
}
static void cap_hex(const uint8_t *md, char out[65]) { for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", md[i]); }

// Hook: nvnQueuePresentTexture, before the present is queued
void nvn_cap_present(const tex_t *t, const int crop[4]) {
    if (!nvn_cap_enabled()) return;
    g_cap_presents++;
    if (!g_cap_active || g_cap_presents != (uint64_t)g_cap_frame) {
        if (g_cap_presents >= (uint64_t)g_cap_frame && !g_cap_active) { MLOG("[RED] DM_NVN_CAPTURE: frame %d presented without a submit: nothing captured", g_cap_frame); g_cap_done = 1; }
        return;
    }
    uint64_t t0 = now_ns();
    cap_pres p = {0}; cap_buf px = {0}; char png[1100]; snprintf(png, sizeof png, "%s.live.png", g_cap_path);
    g_cap_cur_submit = g_cap_submit + 1; g_cap_no_read = 1;
    if (t) { p.tex = cap_tex_obj(t); cap_flush_objs(g_cap_cur_submit); }
    if (crop) memcpy(p.crop, crop, sizeof p.crop);
    p.frame = (uint32_t)g_cap_frame;
    if (!t || !cap_digest(t, p.sha256, &p.bytes, &p.width, &p.height, &p.pixfmt, png, &px)) cap_fail("the presented texture could not be read back");
    g_cap_ns += now_ns() - t0; p.capture_ms = g_cap_ns / 1e6;
    cap_put(&g_cap_w, CAP_PRES, g_cap_cur_submit, &p, sizeof p, 0);
    if (px.n) cap_put(&g_cap_w, CAP_LPIX, g_cap_cur_submit, px.p, px.n, 1);
    cap_buf_free(&px);
    {   // the pools as the frame left them (after the GPU finished): the replayer compares its own, which tells pool writes it did not make (the CPU writing during the frame) from ones it made differently
        cap_buf hb = {0};
        for (uint32_t i = 0; i < g_cap_nreg; i++) {
            cap_region *r = &g_cap_reg[i]; uint32_t nb = (uint32_t)((r->size + 4095) / 4096), hd[2] = {g_cap_rid[i], nb}; cap_buf_put(&hb, hd, 8);
            for (uint32_t k = 0; k < nb; k++) { uint64_t o = (uint64_t)k * 4096, h = cap_hash_block((const uint8_t *)(uintptr_t)r->lo + o, (size_t)MIN(4096, r->size - o)); cap_buf_put(&hb, &h, 8); }
        }
        cap_put(&g_cap_w, CAP_PHSH, g_cap_cur_submit, hb.p, hb.n, 1); cap_buf_free(&hb);
    }
    uint64_t raw = g_cap_w.raw, stored = g_cap_w.stored;
    int ok = cap_close(&g_cap_w); char hex[65]; cap_hex(p.sha256, hex);
    MLOG("%sDM_NVN_CAPTURE: frame %d: %u submits, %llu segments, %u regions, %u textures (%llu read back, %.1f MB; %llu not captured), %llu addresses outside the pools; "
         "%.1f MB (%.1f MB raw) in %.0f ms → %s%s; live digest %s (%ux%u format %u)",
         g_cap_failed || !ok ? "[RED] " : "", g_cap_frame, g_cap_submit, (unsigned long long)g_cap_segs, g_cap_nreg, g_cap_ntex, (unsigned long long)g_cap_tex_n, g_cap_tex_bytes / 1048576.0,
         (unsigned long long)g_cap_tex_skip, (unsigned long long)g_cap_unmapped, stored / 1048576.0, raw / 1048576.0, p.capture_ms, g_cap_path, ok ? "" : " (write failed)", hex, p.width, p.height, p.pixfmt);
    g_cap_active = 0; g_cap_done = 1;
}
