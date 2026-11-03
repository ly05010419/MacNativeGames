// Exact, append-only serial membership without an O(number of serials) rehash.
// The caller provides synchronization, just as for nvn_hmap. Do not clear old
// serials: texture-pool slots and deferred recordings may still refer to them.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef TOTK_SERIAL_SET_FAIL   // out of memory; nvn_mtl.m logs it as FATAL first
#define TOTK_SERIAL_SET_FAIL() abort()
#endif
#define TOTK_SERIAL_PAGE_BITS 16
#define TOTK_SERIAL_WORDS (1u << (TOTK_SERIAL_PAGE_BITS - 6))
typedef struct totk_serial_node { void *child[256]; } totk_serial_node;
typedef struct {
    totk_serial_node *root;
    struct { uint64_t key; uint64_t *bits; int valid; } cache[16];
    uint64_t n, pages, nodes, bytes;
} totk_serial_set;

static inline uint64_t *totk_serial_page(totk_serial_set *s, uint64_t key, int create) {
    unsigned ci = (unsigned)(key ^ (key >> 16) ^ (key >> 32)) & 15;
    if (s->cache[ci].valid && s->cache[ci].key == key && (!create || s->cache[ci].bits))
        return s->cache[ci].bits;
    uint64_t *bits = NULL;
    if (!s->root && create) {
        s->root = calloc(1, sizeof *s->root); if (!s->root) TOTK_SERIAL_SET_FAIL();
        s->nodes++; s->bytes += sizeof *s->root;
    }
    totk_serial_node *node = s->root;
    // key is the upper 48 bits of the serial. Six byte-wide levels support the
    // entire uint64_t domain; each allocation is at most one fixed-size page.
    for (int shift = 40; shift >= 0 && node; shift -= 8) {
        unsigned idx = (key >> shift) & 255;
        if (!node->child[idx] && create) {
            size_t bytes = shift ? sizeof *node : TOTK_SERIAL_WORDS * sizeof(uint64_t);
            node->child[idx] = calloc(1, bytes); if (!node->child[idx]) TOTK_SERIAL_SET_FAIL();
            s->bytes += bytes; if (shift) s->nodes++; else s->pages++;
        }
        if (shift) node = node->child[idx];
        else bits = node->child[idx];
    }
    s->cache[ci].key = key; s->cache[ci].bits = bits; s->cache[ci].valid = 1;
    return bits;
}
static inline int totk_serial_has(totk_serial_set *s, uint64_t serial) {
    uint64_t *p = totk_serial_page(s, serial >> TOTK_SERIAL_PAGE_BITS, 0);
    return p && !!(p[(serial & 65535) >> 6] & (UINT64_C(1) << (serial & 63)));
}
// Returns 1 only for the first insertion. No key or bitmap is ever relocated.
static inline int totk_serial_add(totk_serial_set *s, uint64_t serial) {
    uint64_t *p = totk_serial_page(s, serial >> TOTK_SERIAL_PAGE_BITS, 1);
    uint64_t mask = UINT64_C(1) << (serial & 63); unsigned word = (serial & 65535) >> 6;
    if (p[word] & mask) return 0;
    p[word] |= mask; s->n++; return 1;
}
// Only at teardown or in isolated CPU tests; never used as live history GC.
static inline void totk_serial_free_node(totk_serial_node *p, int level) {
    if (!p) return;
    for (unsigned i = 0; i < 256; i++) if (level == 1) free(p->child[i]);
        else totk_serial_free_node(p->child[i], level - 1);
    free(p);
}
static inline void totk_serial_destroy(totk_serial_set *s) {
    totk_serial_free_node(s->root, 6); memset(s, 0, sizeof *s);
}
