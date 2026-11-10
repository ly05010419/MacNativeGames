// Pipeline keys without unused colour state, and the warm-up's counts of its render rows. pso_key_norm is nxnative's
// (games/mk8d/runtime/gfx/nvn/nvn_pso_skey.h, revision 062cd06e, MIT), unchanged; the counts are new. Included by nvn_pso_skey.h
// after pso_key is defined.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Drop blend factors when blending is disabled, and blend / mask bits for absent normal-pass attachments. Merged passes index
// logical blend / mask state through lmap, so physical formats cannot decide which logical targets to drop. pso_desc builds the
// same descriptor either way (it skips absent attachments and ignores factors with blending off), so two keys that differed only
// there were one pipeline compiled twice. Live keys (pso_key_fill) and prewarm rows (pso_skey_parse) both pass here.
static inline void pso_key_norm(pso_key *k, int merged) {
    for (int i = 0; i < 8; i++) {
        if (!merged && !k->cfmt[i]) { k->blend_en &= ~(1u << i); k->chmask &= ~(0xfu << (4 * i)); }
        if (!(k->blend_en >> i & 1)) memset(&k->blend[i], 0, sizeof k->blend[i]);
    }
}

// One prewarm list's render rows (R), in five counts:
//   1. duplicate rows avoided: the normalized key was already met (in this list or an earlier one), nothing compiled;
//   2. skipped before Metal: unreadable row, shader missing or not in the shader map, specialized fragment not made;
//   3. reached compilation;
//   4. Metal calls: one per row that reached, plus the retries for a vertex shader that writes the layer;
//   5. final results: pipelines created and failures.
// rows = 1 + 2 + 3 and 3 = created + failed always hold; the line says so when they do not.
typedef struct { uint64_t rows, dup, unreadable, no_shader, no_fz, reached, calls, retries, made, failed; } pso_warm_rows;
static inline int pso_warm_rows_ok(const pso_warm_rows *c) {
    return c->rows == c->dup + c->unreadable + c->no_shader + c->no_fz + c->reached && c->reached == c->made + c->failed &&
           c->calls == c->reached + c->retries;
}
static inline void pso_warm_rows_line(const pso_warm_rows *c, const char *list, char *out, size_t n) {
    snprintf(out, n, "pipeline prewarm render rows of %s: %llu = %llu duplicate rows avoided + %llu skipped before Metal (unreadable %llu,"
             " shader missing %llu, specialized fragment %llu) + %llu reached compilation; %llu Metal calls (%llu layer retries);"
             " %llu created, %llu failed%s", list, (unsigned long long)c->rows, (unsigned long long)c->dup,
             (unsigned long long)(c->unreadable + c->no_shader + c->no_fz), (unsigned long long)c->unreadable,
             (unsigned long long)c->no_shader, (unsigned long long)c->no_fz, (unsigned long long)c->reached,
             (unsigned long long)c->calls, (unsigned long long)c->retries, (unsigned long long)c->made, (unsigned long long)c->failed,
             pso_warm_rows_ok(c) ? "" : " [RED] the counts do not add up");
}
