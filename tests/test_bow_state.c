/* Test the bow-drawn reader (games/totk/runtime/bow_state.h) on a fake guest memory: the pointer paths of Breath of the
   Wild 1.9.0 and Tears of the Kingdom 1.4.3, and every way a path can be missing or wrong. */
#include "../games/totk/runtime/bow_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }

/* Fake memory: a few regions; a read outside them fails, as an unmapped page does. */
typedef struct { uint64_t base; uint8_t bytes[0x1000]; } Region;
static Region regions[64];
static int nregions;
static void reset(void) { memset(regions, 0, sizeof regions); nregions = 0; }
static uint8_t *at(uint64_t address, size_t size) {
    for (int i = 0; i < nregions; i++)
        if (address >= regions[i].base && address + size <= regions[i].base + sizeof regions[i].bytes) return regions[i].bytes + (address - regions[i].base);
    if (nregions == 64) abort();
    regions[nregions].base = address & ~0xfffull;
    return regions[nregions++].bytes + (address & 0xfff);
}
static int fake_read(uint64_t address, void *out, size_t size) {
    for (int i = 0; i < nregions; i++)
        if (address >= regions[i].base && address + size <= regions[i].base + sizeof regions[i].bytes) { memcpy(out, regions[i].bytes + (address - regions[i].base), size); return 1; }
    return 0;
}
static int reads;
static int counting_read(uint64_t address, void *out, size_t size) { reads++; return fake_read(address, out, size); }
static int failing_read(uint64_t address, void *out, size_t size) { (void)address; (void)out; (void)size; return 0; }
static void put64(uint64_t address, uint64_t v) { memcpy(at(address, 8), &v, 8); }
static void put32(uint64_t address, uint32_t v) { memcpy(at(address, 4), &v, 4); }
static void putf(uint64_t address, float v) { memcpy(at(address, 4), &v, 4); }
static void put8(uint64_t address, uint8_t v) { *at(address, 1) = v; }
static void puts_(uint64_t address, const char *s) { memcpy(at(address, strlen(s) + 1), s, strlen(s) + 1); }

enum { MAIN = 0x100000000 };
/* 1.4.3's scene slot and actor name offsets, as runtime/game_version.h has them (GAME_ADDR(0x4663f08), GAME_ACTOR_NAME) */
enum { TOTK_SCENE_SLOT = 0x3add238, TOTK_ACTOR = 0x218 };

/* Breath of the Wild 1.9.0: [[main+0x1e14fb0]+0x744] is the draw timer of an object whose vtable is main+0x1d4fd88. */
static void botw(float timer, uint64_t object, uint64_t vtable) {
    reset();
    put64(MAIN + 0x1e14fb0, object);
    if (object) { put64(object, vtable); putf(object + 0x744, timer); }
}

/* Tears of the Kingdom 1.4.3: main+0x3add238 -> +0x1e8 scene (component count at +0x50) -> +0x58 components -> [13] ->
   +0x20 link count, +0x28 links (0x70 each, data at +8) -> data+0x40 actor, name pointer at +0x218; Player's
   [[actor+0x248]+0x760]+0x9 bit 0x20 is the drawn flag. Actors are 0x7100000000 + i * 0x10000; Player is at link `player`. */
static uint64_t actor_of(int i) { return 0x7100000000ull + (uint64_t)i * 0x10000; }
static void totk(int links, int player, uint8_t flags, int components) {
    reset();
    uint64_t module = 0x7000010000, scene = 0x7000020000, comps = 0x7000030000, resident = 0x7000040000, list = 0x7000050000;
    put64(MAIN + 0x3add238, module); put64(module + 0x1e8, scene);
    put32(scene + 0x50, (uint32_t)components); put64(scene + 0x58, comps); put64(comps + 13 * 8, resident);
    put32(resident + 0x20, (uint32_t)links); put64(resident + 0x28, list);
    for (int i = 0; i < links && i < 16; i++) {
        uint64_t data = 0x7000060000 + (uint64_t)i * 0x100, actor = actor_of(i), name = actor + 0x800;
        put64(list + (uint64_t)i * 0x70 + 8, data); put64(data + 0x40, actor); put64(actor + 0x218, name);
        puts_(name, i == player ? "Player" : i == player + 1 ? "PlayerCamera" : "Parasail");
    }
    if (player >= 0 && player < links) {
        uint64_t block = 0x7200000000, sub = 0x7200010000;
        put64(actor_of(player) + 0x248, block); put64(block + 0x760, sub); put8(sub + 9, flags);
    }
}

