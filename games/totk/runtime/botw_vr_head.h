/* Link's head hidden in first person (user 2026-11-19: "用BetterVR的 方案"), as BotW-BetterVR does it
 * (src/game/botw/player/head_visibility.cpp, MIT: the player model's head materials set invisible through the model
 * unit's material visibility; no code copied, its Wii U addresses do not apply here).
 *
 * Breath of the Wild 1.6.0, found 2026-11-19 in the game's own SetMaterialVisible AI action (oneShot_ at main+0x3a0b80,
 * loadParams_ at main+0x3a0c70; zeldaret/botw names both):
 *   the actor's model           actor + 0x4e0
 *   its model units             model + 0x38 (int32 count), model + 0x40 (array; each entry points at the unit pointer)
 *   searchMaterialIndex         unit vtable + 0xc0: (unit, const sead::SafeString *name) -> index, or -1
 *   setMaterialVisible          unit vtable + 0x1b0: (unit, int index, bool visible)
 *   sead::SafeString's vtable   main + 0x2a6e6c0 (loadParams_ builds {vtable, "MaterialName"} on its stack)
 *   a unit's materials          [unit + 0xe0] the model resource, its + 0x48 the materials' ResDic: int32 count at + 4,
 *                               16-byte entries from + 0x18, each with its name at + 8 (u16 length, then the text)
 *                               (searchMaterialIndex, main+0x13a1500, walks it); the visibility bits at [unit + 0xf0]
 * The player actor is [[main+0x2ca1140]+0x60] (world_survey.c's world matrix chain, Bloom's 1.6.0 research); an actor's
 * name is the text at [actor + 0x10] (ksys::act::BaseProc::mName, a FixedSafeString after the vtable).
 * The hair is not in Link's model: the head armour actor draws it (Armor_Default_Head when nothing is worn on the head;
 * user 2026-11-19 with the head hidden in third person: "头发在 脸没有了"). As BetterVR hides every material of the
 * head-gear actor, the actor named Armor_*_Head that Link's actor points at has all its materials hidden too; that
 * pointer's place in his actor is found by name once and read every frame (a change of head gear is a new actor).
 * Called on the game's own thread, from the world-camera update (botw_vr_camera.h), every frame: hidden while first
 * person is on, shown again once when it goes off.
 * Breath of the Wild 1.9.0: not found yet, so nothing is hidden there. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>

#ifdef GAME_BOTW160
enum {
    BOTW_HEAD_PLAYER_ROOT = 0x2ca1140, BOTW_HEAD_PLAYER_STEP = 0x60, BOTW_HEAD_ACTOR_NAME = 0x10,
    BOTW_HEAD_ACTOR_MODEL = 0x4e0, BOTW_HEAD_MODEL_UNIT_COUNT = 0x38, BOTW_HEAD_MODEL_UNITS = 0x40,
    BOTW_HEAD_UNIT_RES = 0xe0, BOTW_HEAD_RES_MATERIALS = 0x48,
    BOTW_HEAD_SEARCH_MATERIAL = 0xc0, BOTW_HEAD_SET_MATERIAL_VISIBLE = 0x1b0,
    BOTW_HEAD_SAFE_STRING_VTABLE = 0x2a6e6c0,
    BOTW_HEAD_MAX_UNITS = 16, BOTW_HEAD_MAX_MATERIALS = 128, BOTW_HEAD_MAX_HITS = 256,
    BOTW_HEAD_PLAYER_SCAN = 0x3000,   /* the bytes of Link's actor looked through for the head gear's pointer */
};
/* The player model's head materials, the names BetterVR hides (the face, eyes and lashes; the hair is the head gear's). */
static const char *const botw_head_materials[] = {"Mt_Face", "Mt_Head", "Mt_Eyeball_L", "Mt_Eyeball_R", "Mt_Eyelashes"};
typedef struct { const void *vtable; const char *text; } botw_safe_string;   /* sead::SafeStringBase<char> */
typedef int32_t (*botw_search_material_fn)(void *unit, const botw_safe_string *name);
typedef void (*botw_set_material_visible_fn)(void *unit, int32_t index, bool visible);
typedef struct {
    uintptr_t actor;                                  /* the actor the hits belong to (0: none resolved) */
    int hits, hidden;
    struct { void *unit; int32_t index; } hit[BOTW_HEAD_MAX_HITS];
} botw_head_group;
static botw_head_group botw_head, botw_headgear;      /* Link's own model; the head gear's (all of its materials) */

