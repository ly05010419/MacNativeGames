#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn_obj.h"

typedef struct {
    const void *vs, *fs; uint32_t cfmt[8], dfmt, sfmt, samples, blend_en, chmask, a2c; blend_st blend[8];
    va_st attr[16]; vs_st stream[16];
    uint8_t lmap[8]; const void *fsfn;
} pso_key;

typedef struct {
    uint8_t kind, fbmask;
    int8_t fx[32];
} pso_fz_desc;

typedef struct { char vs[24], fs[24]; uint8_t vs_stage, fs_stage, merged, fz; pso_fz_desc fz_desc; pso_key k; } pso_skey;

// Drop blend factors when blending is disabled, and blend / mask bits for absent normal-pass attachments. Merged passes index logical blend / mask state through lmap,
// so physical formats cannot decide which logical targets to drop. Depth / stencil formats stay for MTLDevice pipelines. Normalize both live keys and old prewarm rows.
// See docs/reference/shaders.md, Pipeline keys omit unused colour state; measurements belong to the linked game notes.
static inline void pso_key_norm(pso_key *k, int merged) {
    for (int i = 0; i < 8; i++) {
        if (!merged && !k->cfmt[i]) { k->blend_en &= ~(1u << i); k->chmask &= ~(0xfu << (4 * i)); }
        if (!(k->blend_en >> i & 1)) memset(&k->blend[i], 0, sizeof k->blend[i]);
    }
}

static inline void pso_skey_from(const pso_key *k, NvShader *vs, NvShader *fs, int merged, const pso_fz_desc *fz, pso_skey *out) {
    memset(out, 0, sizeof *out);
    memcpy(&out->k, k, sizeof out->k);
    out->k.vs = out->k.fs = out->k.fsfn = NULL;
    if (vs) { memcpy(out->vs, vs->key, sizeof out->vs); out->vs_stage = (uint8_t)vs->stage; }
    if (fs) { memcpy(out->fs, fs->key, sizeof out->fs); out->fs_stage = (uint8_t)fs->stage; }
    out->merged = merged != 0;
    out->fz = k->fsfn != NULL;
    if (fz) memcpy(&out->fz_desc, fz, sizeof out->fz_desc);
}

static inline void pso_skey_hex(const pso_skey *key, char *out) {
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof *key; i++) { out[i * 2] = hex[p[i] >> 4]; out[i * 2 + 1] = hex[p[i] & 15]; }
    out[sizeof *key * 2] = 0;
}

static inline int pso_skey_parse(const char *hex, pso_skey *out) {
    size_t n = sizeof *out * 2;
    if (!hex || strlen(hex) != n) return -1;
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < n; i += 2) {
        int a = hex[i] >= '0' && hex[i] <= '9' ? hex[i] - '0' : hex[i] >= 'a' && hex[i] <= 'f' ? hex[i] - 'a' + 10 : hex[i] >= 'A' && hex[i] <= 'F' ? hex[i] - 'A' + 10 : -1;
        int b = hex[i + 1] >= '0' && hex[i + 1] <= '9' ? hex[i + 1] - '0' : hex[i + 1] >= 'a' && hex[i + 1] <= 'f' ? hex[i + 1] - 'a' + 10 : hex[i + 1] >= 'A' && hex[i + 1] <= 'F' ? hex[i + 1] - 'A' + 10 : -1;
        if (a < 0 || b < 0) return -1;
        p[i / 2] = (uint8_t)(a << 4 | b);
    }
    pso_key_norm(&out->k, out->merged);
    return 0;
}
