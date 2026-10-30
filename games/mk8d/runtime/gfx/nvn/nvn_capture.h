// NVN frame capture and replay (DM_NVN_CAPTURE, dm/gfx/tests/nvn_replay.m): the file format and the record relocation shared by the host's capture
// (nvn_capture.inc, included by nvn_mtl.m), the replayer and the unit test (dm/gfx/tests/t_capture.c). Plain C: no Metal here, so the test runs without a GPU.
//
// A capture is one file: an 16-byte file header, then chunks {tag, codec, raw length, stored length, submit, bytes}. Submit 0 is the state at the first submit of the frame
// (pools, objects, registries, texture contents); submit k > 0 carries what changed before the k-th submission and that submission's command segments.
// Pointers in the captured records are not host addresses: an address into a memory pool is a region number and offset (CAP_ADDR), an object pointer is a kind and index
// (CAP_OBJ). The replayer maps the regions at fresh addresses and rewrites the records back with the same field walk (cap_rec_fields).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvn_obj.h"

#define CAP_MAGIC "NVNCAP\0\0"
#define CAP_VERSION 1u

// Chunk tags (four characters, little-endian)
#define CAP_TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
enum {
    CAP_HEAD = CAP_TAG('H', 'E', 'A', 'D'),   // text: key \t value lines (chain, commit, title, build, frame, DM_* settings, format)
    CAP_LAYT = CAP_TAG('L', 'A', 'Y', 'T'),   // text: the record layout fingerprint (cap_layout); a replayer built against another nvn_obj.h refuses the capture
    CAP_STAT = CAP_TAG('S', 'T', 'A', 'T'),   // text: replay-layer state set outside records (texture pool size, origin, depth mode)
    CAP_REGN = CAP_TAG('R', 'E', 'G', 'N'),   // one memory region: cap_regn + its bytes
    CAP_PADD = CAP_TAG('P', 'A', 'D', 'D'),   // the replay layer's pool list (mtl_pool_add calls): cap_padd[]
    CAP_PDIF = CAP_TAG('P', 'D', 'I', 'F'),   // region bytes that changed before submit k: {u32 region, u32 0, u64 off, u64 len, bytes}...
    CAP_POBJ = CAP_TAG('P', 'O', 'B', 'J'),   // pool_t objects: cap_pobj[]
    CAP_TOBJ = CAP_TAG('T', 'O', 'B', 'J'),   // tex_t objects referenced by records: {u32 index, u32 0, tex_t (relocated)}...
    CAP_PROG = CAP_TAG('P', 'R', 'O', 'G'),   // programs: cap_prog[]
    CAP_DPOL = CAP_TAG('D', 'P', 'O', 'L'),   // descriptor pools: cap_dpol[]
    CAP_SYNC = CAP_TAG('S', 'Y', 'N', 'C'),   // sync objects: u32 count (the replayer allocates that many sync_t)
    CAP_TREG = CAP_TAG('T', 'R', 'E', 'G'),   // texture registry entries (all at submit 0, changed ones later): cap_treg[]
    CAP_SREG = CAP_TAG('S', 'R', 'E', 'G'),   // sampler registry entries: cap_sreg[]
    CAP_TKND = CAP_TAG('T', 'K', 'N', 'D'),   // per texture the frame reaches: how the replay layer holds it (cap_tknd[])
    CAP_TPIX = CAP_TAG('T', 'P', 'I', 'X'),   // contents of one texture whose content lives only in Metal: cap_tpix + cap_tsub + bytes per level and slice
    CAP_SEGM = CAP_TAG('S', 'E', 'G', 'M'),   // one command segment of submit k: {u32 index, u32 length, bytes}
    CAP_SUBM = CAP_TAG('S', 'U', 'B', 'M'),   // submit k: {u32 n, u32 segment[n]}
    CAP_PRES = CAP_TAG('P', 'R', 'E', 'S'),   // the present: cap_pres
    CAP_PHSH = CAP_TAG('P', 'H', 'S', 'H'),   // after the frame: every region's 4 KiB block hashes as the live run left them ({u32 region, u32 blocks, u64 hash[blocks]}...), for the replayer to compare
    CAP_LPIX = CAP_TAG('L', 'P', 'I', 'X'),   // the presented frame's pixels as the live run read them (level 0, rows packed, the format and size of cap_pres), for a pixel comparison; optional
    CAP_END_ = CAP_TAG('E', 'N', 'D', ' '),
};
enum { CAP_RAW = 0, CAP_LZFSE = 1 };
typedef struct { uint32_t tag, codec; uint64_t raw, stored; uint32_t submit, pad; } cap_chunk;

// Field encodings. Host addresses stay below 2^48, so a set top byte marks an encoded value; anything else (0, an address outside every region) is left as it is
#define CAP_ADDR_TAG 0xCAull
#define CAP_REGION_BITS 20
#define CAP_OFF_BITS 36
static inline uint64_t cap_addr(uint32_t region, uint64_t off) { return CAP_ADDR_TAG << 56 | (uint64_t)region << CAP_OFF_BITS | off; }
static inline int cap_is_addr(uint64_t v) { return v >> 56 == CAP_ADDR_TAG; }
static inline uint32_t cap_addr_region(uint64_t v) { return (uint32_t)(v >> CAP_OFF_BITS) & ((1u << CAP_REGION_BITS) - 1); }
static inline uint64_t cap_addr_off(uint64_t v) { return v & ((1ull << CAP_OFF_BITS) - 1); }
enum { CAP_K_TEX = 1, CAP_K_TEXIN, CAP_K_PROG, CAP_K_DPOOL, CAP_K_SYNC, CAP_K_SEG, CAP_K_POOL, CAP_K_N };   // CAP_K_TEXIN: a tex_t snapshot inside the record (hle_nvn.c tex_snap); index = byte offset from the payload
static inline uint64_t cap_obj(int kind, uint64_t index) { return (0xC0ull + (uint64_t)kind) << 56 | index; }
static inline int cap_obj_kind(uint64_t v) { uint64_t t = v >> 56; return t > 0xC0 && t < 0xC0 + CAP_K_N ? (int)(t - 0xC0) : 0; }
static inline uint64_t cap_obj_index(uint64_t v) { return v & ((1ull << 56) - 1); }

