/* Ocarina of Time in VR: the dungeon row of the wrist panel (small keys, boss key, map, compass), without the runtime
   (tests/test_oot_vr_dungeon.c runs it). vr.md row 83.
   The facts are HookshotVR's notes (camera-ui.md 1.7 and 6.1; vr/menu/wrist_host.c, GPL-3: facts only, no code): the game keeps
   DungeonInfo.<prefix>.SmallKeyNum (Int), .IsGetBossKey, .IsGetMap, .IsGetCompass (Bool) for every dungeon; Link is in one when
   the map he is in (Sequence_CurrentBanc) starts with its prefix, boss rooms too; "has" is what the dungeon really holds, so a
   missing piece is drawn dimmed only where there is one to find. The hashes below are the notes'; the test recomputes each from
   its name with our own MurmurHash3 (oot_gd_hash), so a wrong name or a wrong hash fails there. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "oot_gamedata.h"

enum { OOT_DG_BOSS = 1, OOT_DG_MAP = 2, OOT_DG_COMPASS = 4, OOT_DG_KEYS = 8, OOT_DG_ALL = 15 };
typedef struct { const char *prefix; uint32_t keys, boss, map, compass; uint8_t has; } oot_dungeon;
static const oot_dungeon OOT_DUNGEONS[] = {
    { "InsideTheDekuTree",    0x0914ba07u, 0xf6995ceeu, 0x89787395u, 0x06c51ff2u, OOT_DG_MAP | OOT_DG_COMPASS },
    { "DodongoCavern",        0x3fdbc3d0u, 0xfaabc084u, 0xa52c3a19u, 0x8b93edf9u, OOT_DG_MAP | OOT_DG_COMPASS },
    { "InsideJabuJabuBelly",  0x1470a816u, 0x656bd5eeu, 0xfcadd210u, 0x9a2218dbu, OOT_DG_MAP | OOT_DG_COMPASS },
    { "ForestTemple",         0x90237092u, 0x427333a0u, 0xe704f9b4u, 0xf240eeb2u, OOT_DG_ALL },
    { "FireTemple",           0x1df11487u, 0xb72d73dfu, 0x5b3ac0bau, 0x0aa068b6u, OOT_DG_ALL },
    { "WaterTemple",          0x877bdceeu, 0x79daf80au, 0x4a4402abu, 0x2e82c8feu, OOT_DG_ALL },
    { "ShadowTemple",         0x5edda9e3u, 0xc33c66cfu, 0xf6177b93u, 0x1e2feec5u, OOT_DG_ALL },
    { "SpiritTemple",         0x79eeec8eu, 0x1984c03fu, 0xb85002bau, 0x9bd1181cu, OOT_DG_ALL },
    { "BottomOfTheWell",      0x7dd79d10u, 0x9402715cu, 0x77c47ec9u, 0x9d6efaacu, OOT_DG_MAP | OOT_DG_COMPASS | OOT_DG_KEYS },
    { "IceCavern",            0xfa5d133du, 0x275f4512u, 0x322f2a94u, 0xea1846e6u, OOT_DG_MAP | OOT_DG_COMPASS },
    { "GerudoTrainingGround", 0xea9786f5u, 0x6d155841u, 0x4b36041eu, 0x3d9a2750u, OOT_DG_KEYS },
    { "InsideGanonCastle",    0xfbfd2910u, 0x2fd59293u, 0x9bbc0c4au, 0xc4d4734eu, OOT_DG_KEYS | OOT_DG_BOSS },
    { "GerudoFortress",       0x3a28b6a4u, 0x95fbcb7cu, 0xbcaad6ceu, 0x5eb80679u, OOT_DG_KEYS },
};
enum { OOT_DUNGEON_COUNT = sizeof OOT_DUNGEONS / sizeof *OOT_DUNGEONS };

/* Which dungeon a map belongs to, -1 none. */
static inline int oot_dungeon_for(const char *banc) {
    if (!banc || !*banc) return -1;
    for (int i = 0; i < OOT_DUNGEON_COUNT; i++) if (!strncmp(banc, OOT_DUNGEONS[i].prefix, strlen(OOT_DUNGEONS[i].prefix))) return i;
    return -1;
}

/* The row: which dungeon (-1 none: no row), its small keys (0..99), what is owned (OOT_DG_ bits) and what the dungeon has. */
typedef struct { int dungeon, keys, owned, has; } oot_dungeon_row;
static inline int oot_dungeon_read(oot_gd_reader read, uintptr_t manager, const char *banc, oot_dungeon_row *out) {
    memset(out, 0, sizeof *out); out->dungeon = -1;
    const int d = oot_dungeon_for(banc);
    if (d < 0 || !manager) return 0;
    const oot_dungeon *g = &OOT_DUNGEONS[d];
    int32_t keys = 0; int boss = 0, map = 0, compass = 0;
    (void)oot_gd_int(read, manager, g->keys, &keys);
    (void)oot_gd_bool(read, manager, g->boss, &boss); (void)oot_gd_bool(read, manager, g->map, &map); (void)oot_gd_bool(read, manager, g->compass, &compass);
    out->dungeon = d; out->has = g->has;
    out->keys = keys < 0 ? 0 : keys > 99 ? 99 : keys;
    out->owned = (boss ? OOT_DG_BOSS : 0) | (map ? OOT_DG_MAP : 0) | (compass ? OOT_DG_COMPASS : 0);
    return 1;
}
