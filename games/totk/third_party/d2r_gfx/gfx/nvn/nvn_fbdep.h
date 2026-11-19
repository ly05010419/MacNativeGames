/* TOTK (found in 1.4.3): does a fragment function read one of its textures only at positions that depend on another texture read
   (refraction: the screen position plus an offset from a normal map)? Such a draw can sample the colour target it draws
   into anywhere, so the tile read of nvn_fbread.h (the fragment's own pixel) does not serve it; nvn_mtl.m gives it a copy of
   the target instead (csnap_draw). Rauru's ghost in Ukouh Shrine fades in and out with 20 such draws on the HDR scene target.

   nvn_fbdep_dependent: 1 when the texture with argument id aid in a SPIRV-Cross fragment source is used only by .sample(
   calls, at least once, and the position of every such call (its second argument; a level or a bias does not count) names
   a temporary that depends on a texture read. A temporary
   _N depends on one when a line "[type] _N[.lanes] = <rhs>;" has a texture read in <rhs>, or <rhs> names a dependent
   temporary (any order of lines; repeated until nothing changes). 0 for every other shape: the caller then keeps the
   path it had. Shared with tests/fbread/probe.m. */
#pragma once
#import <Foundation/Foundation.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int nvn_fbdep_ident(int c) { return isalnum(c) || c == '_'; }
/* The temporary "_<digits>" that starts at p, not part of a longer name: its number and *end, or -1. */
static inline long nvn_fbdep_tmp(const char *s, const char *p, const char **end) {
    if (*p != '_' || !isdigit((unsigned char)p[1]) || (p > s && nvn_fbdep_ident((unsigned char)p[-1]))) return -1;
    char *e; long v = strtol(p + 1, &e, 10);
    if (nvn_fbdep_ident((unsigned char)*e)) return -1;
    *end = e; return v;
}
static inline int nvn_fbdep_names(const char *s, const char *a, const char *b, const uint8_t *dep, long ndep) {   // [a, b) names a dependent temporary
    for (const char *p = a; p < b; p++) { const char *e; long v = nvn_fbdep_tmp(s, p, &e);
        if (v < 0) continue;
        if (v < ndep && dep[v]) return 1;
        p = e - 1; }
    return 0;
}
static inline int nvn_fbdep_reads(const char *a, const char *b) {   // [a, b) holds a texture read
    static const char *const call[] = {".sample(", ".read(", ".gather(", ".sample_compare(", ".gather_compare("};
    for (unsigned i = 0; i < sizeof call / sizeof *call; i++) if (strnstr(a, call[i], (size_t)(b - a))) return 1;
    return 0;
}

static inline int nvn_fbdep_dependent(NSString *src, int aid) {
    const char *s = src.UTF8String; if (!s) return 0;
    char pat[32]; snprintf(pat, sizeof pat, "[[id(%d)]]", aid);
    const char *d = strstr(s, pat); if (!d) return 0;
    const char *ne = d; while (ne > s && ne[-1] == ' ') ne--;
    const char *nb = ne; while (nb > s && nvn_fbdep_ident((unsigned char)nb[-1])) nb--;
    const char *te = nb; while (te > s && te[-1] == ' ') te--;
    static const char ty[] = "texture2d<float>";
    if (nb == ne || te - s < (long)(sizeof ty - 1) || memcmp(te - (sizeof ty - 1), ty, sizeof ty - 1)) return 0;
    char use[160]; int ul = snprintf(use, sizeof use, "spvDescriptorSet0.%.*s", (int)(ne - nb), nb);
    if (ul >= (int)sizeof use) return 0;
    typedef struct { long v; const char *a, *b; } asg;   // _v = [a, b)
    asg *as = NULL; size_t na = 0, cap = 0; long ndep = 0; int res = 0;
    for (const char *ls = s; *ls; ) { const char *le = strchr(ls, '\n'); if (!le) le = ls + strlen(ls);
        const char *eq = NULL;
        for (const char *p = ls; p < le; p++) { if (*p != '=') continue;
            if (p + 1 < le && p[1] == '=') { p++; continue; }
            if (p > ls && (p[-1] == '!' || p[-1] == '<' || p[-1] == '>')) continue;
            eq = p; break; }
        if (eq) { const char *lhs = eq, *e = NULL; long v = -1;
            if (lhs > ls && strchr("+-*/|&^%", lhs[-1])) lhs--;   // a compound assignment keeps and adds
            for (const char *p = ls; p < lhs && *p != '('; p++) if ((v = nvn_fbdep_tmp(s, p, &e)) >= 0) break;   // "(": a condition or a loop head, no plain assignment
            int ok = v >= 0 && v < (1L << 22);
            for (const char *q = e; ok && q < lhs; q++) ok = nvn_fbdep_ident((unsigned char)*q) || *q == '.' || *q == '[' || *q == ']' || *q == ' ';
            if (ok) {
                if (na == cap) { cap = cap ? cap * 2 : 512; asg *t = realloc(as, cap * sizeof *as); if (!t) { free(as); return 0; } as = t; }
                as[na++] = (asg){v, eq + 1, le}; if (v >= ndep) ndep = v + 1; } }
        ls = *le ? le + 1 : le; }
    uint8_t *dep = calloc((size_t)ndep + 1, 1);
    if (!dep) { free(as); return 0; }
    for (int changed = 1; changed; ) { changed = 0;
        for (size_t i = 0; i < na; i++) { if (dep[as[i].v]) continue;
            if (nvn_fbdep_reads(as[i].a, as[i].b) || nvn_fbdep_names(s, as[i].a, as[i].b, dep, ndep)) { dep[as[i].v] = 1; changed = 1; } } }
    int uses = 0; res = 1;
    for (const char *p = s; res && (p = strstr(p, use)); p += ul) {
        if (nvn_fbdep_ident((unsigned char)p[ul])) continue;   // its sampler (<name>Smplr), or a longer name
        uses++;
        if (strncmp(p + ul, ".sample(", 8)) { res = 0; break; }   // read / gather / a size query
        const char *b = p + ul + 8, *c0 = NULL, *c1 = NULL; int depth = 1;   // [c0, c1): the position, the argument after the sampler
        for (; *b && depth; b++) { if (*b == '(' || *b == '[') depth++; else if (*b == ')' || *b == ']') depth--;
            else if (*b == ',' && depth == 1) { if (!c0) c0 = b + 1; else if (!c1) c1 = b; } }
        if (depth || !c0 || !nvn_fbdep_names(s, c0, c1 ? c1 : b - 1, dep, ndep)) res = 0;
    }
    free(dep); free(as);
    return res && uses > 0;
}