static int botw_head_read(uintptr_t address, void *out, size_t size) {
    if (!address) return 0;
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)address, size, (vm_address_t)out, &got) == KERN_SUCCESS && got == size;
}
static uintptr_t botw_head_ptr(uintptr_t address) { uintptr_t p = 0; return botw_head_read(address, &p, sizeof p) ? p : 0; }
static uintptr_t botw_head_player(const uint8_t *main) {
    uintptr_t root = botw_head_ptr((uintptr_t)main + BOTW_HEAD_PLAYER_ROOT);
    return root ? botw_head_ptr(root + BOTW_HEAD_PLAYER_STEP) : 0;
}
static void *botw_head_vfn(void *unit, unsigned slot) {
    uintptr_t vtable = botw_head_ptr((uintptr_t)unit);
    return vtable ? (void *)botw_head_ptr(vtable + slot) : NULL;
}
/* An actor's name (ksys::act::BaseProc::mName), or "" when it does not read as one. */
static void botw_head_actor_name(uintptr_t actor, char out[64]) {
    out[0] = 0;
    uintptr_t text = actor ? botw_head_ptr(actor + BOTW_HEAD_ACTOR_NAME) : 0;
    if (!text || !botw_head_read(text, out, 63)) { out[0] = 0; return; }
    out[63] = 0;
    for (int i = 0; out[i]; i++) if (out[i] < 0x20 || out[i] > 0x7e) { out[0] = 0; return; }
}
/* The head gear: an actor named Armor_*_Head, or the names TOTK_VR_HEADGEAR lists (comma-separated; a test of which
   actor draws the hair, 2026-11-19). */