// Chunk payloads
typedef struct { uint32_t index, kind; uint64_t old_lo, size; } cap_regn;   // kind 0: pool storage, 1: memory a record points to outside the pools (deferred handles, event words)
typedef struct { uint64_t mem, size; } cap_padd;                             // mem: CAP_ADDR
typedef struct { uint32_t index, flags; uint64_t mem, size; int32_t vmode, pad; } cap_pobj;
// ok: bit i = stage i has a key; iface[s]: fingerprint of the bindings and vertex inputs the replay layer resolved for NVN stage s (0 = none), so a replay
// whose translated shaders take other bindings than the records were made for stops (nvn_replay) instead of drawing with them
typedef struct { uint32_t index, nstage; uint64_t serial; uint8_t key[6][8]; uint8_t ok, pad[7]; uint64_t iface[6]; } cap_prog;
typedef struct { uint32_t index; int32_t n; uint64_t pool; int64_t off; int32_t tex_base, samp_base; } cap_dpol;   // pool: CAP_OBJ(CAP_K_POOL)
typedef struct { int32_t tid; uint8_t ok, has_v, pad[2]; tex_t t; view_t v; } cap_treg;
typedef struct { int32_t sid, pad; samp_par p; } cap_sreg;
// per texture the frame reaches: live = initialized and not finalized (mtl_tex_init); cpu = its host texture is uploaded from its pool (CPU-written, or a texture the
// flush tracking of DM_GAME_TEX_DIRTY_UPLOAD made packaged): the replay marks it CPU-written so it takes the same path
enum { CAP_TK_LIVE = 1, CAP_TK_CPU = 2 };
typedef struct { tex_t t; uint32_t kind, pad; } cap_tknd;   // t: relocated
typedef struct { tex_t t; uint32_t pixfmt, type, width, height, depth, levels, slices, samples, nsub, pad; } cap_tpix;   // t: relocated, the texture to create and fill
typedef struct { uint32_t level, slice, w, h, d, plane; uint64_t row, img, bytes; } cap_tsub;   // plane 0: color or depth, 1: stencil
typedef struct { uint64_t tex; int32_t crop[4]; uint8_t sha256[32]; uint64_t bytes; uint32_t width, height, pixfmt, frame; double capture_ms; } cap_pres;   // tex: CAP_OBJ(CAP_K_TEX)

// The field walk: every pointer or address a record's payload holds, for capture (encode) and replay (decode) alike.
// addr: an address field; len is how many bytes the record uses there when known (0 = a buffer of unknown extent), writable: the replay writes there (events)
// obj: an object pointer of the given kind (CAP_K_*), payload / size: the record's payload, for snapshots inside it
typedef struct {
    int (*addr)(void *ctx, uint64_t *field, uint64_t len, int writable);
    int (*obj)(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
} cap_ops;
// Walks one record (op, payload of size - 8 bytes); 0 = an op the walk does not know (the capture fails rather than write host pointers)
int cap_rec_fields(uint32_t op, uint8_t *payload, uint32_t size, const cap_ops *ops, void *ctx);
// The pointers inside a tex_t snapshot: mem (an address) and pool (a pool object)
int cap_tex_fields(tex_t *t, const cap_ops *ops, void *ctx);
// Layout fingerprint of every payload struct the walk knows (text): stored in the capture, compared by the replayer
const char *cap_layout(void);

// Writing: chunks, compressed with LZFSE when that makes them smaller
typedef struct { FILE *f; uint64_t raw, stored; int err; } cap_writer;
int cap_open(cap_writer *w, const char *path);
void cap_put(cap_writer *w, uint32_t tag, uint32_t submit, const void *data, uint64_t n, int compress);
int cap_close(cap_writer *w);
// Reading: the whole file into memory, then chunk by chunk (data decompressed)
typedef struct { uint8_t *buf; size_t n, pos; } cap_reader;
int cap_load(cap_reader *r, const char *path);
int cap_next(cap_reader *r, cap_chunk *c, uint8_t **data);   // 1 = a chunk (*data malloc'd, free it), 0 = end, -1 = corrupt
void cap_unload(cap_reader *r);
// A growable byte buffer for building chunk payloads
typedef struct { uint8_t *p; size_t n, cap; } cap_buf;
void cap_buf_put(cap_buf *b, const void *data, size_t n);
static inline void cap_buf_free(cap_buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

// Host hooks (nvn_capture.inc, in nvn_mtl.m): hle_nvn.c calls them at each submit (under the submit lock, before the replay) and each present (before it is queued)
void nvn_cap_submit(int n, const uint64_t *handles);
void nvn_cap_present(const tex_t *t, const int crop[4]);
