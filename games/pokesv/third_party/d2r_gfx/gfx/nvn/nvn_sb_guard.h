/* Storage-buffer bounds for translated MSL (Pokémon Violet, 2026-10-31). NVN bounds-checks storage-buffer access against the
   size bound with the buffer (the size the driver keeps next to the address in c0): a load past it reads 0, a store past it
   is dropped. The translation lost those checks, and Metal device pointers have none, so kernels that wrote 16 bytes per
   element for a count read past a 16-byte buffer overwrote game objects next to small pools and faulted the GPU.
   sb_guard_msl rewrites every storage access (*spvDescriptorSet0.<s>p_sN)._m0[E] of a single-array buffer:
     store statement  ->  if (uint(E) < bytes / size) <statement>
     load             ->  ((uint(E) < bytes / size) ? (*...)._m0[E] : T(0))
     &access (atomics)->  in range ? &(*...)._m0[E] : a scratch word
   and adds `device uint* psv_sb [[id(K)]]` (K = the highest id + 1) to the argument struct: bytes bound per argument id
   at [id], scratch at word 128. The replay fills it (NVN_RB_SBSZ, encode_args). Returns NULL when nothing changed. */
#pragma once
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[32], type[32], elem[16]; int id, esize, isconst; } sbg_buf;
typedef struct { char *p; size_t n, cap; } sbg_out;
static void sbg_put(sbg_out *o, const char *s, size_t n) {
    if (o->n + n + 1 > o->cap) { o->cap = (o->n + n + 1) * 2; o->p = realloc(o->p, o->cap); }
    memcpy(o->p + o->n, s, n); o->n += n; o->p[o->n] = 0;
}
static void sbg_puts(sbg_out *o, const char *s) { sbg_put(o, s, strlen(s)); }
static int sbg_esize(const char *t) {
    static const struct { const char *t; int n; } m[] = { {"uint", 4}, {"int", 4}, {"float", 4}, {"uint2", 8}, {"int2", 8}, {"float2", 8},
        {"uint4", 16}, {"int4", 16}, {"float4", 16}, {"ushort", 2}, {"short", 2}, {"half", 2}, {"uchar", 1}, {"char", 1} };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) if (!strcmp(m[i].t, t)) return m[i].n;
    return 0;
}
static const char *SBG_PREFIX = "(*spvDescriptorSet0.";
/* end of the bracket that opens at s[i] ('['), or 0 */
static size_t sbg_close(const char *s, size_t i, size_t end) {
    int d = 0; for (size_t k = i; k < end; k++) { if (s[k] == '[') d++; else if (s[k] == ']' && --d == 0) return k; } return 0;
}
static void sbg_rewrite(sbg_out *o, const char *s, size_t a, size_t end, const sbg_buf *b, int nb);
/* the access at s[p] (pointing at "(*spvDescriptorSet0."): returns the buffer, sets *ib to the '[' and *ie to the ']' */
static const sbg_buf *sbg_match(const char *s, size_t p, size_t end, const sbg_buf *b, int nb, size_t *ib, size_t *ie) {
    size_t lp = strlen(SBG_PREFIX); if (p + lp >= end || strncmp(s + p, SBG_PREFIX, lp)) return NULL;
    for (int i = 0; i < nb; i++) { size_t ln = strlen(b[i].name);
        if (!strncmp(s + p + lp, b[i].name, ln) && !strncmp(s + p + lp + ln, ")._m0[", 6)) {
            *ib = p + lp + ln + 5; *ie = sbg_close(s, *ib, end); return *ie ? &b[i] : NULL; } }
    return NULL;
}
static void sbg_cond(sbg_out *o, const char *s, size_t ib, size_t ie, const sbg_buf *x, const sbg_buf *b, int nb) {
    char t[96]; sbg_puts(o, "(uint("); sbg_rewrite(o, s, ib + 1, ie, b, nb);
    snprintf(t, sizeof t, ") < (spvDescriptorSet0.psv_sb[%d] / %du))", x->id, x->esize); sbg_puts(o, t);
}
static void sbg_access(sbg_out *o, const char *s, const sbg_buf *x, size_t ib, size_t ie, const sbg_buf *b, int nb) {
    sbg_puts(o, SBG_PREFIX); sbg_puts(o, x->name); sbg_puts(o, ")._m0["); sbg_rewrite(o, s, ib + 1, ie, b, nb); sbg_puts(o, "]");
}
static void sbg_rewrite(sbg_out *o, const char *s, size_t a, size_t end, const sbg_buf *b, int nb) {
    size_t i = a, run = a;
    while (i < end) {
        size_t ib, ie; const sbg_buf *x;
        int amp = s[i] == '&' && i + 1 < end && s[i + 1] == '(';
        size_t at = amp ? i + 1 : i;
        if ((s[at] == '(') && (x = sbg_match(s, at, end, b, nb, &ib, &ie))) {
            sbg_put(o, s + run, i - run);
            if (amp) {   /* address (atomics): in range, or the scratch words */
                char t[96]; sbg_puts(o, "("); sbg_cond(o, s, ib, ie, x, b, nb); sbg_puts(o, " ? &"); sbg_access(o, s, x, ib, ie, b, nb);
                snprintf(t, sizeof t, " : (%sdevice %s*)(spvDescriptorSet0.psv_sb + 128))", x->isconst ? "const " : "", x->elem); sbg_puts(o, t);
            } else {
                /* a store when only whitespace precedes it on the line and an assignment follows ([.member] op=) */
                size_t ls = at; while (ls > 0 && (s[ls - 1] == ' ' || s[ls - 1] == '\t')) ls--;
                size_t k = ie + 1; while (k < end && (s[k] == '.' || isalnum((unsigned char)s[k]) || s[k] == '_')) k++;
                int store = (ls == 0 || s[ls - 1] == '\n') && k + 2 < end && s[k] == ' ' &&
                            (s[k + 1] == '=' ? s[k + 2] == ' ' : (strchr("+-*/|&^", s[k + 1]) && s[k + 2] == '='));
                if (store) { sbg_puts(o, "if "); sbg_cond(o, s, ib, ie, x, b, nb); sbg_puts(o, " "); sbg_access(o, s, x, ib, ie, b, nb); }
                else { char t[48]; sbg_puts(o, "("); sbg_cond(o, s, ib, ie, x, b, nb); sbg_puts(o, " ? "); sbg_access(o, s, x, ib, ie, b, nb);
                       snprintf(t, sizeof t, " : %s(0))", x->elem); sbg_puts(o, t); }
            }
            i = run = ie + 1; continue;
        }
        i++;
    }
    sbg_put(o, s + run, end - run);
}
static char *sb_guard_msl(const char *src, int *aid_out) {
    *aid_out = -1;
    const char *st = strstr(src, "struct spvDescriptorSetBuffer0\n{\n"); if (!st) return NULL;
    const char *se = strstr(st, "\n};"); if (!se) return NULL;
    sbg_buf b[64]; int nb = 0, maxid = -1;
    for (const char *l = strchr(st, '\n') + 1; l && l < se; l = strchr(l, '\n') + 1) {
        const char *idp = strstr(l, "[[id("); const char *le = strchr(l, '\n'); if (!idp || idp > le) continue;
        int id = atoi(idp + 5); if (id > maxid) maxid = id;
        char q1[16] = "", q2[16] = "", ty[32] = "", nm[32] = ""; int isconst = 0;
        if (sscanf(l, " const device %31[A-Za-z0-9_]* %31[A-Za-z0-9_]", ty, nm) == 2) isconst = 1;
        else if (sscanf(l, " device %31[A-Za-z0-9_]* %31[A-Za-z0-9_]", ty, nm) != 2) continue;
        (void)q1; (void)q2;
        if (!(strlen(nm) >= 5 && islower((unsigned char)nm[0]) && nm[1] == 'p' && nm[2] == '_' && nm[3] == 's' && isdigit((unsigned char)nm[4]))) continue;
        /* the buffer's struct must be exactly one array member _m0 of a known element type */
        char pat[64]; snprintf(pat, sizeof pat, "struct %s\n{\n", ty); const char *sd = strstr(src, pat); if (!sd) continue;
        char el[16] = ""; int alen = 0; const char *ml = sd + strlen(pat);
        if (sscanf(ml, " %15s _m0[%d];", el, &alen) != 2) continue;
        const char *mle = strchr(ml, '\n'); if (!mle || strncmp(mle, "\n};", 3)) continue;
        int es = sbg_esize(el); if (!es || nb >= 64) continue;
        sbg_buf *x = &b[nb++]; snprintf(x->name, sizeof x->name, "%s", nm); snprintf(x->type, sizeof x->type, "%s", ty); snprintf(x->elem, sizeof x->elem, "%s", el);
        x->id = id; x->esize = es; x->isconst = isconst;
    }
    if (!nb || maxid < 0 || maxid + 1 >= 128) return NULL;
    for (int i = 0; i < nb; i++) { char probe[64]; snprintf(probe, sizeof probe, "%s%s)._m0[", SBG_PREFIX, b[i].name); if (strstr(se, probe)) goto used; }
    return NULL;
used:;
    sbg_out o = {0}; char line[96];
    sbg_put(&o, src, (size_t)(se - src) + 1);   /* through the newline before "};" */
    snprintf(line, sizeof line, "    device uint* psv_sb [[id(%d)]];\n", maxid + 1); sbg_puts(&o, line);
    sbg_rewrite(&o, src, (size_t)(se - src) + 1, strlen(src), b, nb);
    *aid_out = maxid + 1;
    return o.p;
}