static int botw_head_is_headgear(const char *name) {
    static const char *list; static int looked;
    if (!looked) { looked = 1; list = getenv("TOTK_VR_HEADGEAR"); if (list && !*list) list = NULL; }
    size_t n = strlen(name);
    if (list) {
        for (const char *at = list; *at;) {
            const char *end = strchr(at, ','); size_t k = end ? (size_t)(end - at) : strlen(at);
            if (k == n && !strncmp(at, name, n)) return 1;
            at += k + (end ? 1 : 0);
        }
        return 0;
    }
    return !strncmp(name, "Armor_", 6) && n > 11 && !strcmp(name + n - 5, "_Head");
}
/* The model units of an actor; their number (0 when the model does not read). */
static int botw_head_units(uintptr_t actor, void *out[BOTW_HEAD_MAX_UNITS]) {
    uintptr_t model = botw_head_ptr(actor + BOTW_HEAD_ACTOR_MODEL), units = 0;
    int32_t count = 0;
    if (!model || !botw_head_read(model + BOTW_HEAD_MODEL_UNIT_COUNT, &count, 4) ||
        !(units = botw_head_ptr(model + BOTW_HEAD_MODEL_UNITS)) || count <= 0 || count > BOTW_HEAD_MAX_UNITS) return 0;
    int n = 0;
    for (int u = 0; u < count; u++) { void *unit = (void *)botw_head_ptr(botw_head_ptr(units + 8u * (uintptr_t)u)); if (unit) out[n++] = unit; }
    return n;
}
static void botw_head_add(botw_head_group *g, void *unit, int32_t index) {
    if (index >= 0 && index < 0xffff && g->hits < BOTW_HEAD_MAX_HITS) { g->hit[g->hits].unit = unit; g->hit[g->hits].index = index; g->hits++; }
}
/* Link's head materials, by name. */
static void botw_head_resolve(const uint8_t *main, uintptr_t actor) {
    botw_head.actor = actor; botw_head.hits = 0; botw_head.hidden = 0;
    void *units[BOTW_HEAD_MAX_UNITS]; int n = botw_head_units(actor, units);
    const botw_safe_string vt_only = {main + BOTW_HEAD_SAFE_STRING_VTABLE, NULL};
    for (int u = 0; u < n; u++) {
        botw_search_material_fn search = (botw_search_material_fn)botw_head_vfn(units[u], BOTW_HEAD_SEARCH_MATERIAL);
        if (!search) continue;
        for (size_t k = 0; k < sizeof botw_head_materials / sizeof *botw_head_materials; k++) {
            botw_safe_string name = vt_only; name.text = botw_head_materials[k];
            botw_head_add(&botw_head, units[u], search(units[u], &name));
        }
    }
    char name[64]; botw_head_actor_name(actor, name);
    fprintf(stderr, "[vr] first person: %d of Link's head materials found in %d model unit(s) (actor %#lx \"%s\")\n",
            botw_head.hits, n, (unsigned long)actor, name);
}
/* An actor's materials into g (when g is not NULL) and their names into names (for the log); the number of units. */
static int botw_head_materials_of(uintptr_t actor, botw_head_group *g, char *names, size_t size) {
    void *units[BOTW_HEAD_MAX_UNITS]; int n = botw_head_units(actor, units);
    size_t used = 0; if (size) names[0] = 0;
    for (int u = 0; u < n; u++) {
        uintptr_t dic = botw_head_ptr(botw_head_ptr((uintptr_t)units[u] + BOTW_HEAD_UNIT_RES) + BOTW_HEAD_RES_MATERIALS);
        int32_t count = 0;
        if (!dic || !botw_head_read(dic + 4, &count, 4) || count <= 0 || count > BOTW_HEAD_MAX_MATERIALS) continue;
        for (int32_t i = 0; i < count; i++) {
            if (g) botw_head_add(g, units[u], i);
            uintptr_t text = botw_head_ptr(dic + 0x18 + 16u * (uintptr_t)i + 8); uint16_t len = 0; char buf[40];
            if (text && botw_head_read(text, &len, 2) && len && len < sizeof buf && botw_head_read(text + 2, buf, len) && used + len + 2 < size) {
                buf[len] = 0; used += (size_t)snprintf(names + used, size - used, "%s%s", used ? " " : "", buf);
            }
        }
    }
    return n;
}
/* Every material of the head gear's model (their names logged once per actor). */
static void botw_headgear_resolve(uintptr_t actor) {
    botw_headgear.actor = actor; botw_headgear.hits = 0; botw_headgear.hidden = 0;
    char names[512]; int n = botw_head_materials_of(actor, &botw_headgear, names, sizeof names);
    char name[64]; botw_head_actor_name(actor, name);
    fprintf(stderr, "[vr] first person: head gear \"%s\": %d material(s) in %d unit(s): %s\n", name, botw_headgear.hits, n, names);
}
/* The head gear actor reached from Link's actor: directly (a pointer in his first 64 KB) or through one object in
   between (a pointer in his first 12 KB, then one in that object's first 1 KB). The path is found by the actor's name
   once, then read again each frame; looked for again about every second while there is none. */
enum { BOTW_HEAD_SCAN_DIRECT = 0x10000, BOTW_HEAD_SCAN_OUTER = 0x10000, BOTW_HEAD_SCAN_INNER = 0x400,
       BOTW_HEAD_MAIN_SPAN = 0x2f00000 };   /* main's image: an actor's vtable is in it */
static intptr_t botw_headgear_path[2] = {-1, -1};   /* [0] in Link's actor, [1] in the object there (-1: direct) */
static uintptr_t botw_headgear_follow(uintptr_t player) {
    if (botw_headgear_path[0] < 0) return 0;
    uintptr_t p = botw_head_ptr(player + (uintptr_t)botw_headgear_path[0]);
    if (p && botw_headgear_path[1] >= 0) p = botw_head_ptr(p + (uintptr_t)botw_headgear_path[1]);
    return p;
}
static int botw_head_heap_ptr(uintptr_t p) { return p >= 0x100000000ull && p < 0x800000000000ull && !(p & 7); }
static uintptr_t botw_head_main;   /* main's base: an object whose first word is not in main's image is no actor */
static int botw_head_maybe_actor(uintptr_t p) {
    uintptr_t vt = botw_head_heap_ptr(p) ? botw_head_ptr(p) : 0;
    return vt >= botw_head_main && vt < botw_head_main + BOTW_HEAD_MAIN_SPAN;
}
/* An actor pool reached from Link's actor (2026-11-19: the object at his + 0x2420 holds every actor, one 0x58-byte
   record each, the actor at + 0x48): walked by its stride for the head gear, up to 1 MB. */
