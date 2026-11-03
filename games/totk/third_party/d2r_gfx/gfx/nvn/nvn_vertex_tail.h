/* NVN may bind a four-component format to tightly packed three-component data.
 * Metal's offline compiler rejects the overlapping fetch even when the shader
 * never reads its last component. Narrow only when the MSL proves it unused;
 * the address, offset, stride and every component actually read stay unchanged.
 * The narrowing itself is nvn_mtl.m's vfmt_fit (its table of each type's 1-4 component formats). */
#ifndef NVN_VERTEX_TAIL_H
#define NVN_VERTEX_TAIL_H
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; size_t n; } nvn_vt_token;
static inline int nvn_vt_is(nvn_vt_token t, const char *s) {
    return t.n == strlen(s) && !memcmp(t.p, s, t.n);
}
static inline int nvn_vt_same(nvn_vt_token a, nvn_vt_token b) {
    return a.n == b.n && a.n && !memcmp(a.p, b.p, a.n);
}
static inline nvn_vt_token nvn_vt_next(const char **cursor) {
    const char *p = *cursor;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }
        break;
    }
    const char *start = p;
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        while (*p && *p != quote) { if (*p == '\\' && p[1]) p++; p++; }
        if (*p) p++;
    } else if (isalnum((unsigned char)*p) || *p == '_') {
        do { p++; } while (isalnum((unsigned char)*p) || *p == '_');
    } else if (*p) p++;
    *cursor = p;
    return (nvn_vt_token){start, (size_t)(p - start)};
}
static inline int nvn_vt_annotation(const char *p, const char *name, const char **end) {
    if (!nvn_vt_is(nvn_vt_next(&p), "[") || !nvn_vt_is(nvn_vt_next(&p), name)) return 0;
    *end = p; return 1;
}
/* Returns the highest lane read (0 means unused), or 4 when proof is unavailable.
 * Whole-vector, indexed or whole-stage-input uses deliberately keep all lanes. */
static inline unsigned nvn_vertex_components(const char *source, unsigned location) {
    if (!source) return 4;
    const char *p = source, *decl = NULL;
    nvn_vt_token type = {0}, input = {0}, before = {0}, previous = {0}, t;
    while ((t = nvn_vt_next(&p)).n) {
        const char *q;
        if (nvn_vt_is(t, "[") && nvn_vt_annotation(p, "stage_in", &q) &&
            nvn_vt_is(nvn_vt_next(&q), "]") && nvn_vt_is(nvn_vt_next(&q), "]")) {
            if (decl) return 4;
            type = before; input = previous; decl = input.p;
        }
        before = previous; previous = t;
    }
    if (!decl || !type.n) return 4;
    nvn_vt_token field = {0};
    p = source;
    while ((t = nvn_vt_next(&p)).n) {
        if (!nvn_vt_is(t, "struct")) continue;
        const char *q = p;
        if (!nvn_vt_same(nvn_vt_next(&q), type) || !nvn_vt_is(nvn_vt_next(&q), "{")) continue;
        int depth = 1; previous = (nvn_vt_token){0};
        while (depth && (t = nvn_vt_next(&q)).n) {
            if (nvn_vt_is(t, "{")) depth++;
            if (nvn_vt_is(t, "}")) depth--;
            const char *a;
            if (depth == 1 && nvn_vt_is(t, "[") && nvn_vt_annotation(q, "attribute", &a) &&
                nvn_vt_is(nvn_vt_next(&a), "(")) {
                nvn_vt_token number = nvn_vt_next(&a);
                char *end; unsigned long n = strtoul(number.p, &end, 10);
                if (number.n && end == number.p + number.n && n == location &&
                    nvn_vt_is(nvn_vt_next(&a), ")") && nvn_vt_is(nvn_vt_next(&a), "]") &&
                    nvn_vt_is(nvn_vt_next(&a), "]")) {
                    if (field.n) return 4;
                    field = previous;
                }
            }
            previous = t;
        }
        break;
    }
    if (!field.n) return 4;
    unsigned used = 0; p = source;
    while ((t = nvn_vt_next(&p)).n) {
        if (!nvn_vt_same(t, input) || t.p == decl) continue;
        const char *q = p;
        if (!nvn_vt_is(nvn_vt_next(&q), ".")) return 4;
        if (!nvn_vt_same(nvn_vt_next(&q), field)) continue;
        if (!nvn_vt_is(nvn_vt_next(&q), ".")) return 4;
        nvn_vt_token swizzle = nvn_vt_next(&q);
        if (!swizzle.n || swizzle.n > 4) return 4;
        for (size_t i = 0; i < swizzle.n; i++) {
            const char *lanes = "xyzw", *colors = "rgba";
            const char *lane = strchr(lanes, swizzle.p[i]);
            const char *rgba = strchr(colors, swizzle.p[i]);
            unsigned n = lane ? (unsigned)(lane - lanes) + 1 : rgba ? (unsigned)(rgba - colors) + 1 : 4;
            if (n > used) used = n;
        }
    }
    return used;
}
#endif