int main(void) {
    /* Breath of the Wild 1.9.0 */
    botw(12.5f, 0x7000001000, MAIN + 0x1d4fd88);
    expect(bow_state_botw190(MAIN, fake_read) == BOW_DRAWN, "botw_drawn: a running draw timer is drawn");
    botw(0, 0x7000001000, MAIN + 0x1d4fd88);
    expect(bow_state_botw190(MAIN, fake_read) == BOW_NOT_DRAWN, "botw_idle: timer 0 is not drawn");
    botw(12.5f, 0, 0);
    expect(bow_state_botw190(MAIN, fake_read) == BOW_UNKNOWN, "botw_no_object: an empty slot (title screen) is unknown");
    botw(12.5f, 0x7000001000, MAIN + 0x1d4fd90);
    expect(bow_state_botw190(MAIN, fake_read) == BOW_UNKNOWN, "botw_wrong_vtable: another object in the slot is unknown");
    botw(NAN, 0x7000001000, MAIN + 0x1d4fd88);
    expect(bow_state_botw190(MAIN, fake_read) == BOW_UNKNOWN, "botw_nan_timer: a non-finite timer is unknown");
    botw(12.5f, 0x7000001000, MAIN + 0x1d4fd88);
    expect(bow_state_botw190(MAIN, failing_read) == BOW_UNKNOWN, "botw_unreadable: a failed read is unknown");

    /* Tears of the Kingdom 1.4.3 */
    BowTotkCache cache = {0, 0};
    totk(9, 3, 0x20, 55);
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_DRAWN, "totk_drawn: Player's bit 0x20 is drawn");
    expect(cache.actor == actor_of(3), "totk_drawn: the Player actor is cached");
    reads = 0; bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, counting_read, &cache); int warm = reads;
    put8(0x7200010000 + 9, 0);
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_NOT_DRAWN && cache.actor == actor_of(3), "totk_warm_cache: the cached Player follows the bit");
    cache = (BowTotkCache){0, 0}; reads = 0; bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, counting_read, &cache);
    expect(warm < reads, "totk_warm_cache: a cached Player needs fewer reads than a search");
    totk(9, 3, 0x1f, 55); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_NOT_DRAWN, "totk_other_bits: other bits are not drawn");
    totk(9, -1, 0, 55); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_UNKNOWN, "totk_no_player: no Player actor is unknown");
    expect(cache.actor == 0, "totk_no_player: nothing is cached");
    totk(9, 5, 0x20, 55); cache = (BowTotkCache){actor_of(3), 3};   /* the old Player address now holds another actor */
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_DRAWN, "totk_stale_cache: a stale cache is searched again");
    expect(cache.actor == actor_of(5), "totk_stale_cache: the new Player actor is cached");
    /* a freed Player (a world reload made a new one at link 5): the old memory, in no link now, still reads "Player" with bit 0x20 */
    totk(9, 5, 0x00, 55); cache = (BowTotkCache){0x7400000000, 3};
    put64(0x7400000000 + 0x218, 0x7400000800); puts_(0x7400000800, "Player");
    put64(0x7400000000 + 0x248, 0x7300000000); put64(0x7300000000 + 0x760, 0x7300010000); put8(0x7300010000 + 9, 0x20);
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_NOT_DRAWN, "totk_freed_cache: a freed actor that still reads Player is not used");
    expect(cache.actor == actor_of(5), "totk_freed_cache: the live Player actor is cached");
    totk(0, 3, 0x20, 55); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_UNKNOWN, "totk_count_out_of_range: no links is unknown");
    totk(65, 3, 0x20, 55); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_UNKNOWN, "totk_count_out_of_range: 65 links is unknown");
    totk(9, 3, 0x20, 13); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_UNKNOWN, "totk_count_out_of_range: fewer than 14 components is unknown");
    totk(9, 3, 0x20, 55); put64(MAIN + 0x3add238, 0); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_UNKNOWN, "totk_no_scene: an empty scene slot (title screen) is unknown");
    totk(9, 3, 0x20, 55); put64(actor_of(3) + 0x248, 0); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, fake_read, &cache) == BOW_UNKNOWN, "totk_null_block: a missing state block is unknown");
    totk(9, 3, 0x20, 55); cache = (BowTotkCache){0, 0};
    expect(bow_state_totk143(MAIN, TOTK_SCENE_SLOT, TOTK_ACTOR, failing_read, &cache) == BOW_UNKNOWN, "totk_unreadable: a failed read is unknown");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("bow state: all checks passed\n");
    return 0;
}