enum { BOTW_HEAD_POOL_BYTES = 0x100000 };
static struct { intptr_t off, first, stride; } botw_headgear_pool = {-1, 0, 0};
static uintptr_t botw_headgear_walk(uintptr_t player, uintptr_t off, uintptr_t first, uintptr_t stride) {
    static uint8_t pool[BOTW_HEAD_POOL_BYTES];
    uintptr_t mid = botw_head_ptr(player + off); size_t have = sizeof pool; char name[64];
    if (!botw_head_heap_ptr(mid)) return 0;
    while (have >= 0x1000 && !botw_head_read(mid, pool, have)) have /= 2;
    if (have < 0x1000) return 0;
    static int said_walk; const int said = said_walk; said_walk = 1;   /* the armour actors are logged on the first walk only */
    for (uintptr_t in = first; in + 8 <= have; in += stride) {
        uintptr_t p; memcpy(&p, pool + in, 8);
        if (p == player || !botw_head_maybe_actor(p)) continue;
        botw_head_actor_name(p, name);
        if (!said && !strncmp(name, "Armor_", 6)) {   /* each armour actor in the pool and its materials, once */
            char names[512]; int units = botw_head_materials_of(p, NULL, names, sizeof names);
            fprintf(stderr, "[vr] first person: pool actor \"%s\" (record + %#lx): %d unit(s): %s\n", name, (unsigned long)in, units, names);
        }
        if (botw_head_is_headgear(name)) {
            fprintf(stderr, "[vr] first person: head gear \"%s\" in the actor pool at Link's actor + %#lx, record + %#lx\n", name, (unsigned long)off, (unsigned long)in);
            botw_headgear_path[0] = (intptr_t)off; botw_headgear_path[1] = (intptr_t)in; return p;
        }
    }
    return 0;
}
static uintptr_t botw_headgear_find(uintptr_t player) {
    char name[64];
    uintptr_t p = botw_headgear_follow(player);
    botw_head_actor_name(p, name);
    if (botw_head_is_headgear(name)) return p;
    static uint64_t tries;
    if (tries++ % 60) return 0;   /* look again about every second */
    if (botw_headgear_pool.off >= 0)   /* the pool it was in: walked again (a change of head gear is a new actor) */
        return botw_headgear_walk(player, (uintptr_t)botw_headgear_pool.off, (uintptr_t)botw_headgear_pool.first, (uintptr_t)botw_headgear_pool.stride);
    static uint8_t outer[BOTW_HEAD_SCAN_DIRECT], inner[BOTW_HEAD_SCAN_INNER];
    static int deep_done;   /* the deep look costs many reads: once a session */
    if (deep_done) return 0;
    deep_done = 1;
    size_t have = BOTW_HEAD_SCAN_DIRECT;
    while (have >= 0x1000 && !botw_head_read(player, outer, have)) have /= 2;
    if (have < 0x1000) return 0;
    for (uintptr_t off = 0; off + 8 <= have; off += 8) {
        memcpy(&p, outer + off, 8);
        if (!botw_head_maybe_actor(p)) continue;
        botw_head_actor_name(p, name);
        if (botw_head_is_headgear(name)) {
            fprintf(stderr, "[vr] first person: head gear \"%s\" at Link's actor + %#lx\n", name, (unsigned long)off);
            botw_headgear_path[0] = (intptr_t)off; botw_headgear_path[1] = -1; return p;
        }
    }
    for (uintptr_t off = 0; off + 8 <= BOTW_HEAD_SCAN_OUTER && off + 8 <= have; off += 8) {
        uintptr_t mid; memcpy(&mid, outer + off, 8);
        if (!botw_head_heap_ptr(mid) || !botw_head_read(mid, inner, sizeof inner)) continue;
        intptr_t at[2] = {-1, -1};   /* the first two actors in the object: a pool when they repeat at a stride */
        for (uintptr_t in = 0; in + 8 <= sizeof inner; in += 8) {
            memcpy(&p, inner + in, 8);
            if (p == player || !botw_head_maybe_actor(p)) continue;
            botw_head_actor_name(p, name);
            if (botw_head_is_headgear(name)) {
                fprintf(stderr, "[vr] first person: head gear \"%s\" at Link's actor + %#lx, then + %#lx\n", name, (unsigned long)off, (unsigned long)in);
                botw_headgear_path[0] = (intptr_t)off; botw_headgear_path[1] = (intptr_t)in; return p;
            }
            if (at[0] < 0) at[0] = (intptr_t)in; else if (at[1] < 0) at[1] = (intptr_t)in;
        }
        const intptr_t stride = at[1] - at[0];
        if (at[1] > 0 && stride >= 0x20 && stride <= 0x200 && (uintptr_t)(at[0] + 2 * stride) + 8 <= sizeof inner) {
            memcpy(&p, inner + at[0] + 2 * stride, 8);
            if (!botw_head_maybe_actor(p)) continue;   /* a third one at the same stride: a pool */
            if ((p = botw_headgear_walk(player, off, (uintptr_t)at[0], (uintptr_t)stride))) {
                botw_headgear_pool.off = (intptr_t)off; botw_headgear_pool.first = at[0]; botw_headgear_pool.stride = stride;
                return p;
            }
        }
    }
    fprintf(stderr, "[vr] first person: no head gear found from Link's actor (%zu bytes looked through)\n", have);
    return 0;
}
static void botw_head_set(botw_head_group *g, bool visible) {
    for (int i = 0; i < g->hits; i++) {
        botw_set_material_visible_fn set = (botw_set_material_visible_fn)botw_head_vfn(g->hit[i].unit, BOTW_HEAD_SET_MATERIAL_VISIBLE);
        if (set) set(g->hit[i].unit, g->hit[i].index, visible);
    }
}
/* Shown again, when the actor is still the one the hits came from (a gone actor is not touched). */
static void botw_head_show(botw_head_group *g, uintptr_t now) {
    if (g->hidden && now && now == g->actor) botw_head_set(g, true);
    g->hidden = 0;
}
/* Every world-camera update: hide = first person is on. TOTK_VR_HIDE_HEAD=0 leaves the head alone; =always hides it in
   third person too, to look at what is hidden from outside (a test, user 2026-11-19: "在第三人称也去掉"). */
