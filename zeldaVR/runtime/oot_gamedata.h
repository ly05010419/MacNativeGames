/* Ocarina of Time's saved flags, read from the game's own tables without calling the game (tests/test_oot_gamedata.c
   runs it on a table of its own). The manager is *(*(main+0x4c7e180)). A flag is named by MurmurHash3 (x86, 32 bits,
   seed 0) of its name; each kind of flag has a "box" in the manager: +8 the count of entries, +16 the entries, +24 a hash
   table of {hash, entry index} pairs, +36 the table's size; the search starts at hash % size and walks on, a pair with
   hash 0 ends it. Layout as HookshotVR's notes give it (host/oot_state_tap.c, GPL-3: facts only, no code); the two hashes
   our own runtime already used (Sequence_IsChildhood 0xcea220d3, Sequence_IsLoadingBanc 0xef2b60c2) come out of this
   hash. Every read goes through the caller's reader, which may refuse an address: a stale pointer ends the search. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef int (*oot_gd_reader)(uintptr_t at, void *out, size_t size);   /* 1: read */

enum { OOT_GD_BOOL, OOT_GD_INT, OOT_GD_FLOAT, OOT_GD_TEXT, OOT_GD_KINDS, OOT_GD_PROBES = 512, OOT_GD_TABLE_MAX = 1 << 22, OOT_GD_TEXT_BYTES = 64 };
static const struct { uint32_t box, stride, value; } OOT_GD_LAYOUT[OOT_GD_KINDS] = {
    { 0x008, 0x18, 0x16 },   /* Bool: a byte */
    { 0x0c8, 0x28, 0x1c },   /* Int */
    { 0x188, 0x28, 0x1c },   /* Float */
    { 0x608, 0x108, 0x68 },  /* String64: a pointer to 64 bytes of text */
};

static inline uint32_t oot_gd_hash(const char *name) {
    const size_t len = strlen(name); const uint8_t *p = (const uint8_t *)name;
    uint32_t h = 0;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t k; memcpy(&k, p + i, 4);
        k *= 0xcc9e2d51u; k = k << 15 | k >> 17; k *= 0x1b873593u;
        h ^= k; h = h << 13 | h >> 19; h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    switch (len & 3) {
        case 3: k ^= (uint32_t)p[(len & ~(size_t)3) + 2] << 16;   /* fall through */
        case 2: k ^= (uint32_t)p[(len & ~(size_t)3) + 1] << 8;    /* fall through */
        case 1: k ^= p[len & ~(size_t)3]; k *= 0xcc9e2d51u; k = k << 15 | k >> 17; k *= 0x1b873593u; h ^= k;
    }
    h ^= (uint32_t)len; h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    return h;
}

/* Where a flag's entry is, 0 when the manager has none of that name (or a read was refused). */
static inline uintptr_t oot_gd_entry(oot_gd_reader read, uintptr_t manager, int kind, uint32_t hash) {
    if (!manager || kind < 0 || kind >= OOT_GD_KINDS || !hash) return 0;
    const uintptr_t box = manager + OOT_GD_LAYOUT[kind].box;
    uint32_t count = 0, size = 0; uint64_t entries = 0, table = 0;
    if (!read(box + 8, &count, 4) || !read(box + 16, &entries, 8) || !read(box + 24, &table, 8) || !read(box + 36, &size, 4)) return 0;
    if (!count || !entries || !table || !size || size > OOT_GD_TABLE_MAX) return 0;
    for (uint32_t at = hash % size, n = 0; n < OOT_GD_PROBES && n < size; n++, at = (at + 1) % size) {
        uint32_t pair[2];
        if (!read((uintptr_t)table + (uintptr_t)at * 8, pair, sizeof pair) || !pair[0]) return 0;
        if (pair[0] == hash) return pair[1] < count ? (uintptr_t)entries + (uintptr_t)pair[1] * OOT_GD_LAYOUT[kind].stride : 0;
    }
    return 0;
}
static inline int oot_gd_int(oot_gd_reader read, uintptr_t manager, uint32_t hash, int32_t *out) {
    const uintptr_t e = oot_gd_entry(read, manager, OOT_GD_INT, hash);
    return e && read(e + OOT_GD_LAYOUT[OOT_GD_INT].value, out, 4);
}
static inline int oot_gd_bool(oot_gd_reader read, uintptr_t manager, uint32_t hash, int *out) {
    const uintptr_t e = oot_gd_entry(read, manager, OOT_GD_BOOL, hash); uint8_t b = 0;
    if (!e || !read(e + OOT_GD_LAYOUT[OOT_GD_BOOL].value, &b, 1)) return 0;
    *out = b != 0;
    return 1;
}

/* A text flag (64 bytes at most), as a C string; 0 when there is none. */
static inline int oot_gd_text(oot_gd_reader read, uintptr_t manager, uint32_t hash, char out[OOT_GD_TEXT_BYTES]) {
    const uintptr_t e = oot_gd_entry(read, manager, OOT_GD_TEXT, hash); uint64_t text = 0;
    out[0] = 0;
    if (!e || !read(e + OOT_GD_LAYOUT[OOT_GD_TEXT].value, &text, 8) || !text || !read((uintptr_t)text, out, OOT_GD_TEXT_BYTES)) { out[0] = 0; return 0; }
    out[OOT_GD_TEXT_BYTES - 1] = 0;
    for (char *c = out; *c; c++) if (*c < 32 || *c > 126) { *c = 0; break; }
    return 1;
}
static const uint32_t OOT_GD_MAP = 0x1d6189dau;   /* Sequence_CurrentBanc: the map Link is in */

/* What the wrist panel shows (oot_vr_wrist.h). */
static const uint32_t OOT_GD_LIFE = 0x31ab5580u, OOT_GD_MAX_LIFE = 0xfbe01da1u, OOT_GD_MAGIC = 0x76b18200u, OOT_GD_MAX_MAGIC = 0x12d63a97u,
                      OOT_GD_RUPEES = 0xa77921d7u, OOT_GD_CHILD = 0xcea220d3u;
typedef struct { int have, life, max_life, magic, max_magic, rupees; } oot_status;   /* life in quarter hearts */
static inline int oot_gd_status(oot_gd_reader read, uintptr_t manager, oot_status *s) {
    int32_t life = 0, max = 0, magic = 0, max_magic = 0, rupees = 0;
    memset(s, 0, sizeof *s);
    if (!oot_gd_int(read, manager, OOT_GD_LIFE, &life) || !oot_gd_int(read, manager, OOT_GD_MAX_LIFE, &max) || max < 1 || max > 400) return 0;
    (void)oot_gd_int(read, manager, OOT_GD_MAGIC, &magic); (void)oot_gd_int(read, manager, OOT_GD_MAX_MAGIC, &max_magic); (void)oot_gd_int(read, manager, OOT_GD_RUPEES, &rupees);
    s->have = 1; s->max_life = max; s->life = life < 0 ? 0 : life > max ? max : life;
    s->max_magic = max_magic < 0 ? 0 : max_magic > 1000 ? 1000 : max_magic;
    s->magic = magic < 0 ? 0 : magic > s->max_magic ? s->max_magic : magic;
    s->rupees = rupees < 0 ? 0 : rupees > 9999 ? 9999 : rupees;
    return 1;
}
