/* The game's saved flags read from its tables (zeldaVR/runtime/oot_gamedata.h), on a manager built here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../zeldaVR/runtime/oot_gamedata.h"

static uint8_t *mem; static size_t mem_size; static int refused;
static int reader(uintptr_t at, void *out, size_t size) {
    const uintptr_t base = (uintptr_t)mem;
    if (at < base || at + size > base + mem_size) { refused++; return 0; }
    memcpy(out, (const void *)at, size);
    return 1;
}
/* A box at `box` with `count` entries of `stride` at `entries` and a table of `size` pairs at `table`. */
static void box_make(size_t box, size_t entries, uint32_t count, size_t table, uint32_t size) {
    const uint64_t e = (uint64_t)(uintptr_t)(mem + entries), t = (uint64_t)(uintptr_t)(mem + table);
    memcpy(mem + box + 8, &count, 4); memcpy(mem + box + 16, &e, 8); memcpy(mem + box + 24, &t, 8); memcpy(mem + box + 36, &size, 4);
}
static void table_put(size_t table, uint32_t size, uint32_t hash, uint32_t index) {
    uint32_t at = hash % size, pair[2];
    for (;; at = (at + 1) % size) { memcpy(pair, mem + table + (size_t)at * 8, 8); if (!pair[0]) break; }
    pair[0] = hash; pair[1] = index; memcpy(mem + table + (size_t)at * 8, pair, 8);
}

int main(void) {
    /* the hash: names whose hashes are known from the game */
    assert(oot_gd_hash("Sequence_IsChildhood") == 0xcea220d3u);
    assert(oot_gd_hash("Sequence_IsLoadingBanc") == 0xef2b60c2u);
    assert(oot_gd_hash("PlayerStatus.Life") == OOT_GD_LIFE && oot_gd_hash("PlayerStatus.MaxLife") == OOT_GD_MAX_LIFE);
    assert(oot_gd_hash("PlayerStatus.MP") == OOT_GD_MAGIC && oot_gd_hash("PlayerStatus.MPMax") == OOT_GD_MAX_MAGIC);
    assert(oot_gd_hash("PlayerStatus.CurrentRupee") == OOT_GD_RUPEES);
    assert(oot_gd_hash("DungeonInfo.InsideTheDekuTree.SmallKeyNum") == 0x0914ba07u);

    mem_size = 1 << 16; mem = calloc(1, mem_size);
    const size_t manager = 0x100;
    /* Int box: 5 entries of 0x28 at 0x2000, a table of 7 pairs at 0x3000; two names share a first slot */
    box_make(manager + 0x0c8, 0x2000, 5, 0x3000, 7);
    const uint32_t ints[5] = { OOT_GD_LIFE, OOT_GD_MAX_LIFE, OOT_GD_MAGIC, OOT_GD_MAX_MAGIC, OOT_GD_RUPEES };
    const int32_t values[5] = { 11, 40, 96, 480, 137 };
    for (uint32_t i = 0; i < 5; i++) { table_put(0x3000, 7, ints[i], i); memcpy(mem + 0x2000 + i * 0x28 + 0x1c, &values[i], 4); }
    /* Bool box: 1 entry of 0x18 at 0x4000, a table of 3 at 0x5000 */
    box_make(manager + 0x008, 0x4000, 1, 0x5000, 3);
    table_put(0x5000, 3, OOT_GD_CHILD, 0); mem[0x4000 + 0x16] = 1;

    const uintptr_t m = (uintptr_t)(mem + manager);
    int32_t v = 0; int b = 0;
    for (int i = 0; i < 5; i++) { assert(oot_gd_int(reader, m, ints[i], &v) && v == values[i]); }
    assert(oot_gd_bool(reader, m, OOT_GD_CHILD, &b) && b == 1);
    assert(!oot_gd_int(reader, m, 0x12345678u, &v));                 /* no such flag: the walk ends at an empty pair or after the table */
    assert(!oot_gd_bool(reader, m, OOT_GD_LIFE, &b));                /* an Int is not among the Bools */
    oot_status s;
    assert(oot_gd_status(reader, m, &s) && s.have && s.life == 11 && s.max_life == 40 && s.magic == 96 && s.max_magic == 480 && s.rupees == 137);

    /* a text flag: the map's name */
    assert(oot_gd_hash("Sequence_CurrentBanc") == OOT_GD_MAP);
    box_make(manager + 0x608, 0x6000, 1, 0x7000, 5);
    table_put(0x7000, 5, OOT_GD_MAP, 0);
    { const uint64_t text = (uint64_t)(uintptr_t)(mem + 0x8000); memcpy(mem + 0x6000 + 0x68, &text, 8); memcpy(mem + 0x8000, "HyruleField\0junk", 17); }
    char map[OOT_GD_TEXT_BYTES];
    assert(oot_gd_text(reader, m, OOT_GD_MAP, map) && !strcmp(map, "HyruleField"));
    assert(!oot_gd_text(reader, m, OOT_GD_LIFE, map) && !map[0]);

    /* values out of range are held in; a manager with no hearts is no save */
    const int32_t big = 99999, neg = -5;
    memcpy(mem + 0x2000 + 4 * 0x28 + 0x1c, &big, 4); memcpy(mem + 0x2000 + 0 * 0x28 + 0x1c, &neg, 4);
    assert(oot_gd_status(reader, m, &s) && s.rupees == 9999 && s.life == 0);
    const int32_t none = 0; memcpy(mem + 0x2000 + 1 * 0x28 + 0x1c, &none, 4);
    assert(!oot_gd_status(reader, m, &s) && !s.have);

    /* a stale pointer: the reader refuses and nothing is found; an index past the entries is no entry */
    const uint64_t wild = 0x10; memcpy(mem + manager + 0x0c8 + 24, &wild, 8); refused = 0;
    assert(!oot_gd_int(reader, m, OOT_GD_RUPEES, &v) && refused == 1);
    box_make(manager + 0x0c8, 0x2000, 2, 0x3000, 7);
    assert(!oot_gd_int(reader, m, OOT_GD_RUPEES, &v) && oot_gd_int(reader, m, OOT_GD_LIFE, &v));
    assert(!oot_gd_entry(reader, 0, OOT_GD_INT, OOT_GD_LIFE) && !oot_gd_entry(reader, m, 7, OOT_GD_LIFE));
    free(mem);
    puts("the game's saved flags: the hash, the table walk, the values held in range, a refused read");
    return 0;
}