/* 2D image atomics (SPIRV-Cross emulates them through a buffer indexed aligned width * y + x): NVN drops an atomic outside
   the image, the emulation wrote past the buffer (shader 318346b1 faulted the GPU in the grassland). Coordinates outside the
   image go to the slot right after the last row; the replay's scratch buffer for such images has room for it. */
static char *img_atomic_guard_msl(const char *src) {
    const char *def = "#define spvImage2DAtomicCoord(tc, tex)"; const char *d = strstr(src, def); if (!d) return NULL;
    const char *use = strstr(d + strlen(def), "spvImage2DAtomicCoord("); if (!use) return NULL;
    const char *dl = strchr(d, '\n'); if (!dl) return NULL;
    sbg_out o = {0};
    sbg_put(&o, src, (size_t)(dl - src) + 1);
    sbg_puts(&o, "#define spvImage2DAtomicCoordGuarded(tc, tex) ((uint((tc).x) < (tex).get_width() && uint((tc).y) < (tex).get_height()) ? spvImage2DAtomicCoord(tc, tex) : "
                 "((((tex).get_width() + spvLinearTextureAlignment / 4 - 1) & ~(spvLinearTextureAlignment / 4 - 1)) * (tex).get_height()))\n");
    const char *p = dl + 1;
    for (const char *q; (q = strstr(p, "spvImage2DAtomicCoord(")); p = q + strlen("spvImage2DAtomicCoord(")) {
        sbg_put(&o, p, (size_t)(q - p)); sbg_puts(&o, "spvImage2DAtomicCoordGuarded("); }
    sbg_puts(&o, p);
    return o.p;
}