static void botw_vr_head_update(const uint8_t *main, int hide) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VR_HIDE_HEAD"); on = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "always") ? 2 : 1; }
    if (!on || !main) return;
    if (on == 2) hide = 1;
    botw_head_main = (uintptr_t)main;
    uintptr_t actor = botw_head_player(main);
    uintptr_t gear = actor && (hide || botw_headgear.hidden) ? botw_headgear_find(actor) : 0;
    if (!hide) {
        if (botw_head.hidden || botw_headgear.hidden) fprintf(stderr, "[vr] first person off: Link's head shown\n");
        botw_head_show(&botw_head, actor); botw_head_show(&botw_headgear, gear);
        return;
    }
    if (!actor) return;
    if (actor != botw_head.actor) botw_head_resolve(main, actor);
    if (gear != botw_headgear.actor) { botw_head_show(&botw_headgear, 0); if (gear) botw_headgear_resolve(gear); else botw_headgear.actor = 0, botw_headgear.hits = 0; }
    /* each frame: an animation or a reload may show them again */
    botw_head_set(&botw_head, false); botw_head_set(&botw_headgear, false);
    if (!botw_head.hidden && botw_head.hits) fprintf(stderr, "[vr] first person: Link's head hidden (%d materials, head gear %d)\n", botw_head.hits, botw_headgear.hits);
    botw_head.hidden = botw_head.hits > 0; botw_headgear.hidden = botw_headgear.hits > 0;
}
#else
static void botw_vr_head_update(const uint8_t *main, int hide) { (void)main; (void)hide; }
#endif
