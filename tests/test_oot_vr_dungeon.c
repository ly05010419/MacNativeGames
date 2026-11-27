/* The wrist panel's dungeon row (zeldaVR/runtime/oot_vr_dungeon.h): every hash recomputed from its name, the map's prefix, the
   flags read from a manager built here. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../zeldaVR/runtime/oot_vr_dungeon.h"

static uint8_t *mem; static size_t mem_size;
static int reader(uintptr_t at, void *out, size_t size) {
    const uintptr_t base = (uintptr_t)mem;
    if (at < base || at + size > base + mem_size) return 0;
    memcpy(out, (const void *)at, size);
    return 1;
}
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
    /* the notes' hashes are those of the names */
    for (int i = 0; i < OOT_DUNGEON_COUNT; i++) {
        const oot_dungeon *d = &OOT_DUNGEONS[i]; char name[96];
        snprintf(name, sizeof name, "DungeonInfo.%s.SmallKeyNum", d->prefix); assert(oot_gd_hash(name) == d->keys);
        snprintf(name, sizeof name, "DungeonInfo.%s.IsGetBossKey", d->prefix); assert(oot_gd_hash(name) == d->boss);
        snprintf(name, sizeof name, "DungeonInfo.%s.IsGetMap", d->prefix); assert(oot_gd_hash(name) == d->map);
        snprintf(name, sizeof name, "DungeonInfo.%s.IsGetCompass", d->prefix); assert(oot_gd_hash(name) == d->compass);
        assert(d->has && !(d->has & ~OOT_DG_ALL));
    }
    /* the map's name starts with the dungeon's prefix, boss rooms too; the field and the town are none */
    assert(oot_dungeon_for("InsideTheDekuTree") == 0 && oot_dungeon_for("InsideTheDekuTree_Boss") == 0);
    assert(oot_dungeon_for("ForestTemple_Room3") == 3 && oot_dungeon_for("GerudoFortress") == 12);
    assert(oot_dungeon_for("HyruleField_Child") < 0 && oot_dungeon_for("Market_Child") < 0 && oot_dungeon_for("") < 0 && oot_dungeon_for(NULL) < 0);
    assert(oot_dungeon_for("Gerudo") < 0);   /* a prefix of a prefix is not the dungeon */

    mem_size = 1 << 16; mem = calloc(1, mem_size);
    const size_t manager = 0x100; const uintptr_t m = (uintptr_t)(mem + manager);
    oot_dungeon_row row;
    assert(!oot_dungeon_read(reader, m, "HyruleField", &row) && row.dungeon == -1);
    /* the Forest Temple: 3 keys, the map found, the compass and the boss key not */
    const oot_dungeon *f = &OOT_DUNGEONS[3];
    box_make(manager + 0x0c8, 0x2000, 1, 0x3000, 5); table_put(0x3000, 5, f->keys, 0);
    const int32_t three = 3; memcpy(mem + 0x2000 + 0x1c, &three, 4);
    box_make(manager + 0x008, 0x4000, 3, 0x5000, 7);
    table_put(0x5000, 7, f->map, 0); table_put(0x5000, 7, f->compass, 1); table_put(0x5000, 7, f->boss, 2);
    mem[0x4000 + 0x16] = 1; mem[0x4000 + 0x18 + 0x16] = 0; mem[0x4000 + 0x30 + 0x16] = 0;
    assert(oot_dungeon_read(reader, m, "ForestTemple_Boss", &row) && row.dungeon == 3 && row.keys == 3 && row.owned == OOT_DG_MAP && row.has == OOT_DG_ALL);
    mem[0x4000 + 0x30 + 0x16] = 1;   /* the boss key found */
    assert(oot_dungeon_read(reader, m, "ForestTemple", &row) && row.owned == (OOT_DG_MAP | OOT_DG_BOSS));
    const int32_t lots = 500; memcpy(mem + 0x2000 + 0x1c, &lots, 4);
    assert(oot_dungeon_read(reader, m, "ForestTemple", &row) && row.keys == 99);   /* held in range */
    /* a dungeon whose flags are not in the tables: the row with nothing owned */
    assert(oot_dungeon_read(reader, m, "IceCavern", &row) && row.dungeon == 9 && row.keys == 0 && row.owned == 0 && row.has == (OOT_DG_MAP | OOT_DG_COMPASS));
    assert(!oot_dungeon_read(reader, 0, "IceCavern", &row));
    free(mem);
    puts("the dungeon row: 52 hashes from their names, the map's prefix, keys and pieces read and held in range");
    return 0;
}
