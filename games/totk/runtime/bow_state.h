/* Is Link drawing the bow right now? Read from the game's own memory, so a DualSense's R2 can resist like a bowstring
   only while he is (runtime/gamepad_feedback.m). The game sends a controller nothing for this, so the places were found
   by comparing its memory with the bow drawn and not drawn (on the ground, in the air, quick taps and long holds, ZR
   held in the inventory) and then following pointers back to main; they hold for these versions only:
     Breath of the Wild 1.9.0   the float [[main+0x1e14fb0]+0x744] counts frames since the draw began, 0 when the bow is
                                not drawn (its menu, map and weapon / shield wheels set it to 0 too); the object's vtable
                                is main+0x1d4fd88
     Tears of the Kingdom 1.4.3 bit 0x20 of the byte [[Player+0x248]+0x760]+0x9; the Player actor is found as
                                runtime/world_survey.c finds it (the scene's resident component, the actor named "Player"),
                                with the scene slot and the actor's name offset from runtime/game_version.h
   Tears of the Kingdom's menus pause the world, and a paused Player keeps that bit as it was, so a bow drawn when a menu
   opened read as drawn until it closed. Bit 0x40 of the Player's word at +0x5ac says the world holds him (2026-10-27,
   runtime/mem_scan.h): the actor update main+0x2e27a0 looks the actor's pause groups up in the pause requests every frame
   and sets the bit when one holds it (main+0x2e3c04), else clears it (main+0x2e2b78). The pause menu sets it within a frame
   and closing it clears it; in play, jumping and drawing included, it stays clear; the cameras and enemies get it too.
   Such a Player is BOW_PAUSED, never drawn.
   Only reads, each through the caller's read function, which fails instead of faulting on a bad address. Anything
   missing or unexpected (the title screen, a world being loaded, an object of another type) is BOW_UNKNOWN, never drawn.
   Pure C: tests/test_bow_state.c checks it on a fake memory. */
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int (*BowRead)(uint64_t address, void *out, size_t size);   /* 1 when all bytes were read */
enum { BOW_UNKNOWN = -1, BOW_NOT_DRAWN = 0, BOW_DRAWN = 1, BOW_PAUSED = 2 };

/* R2 resists only while everything agrees the bowstring is in the player's hand: the game reads drawn (not paused, not
   unknown), the game gets the controller's input (no settings panel or on-screen keyboard holding it), and the app is in
   front (macOS drops a background app's trigger effects anyway). Anything else lets R2 go. */
static inline int bow_resists(int state, int game_has_input, int app_active) {
    return state == BOW_DRAWN && game_has_input && app_active;
}

static inline int bow_read_u64(BowRead read, uint64_t address, uint64_t *out) { *out = 0; return address && read(address, out, 8) && *out; }

enum { BOTW190_DRAW_SLOT = 0x1e14fb0, BOTW190_DRAW_VTABLE = 0x1d4fd88, BOTW190_DRAW_TIMER = 0x744 };
static inline int bow_state_botw190(uint64_t main_base, BowRead read) {
    uint64_t object, vtable; float timer;
    if (!bow_read_u64(read, main_base + BOTW190_DRAW_SLOT, &object) || !bow_read_u64(read, object, &vtable) ||
        vtable != main_base + BOTW190_DRAW_VTABLE || !read(object + BOTW190_DRAW_TIMER, &timer, sizeof timer) || !isfinite(timer))
        return BOW_UNKNOWN;
    return timer > 0 ? BOW_DRAWN : BOW_NOT_DRAWN;
}

enum { TOTK_SCENE = 0x1e8, TOTK_SCENE_COUNT = 0x50, TOTK_SCENE_COMPONENTS = 0x58, TOTK_RESIDENT = 13, TOTK_LINK_COUNT = 0x20,
       TOTK_LINKS = 0x28, TOTK_LINK_STRIDE = 0x70, TOTK_LINK_DATA = 8, TOTK_DATA_ACTOR = 0x40, TOTK143_PLAYER_BLOCK = 0x248,
       TOTK143_BLOCK_STATE = 0x760, TOTK143_STATE_BYTE = 9, TOTK143_STATE_DRAWN = 0x20, TOTK143_ACTOR_FLAGS = 0x5ac,
       TOTK143_ACTOR_PAUSED = 0x40 };
static inline int bow_totk_is_player(uint64_t actor, uint64_t actor_name, BowRead read) {
    uint64_t name; char text[7];
    return bow_read_u64(read, actor + actor_name, &name) && read(name, text, sizeof text) && !memcmp(text, "Player", 7);
}
static inline uint64_t bow_totk_link_actor(uint64_t links, int32_t i, BowRead read) {
    uint64_t data, actor;
    return bow_read_u64(read, links + (uint64_t)i * TOTK_LINK_STRIDE + TOTK_LINK_DATA, &data) && bow_read_u64(read, data + TOTK_DATA_ACTOR, &actor) ? actor : 0;
}
/* The Player actor and its place in the resident links, kept between calls. A cached actor is used only while the live
   link at that place still holds it (a world reload frees the actor, whose memory may keep reading "Player"); otherwise
   the links are searched again. */
typedef struct { uint64_t actor; int32_t index; } BowTotkCache;
static inline int bow_state_totk143(uint64_t main_base, uint64_t scene_slot, uint64_t actor_name, BowRead read, BowTotkCache *cache) {
    uint64_t module, scene, components, resident, links, actor = 0; int32_t scene_count, count;
    if (!bow_read_u64(read, main_base + scene_slot, &module) || !bow_read_u64(read, module + TOTK_SCENE, &scene) ||
        !read(scene + TOTK_SCENE_COUNT, &scene_count, 4) || scene_count <= TOTK_RESIDENT || scene_count > 256 ||
        !bow_read_u64(read, scene + TOTK_SCENE_COMPONENTS, &components) ||
        !bow_read_u64(read, components + TOTK_RESIDENT * 8, &resident) ||
        !read(resident + TOTK_LINK_COUNT, &count, 4) || count < 1 || count > 64 || !bow_read_u64(read, resident + TOTK_LINKS, &links)) {
        *cache = (BowTotkCache){0, 0};
        return BOW_UNKNOWN;
    }
    if (cache->actor && cache->index >= 0 && cache->index < count && bow_totk_link_actor(links, cache->index, read) == cache->actor &&
        bow_totk_is_player(cache->actor, actor_name, read))
        actor = cache->actor;
    for (int32_t i = 0; i < count && !actor; i++) {
        uint64_t candidate = bow_totk_link_actor(links, i, read);
        if (candidate && bow_totk_is_player(candidate, actor_name, read)) { actor = candidate; *cache = (BowTotkCache){actor, i}; }
    }
    if (!actor) { *cache = (BowTotkCache){0, 0}; return BOW_UNKNOWN; }
    uint64_t block, state; uint8_t flags; uint32_t actor_flags;
    if (!read(actor + TOTK143_ACTOR_FLAGS, &actor_flags, 4) || !bow_read_u64(read, actor + TOTK143_PLAYER_BLOCK, &block) ||
        !bow_read_u64(read, block + TOTK143_BLOCK_STATE, &state) || !read(state + TOTK143_STATE_BYTE, &flags, 1))
        return BOW_UNKNOWN;
    if (actor_flags & TOTK143_ACTOR_PAUSED) return BOW_PAUSED;   /* a menu holds the world: the drawn bit is frozen */
    return flags & TOTK143_STATE_DRAWN ? BOW_DRAWN : BOW_NOT_DRAWN;
}
