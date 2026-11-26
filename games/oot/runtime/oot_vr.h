/* zeldaVR for Ocarina of Time on the Mac (2026-11-22, user: "开始开发 oot的vr mac版本 参考这个实现
   https://github.com/ReTracedGames/OcarinaVR"; the user's choices: third person by default, stereo later by depth
   reprojection). Only with TOTK_VR_CONTROL set, never in an iOS export. Included by game_patches_ns2.c after
   patch_oot_aspect (it uses oot_aspect_cell).

   Where the data is (update 131072 = 1.1.1, the version OcarinaVR / Hookshot bbdc370 name; checked against our image,
   vr研究.md 2026-11-22): the game builds its gameplay view in main+0x4c48c0; at main+0x4c5068 its 3x4 view matrix
   (rows R|t) has just been written at view +8 (x20 the view, vtable main+0x4bf8a88; x19 its camera). The gameplay view
   is camera 0's of the camera manager: [[main+0x4c86bc0]] -> +32 count, +40 list -> camera 0 -> +120. The matrix is
   computed afresh from the camera's eye and target every frame, so turning it does not feed back into the game's
   camera logic: the head's rotation h (rows of R^T, written by totk_vr.m) is applied here, as BotW's adapter does
   (games/totk/runtime/botw_vr_camera.h), and the game's culling, shadows and level of detail then follow the head.
   The field of view: the camera's vertical angle (camera +0x13c) is read for its projection at main+0x4c52f8; the
   veneer there takes the headset's angle from the data page (runtime_aspect_vr), the aspect from the existing aspect
   cell. Only ideas and addresses come from OcarinaVR (GPL-3); no code. */
#pragma once
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "runtime.h"
#include "oot_vr_veneer.h"
#include "oot_vr_hooks.h"
#include "../zeldaVR/runtime/vr_camera.h"
#include "../zeldaVR/runtime/vr_view_math.h"
#include "../zeldaVR/runtime/oot_vr_view.h"
#include "../zeldaVR/runtime/oot_gamedata.h"
#include "../zeldaVR/runtime/oot_vr_comfort.h"

enum {
    OOT_VR_FOV_CELL = 72,         /* data page: the headset's vertical field of view (float radians), 0 = the game's */
    OOT_VR_FN_CELL = 80,          /* data page: the camera call's host function (8 bytes) */
    OOT_VR_PLAYER_FN_CELL = 88,   /* data page: the player update's host function */
    OOT_VR_FILL_FN_CELL = 96,     /* data page: the view fill's host function */
    OOT_VR_PLACE_FN_CELL = 104,   /* data page: an actor's new place, the host function */
    OOT_VR_LOD_CELL = 112,        /* data page: the factor on the views' projected-size scale (float; 1 = the game's) */
    OOT_VR_CALL_STUB = 512,       /* text padding after the time-step and resolution veneers (they end at +372) */
    OOT_VR_FOV_STUB = 512 + 256,
    OOT_VR_PLAYER_STUB = 512 + 256 + 32,
    OOT_VR_FILL_STUB = 512 + 256 + 32 + 256,
    OOT_VR_PLACE_STUB = 512 + 256 + 32 + 256 + 256,
    OOT_VR_LOD_STUB = 512 + 256 + 32 + 256 + 256 + 256,
    OOT_VR_LOD_WORDS = 7,
    OOT_VR_HOOK_FN_CELL = 120,    /* data page: the shared hook body's host function (oot_vr_hooks.h) */
    OOT_VR_HOOK_BODY = 1600,      /* text padding: the shared body, then five words a site */
    OOT_VR_HOOK_STUBS = 1600 + OOT_HOOK_BODY_WORDS * 4,
};
static const uint64_t OOT_VR_CAMERA_SITE = 0x4c5068, OOT_VR_FOV_SITE = 0x4c52f8;
static const uint32_t OOT_VR_CAMERA_WORD = 0xf9401268u;   /* ldr x8, [x19, #0x20] */
static const uint32_t OOT_VR_FOV_WORD = 0xbd413e60u;      /* ldr s0, [x19, #0x13c] */
/* Link's movement update (x19 the actor), just before it tests its own flags; the view fill (x28 the view being filled for
   the GPU), right after it picked the view. Neither displaced instruction depends on the pc. */
static const uint64_t OOT_VR_PLAYER_SITE = 0x2c21728, OOT_VR_FILL_SITE = 0x6bc5b4;
static const uint32_t OOT_VR_PLAYER_WORD = 0xf9000beau;   /* str x10, [sp, #0x10] */
static const uint32_t OOT_VR_FILL_WORD = 0x3951ab89u;     /* ldrb w9, [x28, #0x46a] */
/* An actor takes its new place (oot_vr_sword.h): x22 the actor, x21 the matrix. */
static const uint64_t OOT_VR_PLACE_SITE = 0x137478;
static const uint32_t OOT_VR_PLACE_WORD = 0x2d4106a0u;    /* ldp s0, s1, [x21, #8] */
static const uint64_t OOT_VR_VIEW_VTABLE = 0x4bf8a88, OOT_VR_CAMERA_MANAGER = 0x4c86bc0;
static const uint64_t OOT_VR_ACTOR_VTABLE = 0x4b25e18, OOT_VR_PLAYER_VTABLE = 0x4b4c460, OOT_VR_EVENT_SLOT = 0x4c7e090;
static const uint64_t OOT_VR_GAMEDATA_SLOT = 0x4c7e180;   /* the saved flags' manager: *(*slot) (oot_gamedata.h) */

/* Level of detail (2026-11-25). The game picks a model's level, and drops small ones, by its projected size: bounding
   radius x (1 / tan(half the vertical angle)) / view depth (main+0x863e0..0x86434 and its copies at 0x7defc, 0x84150,
   0x1214d8c and hundreds in the sorters from main+0x3608e98), the scale read from the render view at +0x1e0. The render
   view is built on the stack at main+0x33ab..: tan at +0x1dc, then `fdiv s2, s1, s0` and at main+0x33af7c
   `str s2, [x19, #0x1e0]`, s2 loaded again by the next instruction. (main+0x33edf0 and 0x340e70 write the same field of
   other views; scaling those changed nothing here.) At the headset's 113 degrees the scale is 0.66 where the game's own
   50 degrees give 2.14, so everything counted as three times smaller: the stairs to the temple were drawn with a coarse
   level from 10 m, and with the fine one again when the head turned them off the view's axis (the depth shrinks).
   The stub multiplies the stored scale by the factor on the data page, tan(half the headset's angle) / tan(half the
   game's own), so the levels are the ones the game picks without VR at that depth. OOT_VR_LOD=0 leaves the game's
   scale; "flag vr_lodk <thousandths>" sets the factor by hand (0: computed again).
   Correction to the commit that added this (803c5e68): its message says the VR session reports handheld mode to the
   game, whose LodStartDistCoef default is 0.8. It does not. The log has "handheld" once, when the VR canvas is set, and
   "docked" right after it: the mode is flipped and flipped back so that the game rebuilds its picture at the new size.
   The game runs docked, with the console's coefficient 1.0, and nothing is left to do there. */
static const uint64_t OOT_VR_LOD_SITES[1] = { 0x33af7c };
static const uint32_t OOT_VR_LOD_WORD = 0xbd01e262u;      /* str s2, [x19, #0x1e0] */
static const uint32_t OOT_VR_LOD_NEXT = 0xbd41d662u;      /* ldr s2, [x19, #0x1d4]: s2 is loaded again */
static _Atomic uint32_t *oot_vr_lod_cell, *oot_vr_fov_cell;   /* the second is defined below, with the other cells */
static _Atomic int oot_vr_lod_forced;                     /* thousandths, from the flag; 0 = computed */
static float oot_vr_lod_ref;                              /* the game's own vertical angle while it is not aiming, radians */
static void oot_vr_lod_update(float game_fov) {
    if (!oot_vr_lod_cell) return;
    if (game_fov * 57.2957795f >= 46.5f && game_fov < 3.0f) oot_vr_lod_ref = game_fov;   /* it narrows the angle to aim: not the reference */
    float k = 1, vr = 0; uint32_t bits = oot_vr_fov_cell ? atomic_load(oot_vr_fov_cell) : 0; memcpy(&vr, &bits, 4);
    const int forced = atomic_load(&oot_vr_lod_forced);
    if (forced > 0) k = (float)forced / 1000.0f;
    else if (bits && vr > 0.1f && vr < 3.0f && oot_vr_lod_ref > 0.1f) k = tanf(vr / 2) / tanf(oot_vr_lod_ref / 2);
    if (!(k >= 0.25f && k <= 16.0f)) k = 1;
    memcpy(&bits, &k, 4);
    if (atomic_exchange(oot_vr_lod_cell, bits) != bits) fprintf(stderr, "[vr] OOT level of detail: projected sizes x%.3f (the headset's angle against the game's own)\n", (double)k);
}
static vr_cam_state oot_vr_cam;
static uint8_t *oot_vr_base;
static _Atomic uint32_t *oot_vr_fov_cell;
static pthread_mutex_t oot_vr_mu = PTHREAD_MUTEX_INITIALIZER;
static float oot_vr_base_view[12];
static int oot_vr_base_valid;
static uint32_t oot_vr_aspect_saved;   /* the player's ratio while VR set its own */
static int oot_vr_aspect_owned;

/* Link, from his own update (any worker thread). */
static pthread_mutex_t oot_vr_player_mu = PTHREAD_MUTEX_INITIALIZER;
static float oot_vr_feet[3];
static uint64_t oot_vr_feet_at;
static _Atomic int oot_vr_in_event, oot_vr_is_child = 1;
/* First person. */
static _Atomic uint32_t oot_vr_yaw_bits;          /* the heading, float bits */
static _Atomic int oot_vr_fp_on;                  /* the view is at his eyes */
static _Atomic uint32_t oot_vr_move_turn_bits;    /* third person: the head's turn the left stick must not follow */
/* Diagnostics for a picture that is wrong at some head poses (2026-11-24, the stairs to the temple): "flag vr_noturn 1"
   leaves the head out of the gameplay view; "flag vr_fovdeg <degrees>" replaces the headset's vertical field of view in
   the game's projection (0: the headset's again). Both from TOTK_INPUT_FILE, off unless asked for. */
static _Atomic int oot_vr_dbg_noturn;
static _Atomic uint32_t oot_vr_dbg_fov_saved;
void runtime_oot_vr_blade_kind(int kind);   /* oot_vr_sword.h */
void runtime_oot_vr_blade_suspend(int on);
void runtime_oot_vr_debug(const char *name, int v) {
    if (!strcmp(name, "vr_blade_kind")) { runtime_oot_vr_blade_kind(v); return; }
    if (!strcmp(name, "vr_blade_suspend")) { runtime_oot_vr_blade_suspend(v); return; }
    if (!strcmp(name, "vr_lodk")) { atomic_store(&oot_vr_lod_forced, v > 0 ? v : 0); fprintf(stderr, "[vr] [flag] vr_lodk = %d thousandths (0: computed)\n", v); return; }
    if (!strcmp(name, "vr_noturn")) { atomic_store(&oot_vr_dbg_noturn, v != 0); fprintf(stderr, "[vr] [flag] vr_noturn = %d\n", v != 0); return; }
    if (!oot_vr_fov_cell) return;
    if (v > 0) { if (!atomic_load(&oot_vr_dbg_fov_saved)) atomic_store(&oot_vr_dbg_fov_saved, atomic_load(oot_vr_fov_cell));
                 float f = (float)v / 57.2957795f; uint32_t bits; memcpy(&bits, &f, 4); atomic_store(oot_vr_fov_cell, bits); }
    else if (atomic_load(&oot_vr_dbg_fov_saved)) { atomic_store(oot_vr_fov_cell, atomic_exchange(&oot_vr_dbg_fov_saved, 0)); }
    fprintf(stderr, "[vr] [flag] vr_fovdeg = %d (0: the headset's)\n", v);
}
static pthread_mutex_t oot_vr_cull_mu = PTHREAD_MUTEX_INITIALIZER;
static oot_cull_ring oot_vr_cull;
static _Atomic int oot_vr_cull_live, oot_vr_fill_seen;
static _Atomic uint64_t oot_vr_fill_restored;
static _Atomic uint32_t oot_vr_game_fov_bits;     /* the game's own vertical angle, radians: it narrows it to aim */
static _Atomic uint32_t oot_vr_game_pitch_bits;   /* how far up the game's own camera looks, radians (the VR view is levelled) */
static _Atomic uint64_t oot_vr_event_since;       /* since when a scene of the game has been playing, 0 none */

static uint64_t oot_vr_load64(uintptr_t a) { uint64_t v; memcpy(&v, (const void *)a, 8); return v; }
static uint32_t oot_vr_float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float oot_vr_bits_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint64_t oot_vr_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

int runtime_oot_vr_child(void) { return atomic_load(&oot_vr_is_child); }
void runtime_oot_vr_set_child(int child) { atomic_store(&oot_vr_is_child, child != 0); }
int runtime_oot_vr_fp_active(void) { return atomic_load(&oot_vr_fp_on); }
int runtime_oot_vr_in_event(void) { return atomic_load(&oot_vr_in_event); }
float runtime_oot_vr_game_fov_deg(void) { return oot_vr_bits_float(atomic_load(&oot_vr_game_fov_bits)) * 57.2957795f; }
float runtime_oot_vr_game_pitch(void) { return oot_vr_bits_float(atomic_load(&oot_vr_game_pitch_bits)); }
float runtime_oot_vr_move_turn(void) { return oot_vr_bits_float(atomic_load(&oot_vr_move_turn_bits)); }
void runtime_oot_vr_yaw_add(float radians) {
    if (!isfinite(radians)) return;
    uint32_t have = atomic_load(&oot_vr_yaw_bits), want;
    do want = oot_vr_float_bits(oot_wrap_pi(oot_vr_bits_float(have) + radians));
    while (!atomic_compare_exchange_weak(&oot_vr_yaw_bits, &have, want));
}
/* The game's up / down is the headset's while the world's view is levelled (input.c takes it off the right stick); in menus
   and the map, where the world camera stops, the stick works as it does. */
int runtime_vr_stick_pitch_locked(void) {
    if (!g_vr_headlook || !__atomic_load_n(&oot_vr_cam.on, __ATOMIC_ACQUIRE) || !__atomic_load_n(&oot_vr_cam.level, __ATOMIC_RELAXED)) return 0;
    { const float aim = runtime_oot_vr_game_fov_deg(); extern int runtime_oot_vr_aiming(void);
      if ((aim > 1 && aim < 46.5f) || runtime_oot_vr_aiming()) return 0; }   /* aiming: the stick raises the shot */
    static _Atomic uint32_t last; static _Atomic uint64_t at;
    const uint32_t c = __atomic_load_n(&oot_vr_cam.calls, __ATOMIC_RELAXED);
    const uint64_t now = oot_vr_now();
    if (c != atomic_exchange(&last, c)) atomic_store(&at, now);
    return now - atomic_load(&at) < 200000000ull;
}
/* Where Link's feet are, and how old that is. 0 before his first update. */
int runtime_oot_vr_feet(float out[3], uint64_t *age_ns) {
    pthread_mutex_lock(&oot_vr_player_mu);
    const uint64_t at = oot_vr_feet_at;
    if (at) memcpy(out, oot_vr_feet, sizeof oot_vr_feet);
    pthread_mutex_unlock(&oot_vr_player_mu);
    if (!at) return 0;
    *age_ns = oot_vr_now() - at;
    return 1;
}

#include "oot_vr_sword.h"

/* What the game is doing, for the VR side (2026-11-25, after HookshotVR's list of what a VR player needs; facts from its notes,
   vr研究.md): the saved hearts, magic and rupees for the wrist panel; whether Link crawls (the player object's +0x1f6c bit
   0x800 and its view height at +0x3c90, 0.91 standing and 0.29 in a crawl); what kind of scene plays (the event manager's
   +104 holder, its name at +8). All read on Link's own update through a reader that refuses a stale pointer. */
static pthread_mutex_t oot_vr_status_mu = PTHREAD_MUTEX_INITIALIZER;
static oot_status oot_vr_status_now;
static _Atomic uint32_t oot_vr_crawl_bits;
static _Atomic uint32_t oot_vr_action_now;   /* Link's action state (the player object's +0x1f60) */
/* The game aims the slingshot or the bow: its aim camera is on (bit 0x2000000) and the low byte says raising or drawn
   (0x42, 0x46; run new2: 0x2000442 then 0x2000446, the shot 0x40000443, resting 0x441). */
int runtime_oot_vr_aiming(void) { const uint32_t a = atomic_load(&oot_vr_action_now); return (a & 0x2000000u) && ((a & 0xff) == 0x42 || (a & 0xff) == 0x46); }
/* The player holds a bow or slingshot in two hands these last frames (the graphics side's report): its shot then goes along the
   hands (oot_shot_launch), and the view keeps the player's own heading instead of the game's aim. */
int runtime_oot_vr_two_hands_held(void) {
    pthread_mutex_lock(&oot_two_hands.mu); const uint64_t at = oot_two_hands.at; pthread_mutex_unlock(&oot_two_hands.mu);
    return at && oot_vr_now() - at < 150000000ull;
}
static _Atomic int oot_vr_event_kind_now, oot_vr_events_keep_eyes = 1;
static int oot_vr_safe_read(uintptr_t at, void *out, size_t size) { return oot_scan_read(at, out, size); }
int runtime_oot_vr_status(oot_status *out) {
    pthread_mutex_lock(&oot_vr_status_mu); *out = oot_vr_status_now; pthread_mutex_unlock(&oot_vr_status_mu);
    return out->have;
}
float runtime_oot_vr_crawl(void) { return oot_vr_bits_float(atomic_load(&oot_vr_crawl_bits)); }
int runtime_oot_vr_event_kind(void) { return atomic_load(&oot_vr_event_kind_now); }
/* The VR page: 1 (default) the player keeps his own eyes through talks, signs, doors and arrivals; 0 the game's camera takes every scene. */
void runtime_oot_vr_events_keep_eyes(int on) { atomic_store(&oot_vr_events_keep_eyes, on != 0); }
static void oot_vr_state_read(uintptr_t player, uintptr_t events) {
    { uint32_t action = 0; memcpy(&action, (const void *)(player + 0x1f60), 4); atomic_store(&oot_vr_action_now, action); }
    {   /* the crawl */
        uint32_t flags = 0; float height = 0;
        memcpy(&flags, (const void *)(player + 0x1f6c), 4); memcpy(&height, (const void *)(player + 0x3c90), 4);
        const int crawling = (flags & 0x800) && height > 0 && height < 0.6f;
        const uint32_t was = atomic_exchange(&oot_vr_crawl_bits, crawling ? oot_vr_float_bits(height) : 0);
        if (!was != !crawling) fprintf(stderr, "[vr] OOT: Link %s (flags %#x, the game's view height %.2f)\n", crawling ? "crawls" : "is out of the crawl space", flags, (double)height);
    }
    {   /* the scene */
        char name[64] = ""; const uintptr_t holder = events ? oot_vr_load64(events + 104) : 0; uint64_t text = 0;
        int kind = OOT_EVENT_NONE;
        if (holder) {
            if (oot_vr_safe_read(holder + 8, &text, 8) && text >= 0x100000000ull) {
                for (int n = 63; n >= 7 && !name[0]; n -= 8) { if (oot_vr_safe_read((uintptr_t)text, name, (size_t)n)) name[n] = 0; else name[0] = 0; }   /* shorter when the text ends at a page's end */
                for (char *c = name; *c; c++) if (*c < 32 || *c > 126) { *c = 0; break; }
            }
            kind = oot_event_class(name);
        }
        static char last[64] = "\1"; static int said;
        if (strcmp(name, last) || kind != atomic_load(&oot_vr_event_kind_now)) {
            if (said < 300) { said++; fprintf(stderr, "[vr-event] %.2f s: %s%s%s (kind %d)\n", (double)oot_vr_now() / 1e9, holder ? "scene \"" : "no scene", holder ? name : "", holder ? "\"" : "", kind); }
            memcpy(last, name, sizeof last);
        }
        atomic_store(&oot_vr_event_kind_now, kind);
    }
    static uint64_t read_at;   /* the saved flags, five times a second */
    const uint64_t now = oot_vr_now();
    if (now - read_at < 200000000ull) return;
    read_at = now;
    uint64_t holder = 0, manager = 0; oot_status s; memset(&s, 0, sizeof s);
    if (oot_vr_safe_read((uintptr_t)oot_vr_base + OOT_VR_GAMEDATA_SLOT, &holder, 8) && holder && oot_vr_safe_read((uintptr_t)holder, &manager, 8) && manager)
        (void)oot_gd_status(oot_vr_safe_read, (uintptr_t)manager, &s);
    pthread_mutex_lock(&oot_vr_status_mu);
    const int changed = memcmp(&s, &oot_vr_status_now, sizeof s) != 0; oot_vr_status_now = s;
    pthread_mutex_unlock(&oot_vr_status_mu);
    static int lines;
    if (changed && lines < 60) { lines++; fprintf(stderr, "[vr] OOT status: %s, life %d of %d quarter hearts, magic %d of %d, rupees %d\n", s.have ? "save read" : "no save", s.life, s.max_life, s.magic, s.max_magic, s.rupees); }
    {   /* the map Link is in, when it changes (the log) */
        static char last_map[OOT_GD_TEXT_BYTES]; static int map_lines; char map[OOT_GD_TEXT_BYTES];
        if (manager && oot_gd_text(oot_vr_safe_read, (uintptr_t)manager, OOT_GD_MAP, map) && strcmp(map, last_map) && map_lines < 40) {
            map_lines++; memcpy(last_map, map, sizeof last_map); fprintf(stderr, "[vr] OOT map: %s\n", map);
        }
    }
}

#include "oot_vr_wheel_host.h"
/* The hook at the entry of a hit sensor's update: for Link's, the blade's attack (oot_vr_sword.h) and then the item wheel's
   calls (oot_vr_wheel_host.h), one chain. */
static oot_guest_call *oot_link_calc(oot_hook_regs *r) {
    const uintptr_t sensor = (uintptr_t)r->x[0];
    if (!sensor || sensor != atomic_load_explicit(&oot_link_sensor, memory_order_relaxed)) return NULL;
    oot_guest_call *head = oot_blade_calc(r), *wheel = oot_wheel_calc();
    if (!head) return wheel;
    oot_guest_call *last = head;
    while (last->next) last = last->next;
    last->next = wheel;
    return head;
}

/* Link's movement update. Other actors run this code too: his own update is told by the class of its actor and of the
   player object reached through its movement component, whose resource is named "Player". */
static void oot_vr_player(uint8_t *actor, uint8_t *unused) {
    (void)unused;
    if (!oot_vr_base || !actor) return;
    const uintptr_t base = (uintptr_t)oot_vr_base, a = (uintptr_t)actor;
    if (oot_vr_load64(a) != base + OOT_VR_ACTOR_VTABLE) return;
    const uintptr_t components = oot_vr_load64(a + 624);
    const uintptr_t movement = components ? oot_vr_load64(components + 800) : 0;
    if (movement < 0x1d50 + 4096 || oot_vr_load64(movement + 24) != a) return;
    const uintptr_t player = movement - 0x1d50;
    if (oot_vr_load64(player) != base + OOT_VR_PLAYER_VTABLE) return;
    const uintptr_t resource = oot_vr_load64(player + 16);
    if (!resource || memcmp((const void *)(resource + 88), "Player", 7)) return;
    float feet[3]; memcpy(feet, actor + 796, sizeof feet);
    if (!isfinite(feet[0]) || !isfinite(feet[1]) || !isfinite(feet[2])) return;
    oot_vr_weapon_note(components);
    oot_blade_link(components, player);
    atomic_store_explicit(&oot_link_actor, a, memory_order_relaxed);
    const uintptr_t holder = oot_vr_load64(base + OOT_VR_EVENT_SLOT), events = holder ? oot_vr_load64(holder) : 0;
    {   /* the log: the event manager's word as it changes (a few lines a scene) */
        static uint64_t last_event = ~0ull; static int said;
        const uint64_t word = events ? oot_vr_load64(events + 104) : 0;
        if (word != last_event && said < 200) { said++; fprintf(stderr, "[vr-event] +104 = %#llx (was %#llx) at %.2f s\n", (unsigned long long)word, (unsigned long long)last_event, (double)oot_vr_now() / 1e9); }
        last_event = word;
    }
    oot_vr_state_read(player, events);
    /* A scene of the game takes the camera after 0.3 s: a flag up for a moment (opening a chest's first frame) keeps the eyes.
       Only the scenes with a camera of their own do (a story scene, an item shown to the player); through a talk, a sign, a
       door or an arrival the player keeps his eyes (runtime_oot_vr_events_keep_eyes 0: every scene, as before 2026-11-25). */
    const int takes = !atomic_load(&oot_vr_events_keep_eyes) || oot_event_takes_view(atomic_load(&oot_vr_event_kind_now));
    if (events && oot_vr_load64(events + 104) != 0 && takes) {
        uint64_t none = 0; const uint64_t now = oot_vr_now();
        atomic_compare_exchange_strong(&oot_vr_event_since, &none, now);
        atomic_store(&oot_vr_in_event, now - atomic_load(&oot_vr_event_since) > 300000000ull);
    } else { atomic_store(&oot_vr_event_since, 0); atomic_store(&oot_vr_in_event, 0); }
    if (pthread_mutex_trylock(&oot_vr_player_mu)) return;
    const int first = !oot_vr_feet_at;
    memcpy(oot_vr_feet, feet, sizeof feet); oot_vr_feet_at = oot_vr_now();
    pthread_mutex_unlock(&oot_vr_player_mu);
    if (first) fprintf(stderr, "[vr] OOT: Link's update found, feet %.2f %.2f %.2f\n", (double)feet[0], (double)feet[1], (double)feet[2]);
}

/* The game fills a view's data for the GPU. If this is the view we moved back for its sorting, the true one goes back. */
static void oot_vr_fill(uint8_t *view, uint8_t *unused) {
    (void)unused;
    atomic_store(&oot_vr_fill_seen, 1);
    if (!view || !atomic_load(&oot_vr_cull_live)) return;
    float v[12], z; memcpy(v, view, sizeof v);
    pthread_mutex_lock(&oot_vr_cull_mu);
    const int found = oot_cull_find(&oot_vr_cull, v, &z);
    pthread_mutex_unlock(&oot_vr_cull_mu);
    if (!found) return;
    memcpy(view + 44, &z, 4);
    if (!atomic_fetch_add(&oot_vr_fill_restored, 1)) fprintf(stderr, "[vr] OOT: the true view goes back where the game fills its views\n");
}

/* The gameplay view: camera 0 of the camera manager (0 while there is none). */
static uintptr_t oot_vr_gameplay_view(void) {
    const uintptr_t singleton = oot_vr_load64((uintptr_t)oot_vr_base + OOT_VR_CAMERA_MANAGER);
    const uintptr_t manager = singleton ? oot_vr_load64(singleton) : 0;
    uint32_t count = 0;
    if (manager) memcpy(&count, (const void *)(manager + 32), 4);
    const uintptr_t cameras = manager && count ? oot_vr_load64(manager + 40) : 0;
    const uintptr_t camera = cameras ? oot_vr_load64(cameras) : 0;
    return camera ? oot_vr_load64(camera + 120) : 0;
}

int runtime_vr_base_view(float out[12]) {
    pthread_mutex_lock(&oot_vr_mu);
    const int valid = oot_vr_base_valid;
    if (valid) memcpy(out, oot_vr_base_view, sizeof oot_vr_base_view);
    pthread_mutex_unlock(&oot_vr_mu);
    return valid;
}

/* Diagnostic log (TOTK_VR_HOOKLOG=1): how the camera hook's calls ended, per second. */
enum { OOT_HOOK_KIND, OOT_HOOK_OTHER_VIEW, OOT_HOOK_OFF, OOT_HOOK_NAN, OOT_HOOK_TURNED, OOT_HOOK_N };
static _Atomic uint64_t oot_vr_hook_ends[OOT_HOOK_N];
static void oot_vr_hook_end(int why, const uint8_t *view) {
    static int on = -1; if (on < 0) on = getenv("TOTK_VR_HOOKLOG") != NULL;
    if (!on) return;
    atomic_fetch_add_explicit(&oot_vr_hook_ends[why], 1, memory_order_relaxed);
    static _Atomic uint64_t at; const uint64_t now = oot_vr_now(), was = atomic_load(&at);
    if (now - was < 1000000000ull || !atomic_compare_exchange_strong(&at, &(uint64_t){was}, now)) return;
    fprintf(stderr, "[vr-hook] other kind %llu, other view %llu, off %llu, not finite %llu, turned %llu; this view %p, gameplay view %p\n",
            (unsigned long long)atomic_exchange(&oot_vr_hook_ends[OOT_HOOK_KIND], 0), (unsigned long long)atomic_exchange(&oot_vr_hook_ends[OOT_HOOK_OTHER_VIEW], 0),
            (unsigned long long)atomic_exchange(&oot_vr_hook_ends[OOT_HOOK_OFF], 0), (unsigned long long)atomic_exchange(&oot_vr_hook_ends[OOT_HOOK_NAN], 0),
            (unsigned long long)atomic_exchange(&oot_vr_hook_ends[OOT_HOOK_TURNED], 0), (const void *)view, (void *)oot_vr_gameplay_view());
}
/* The game's thread, right after it wrote a view's matrix (the veneer at main+0x4c5068). */
static void oot_vr_camera(uint8_t *view, uint8_t *camera) {
    if (!oot_vr_base || !view) return;
    if (oot_vr_load64((uintptr_t)view) != (uintptr_t)oot_vr_base + OOT_VR_VIEW_VTABLE) { oot_vr_hook_end(OOT_HOOK_KIND, view); return; }   /* another kind of view */
    if ((uintptr_t)view != oot_vr_gameplay_view()) { oot_vr_hook_end(OOT_HOOK_OTHER_VIEW, view); return; }                                       /* not the gameplay camera */
    float game_fov = 0;
    if (camera) { memcpy(&game_fov, camera + 0x13c, 4); if (isfinite(game_fov)) atomic_store(&oot_vr_game_fov_bits, oot_vr_float_bits(game_fov)); }
    oot_vr_lod_update(game_fov);
    {   /* the log: the game's own angle as it changes (it narrows it to aim), with Link's action state */
        static float said_deg = -100; static int lines; const float deg = game_fov * 57.2957795f;
        if (fabsf(deg - said_deg) > 1.0f && lines < 200) { lines++; said_deg = deg; fprintf(stderr, "[vr] OOT: the game's own vertical angle is %.1f degrees (Link's action state %#x)\n", (double)deg, atomic_load(&oot_vr_action_now)); }
    }
    { static _Atomic int said; if (!atomic_exchange(&said, 1)) fprintf(stderr, "[vr] OOT: the game's own vertical angle %.2f degrees\n", (double)(game_fov * 57.2957795f)); }
    vr_cam_state control;
    vr_camera_control_read(&control);
    if (!control.on) {
        pthread_mutex_lock(&oot_vr_mu); oot_vr_base_valid = 0; pthread_mutex_unlock(&oot_vr_mu);
        oot_vr_hook_end(OOT_HOOK_OFF, view);
        return;
    }
    float m[12];
    memcpy(m, view + 8, sizeof m);
    for (int i = 0; i < 12; i++) if (!isfinite(m[i]) || !isfinite(control.h[i])) { oot_vr_hook_end(OOT_HOOK_NAN, view); return; }
    for (int i = 0; i < 3; i++) if (!isfinite(control.off[i])) return;
    if (!isfinite(control.cam_back)) return;
    { const float up = -m[9]; atomic_store(&oot_vr_game_pitch_bits, oot_vr_float_bits(asinf(up < -1 ? -1 : up > 1 ? 1 : up))); }   /* the camera looks along -(row 2) */
    if (control.level) vr_view_level(m);
    /* First person: at his eyes while his own update is running and no scene of the game is playing. */
    float feet[3]; uint64_t age = 0;
    const int fp = control.fp_blend > 0 && isfinite(control.fp_up[1]) && runtime_oot_vr_feet(feet, &age) &&
                   age < 500000000ull && !atomic_load(&oot_vr_in_event);
    /* The game aims (its camera narrows for a moment as the bow comes up, 50 -> 43 -> 46.2 degrees and wider again while Link's
       state stays "drawn", run new4: so his state decides): the eyes stay and the game's aim turns them, unless the player holds
       the bow in two hands, whose line the shot follows: then the view keeps his own heading. */
    const int aiming = ((game_fov * 57.2957795f > 1 && game_fov * 57.2957795f < 46.5f) || runtime_oot_vr_aiming()) && !runtime_oot_vr_two_hands_held();
    const int was_fp = atomic_exchange(&oot_vr_fp_on, fp);
    float game_level[12]; memcpy(game_level, m, sizeof m); vr_view_level(game_level);
    if (fp) {
        if (!was_fp || aiming) atomic_store(&oot_vr_yaw_bits, oot_vr_float_bits(oot_view_yaw(game_level)));
        oot_fp_input in = { { feet[0], feet[1], feet[2] }, oot_vr_bits_float(atomic_load(&oot_vr_yaw_bits)), control.fp_blend,
                            control.fp_up[1], control.fp_fwd, { control.off[0], control.off[1], control.off[2] } };
        oot_fp_base(game_level, &in, m);
    }
    pthread_mutex_lock(&oot_vr_mu);
    memcpy(oot_vr_base_view, m, sizeof m); oot_vr_base_valid = 1;
    pthread_mutex_unlock(&oot_vr_mu);
    memcpy(oot_vr_cam.game_right, m, 16);
    if (!fp) {
        for (int i = 0; i < 3; i++) m[4 * i + 3] -= vr_dot3(m + 4 * i, control.off);
        m[11] -= control.cam_back;
    }
    if (!atomic_load(&oot_vr_dbg_noturn)) vr_view_turn(m, control.h, m);   /* "flag vr_noturn 1" (diagnostic): the game's own view, the head not applied */
    /* Third person the left stick goes by the game's camera, not by where the head looks; first person by the head. */
    { const float turned = atan2f(-m[8], m[10]), game = atan2f(-game_level[8], game_level[10]);
      atomic_store(&oot_vr_move_turn_bits, oot_vr_float_bits(fp ? 0 : oot_wrap_pi(game - turned))); }
    if (fp && atomic_load(&oot_vr_fill_seen)) {   /* the game sorts with the camera moved back, so the hands below the eye stay */
        float culled[12]; oot_cull_view(m, control.fp_up[1] * control.fp_blend, culled);
        pthread_mutex_lock(&oot_vr_cull_mu); oot_cull_put(&oot_vr_cull, culled, m[11]); pthread_mutex_unlock(&oot_vr_cull_mu);
        atomic_store(&oot_vr_cull_live, 1);
        memcpy(view + 8, culled, sizeof culled);
    } else memcpy(view + 8, m, sizeof m);
    /* The ring the replay matches each frame's camera block against (totk_vr.m vr_note_camera): this frame's head pose. */
    const uint32_t calls = __atomic_load_n(&oot_vr_cam.calls, __ATOMIC_RELAXED) + 1;
    memcpy(oot_vr_cam.ring[calls & 15].m, m, sizeof m);
    oot_vr_cam.ring[calls & 15].seq = control.seq;
    memset(oot_vr_cam.ring[calls & 15].pos, 0, sizeof oot_vr_cam.ring[calls & 15].pos);   /* third person */
    __atomic_store_n(&oot_vr_cam.used, control.seq, __ATOMIC_RELEASE);
    __atomic_store_n(&oot_vr_cam.cam, (uint64_t)(uintptr_t)view, __ATOMIC_RELEASE);
    __atomic_store_n(&oot_vr_cam.calls, calls, __ATOMIC_RELEASE);
    oot_vr_hook_end(OOT_HOOK_TURNED, view);
    if (calls < 4)
        fprintf(stderr, "[vr] OOT gameplay view %p turned by the head (sequence %u), view t %.3f %.3f %.3f\n", (void *)view,
                control.seq, (double)m[3], (double)m[7], (double)m[11]);
}

/* totk_vr.m headlook_fov: the game renders the headset's view. a: the width / height ratio (0: back to the player's);
   k < 0: the vertical half-angle tangent itself (k > 0, a widening factor, is TotK's Vert+ and not offered here). */
int runtime_aspect_vr(float a, float k) {
    if (!oot_vr_fov_cell || !oot_aspect_cell) return 0;
    if (!(a > 0)) {
        atomic_store(oot_vr_fov_cell, 0);
        if (oot_vr_aspect_owned) { atomic_store(oot_aspect_cell, oot_vr_aspect_saved); oot_vr_aspect_owned = 0; }
        fprintf(stderr, "[vr] OOT field of view and aspect back to the game's\n");
        return 1;
    }
    if (!(k < 0) || !isfinite(a) || !isfinite(k)) return 0;
    const float fovy = 2 * atanf(-k);
    uint32_t bits;
    if (!oot_vr_aspect_owned) { oot_vr_aspect_saved = atomic_load(oot_aspect_cell); oot_vr_aspect_owned = 1; }
    memcpy(&bits, &a, 4); atomic_store(oot_aspect_cell, bits);
    memcpy(&bits, &fovy, 4); atomic_store(oot_vr_fov_cell, bits);
    fprintf(stderr, "[vr] OOT renders the headset's view: aspect %.4f, vertical %.1f degrees\n", (double)a, (double)(fovy * 57.2957795f));
    return 1;
}

/* The small hooks on the shared body (oot_vr_hooks.h). log_only: installed only with TOTK_VR_BLADELOG=1. */
typedef oot_guest_call *(*oot_hook_fn)(oot_hook_regs *);
typedef struct { uint64_t site; uint32_t word; int after, log_only; oot_hook_fn fn; const char *what; } oot_hook_site;
static const oot_hook_site OOT_VR_HOOKS[] = {
    { 0x2ea3ef0, 0xd503237fu, 0, 0, oot_link_calc, "a hit sensor's update: the blade's attack is set up, the item wheel's calls" },
    { 0x8d29c0, 0xb9400428u, 1, 1, oot_blade_setup_seen, "an attack set up on a hit sensor (log)" },
    { 0x8d56f0, 0xd503237fu, 0, 1, oot_blade_off_seen, "a hit sensor switched off (log)" },
    { 0x2dff2d0, 0xd503237fu, 0, 0, oot_shot_entry, "the shoot function's entry: is the shooter Link" },
    { 0x2e035f0, 0x394073a8u, 0, 0, oot_shot_launch, "a launch worked out: the shot along the two hands" },
    { 0x2e036d0, 0x52832c08u, 0, 0, oot_shot_launch, "a launch worked out (its other way)" },
};
enum { OOT_VR_HOOK_COUNT = sizeof OOT_VR_HOOKS / sizeof *OOT_VR_HOOKS };
static uintptr_t oot_vr_hook_stubs;   /* the first site's stub */
static oot_guest_call *oot_vr_hook_dispatch(oot_hook_regs *r, uint64_t stub_return) {
    const uint64_t k = (stub_return - oot_vr_hook_stubs) / (OOT_HOOK_STUB_WORDS * 4);
    return k < OOT_VR_HOOK_COUNT ? OOT_VR_HOOKS[k].fn(r) : NULL;
}
static void oot_vr_hooks_install(Module *m, uint64_t pad, uint64_t page) {
    const uintptr_t b = (uintptr_t)m->base;
    const uint64_t body = pad + OOT_VR_HOOK_BODY, stubs = pad + OOT_VR_HOOK_STUBS, end = stubs + OOT_VR_HOOK_COUNT * OOT_HOOK_STUB_WORDS * 4;
    const uint64_t limit = m->ro_off && m->ro_off < ((m->text_end + 0x3fff) & ~0x3fffULL) ? m->ro_off : (m->text_end + 0x3fff) & ~0x3fffULL;
    if (end > limit || page + OOT_VR_HOOK_FN_CELL + 8 > m->size) runtime_fail("OOT VR: no room left for the hooks");
    for (uint64_t k = body; k < end; k++) if (m->base[k]) runtime_fail("OOT VR: text padding already in use (hooks)");
    for (uint64_t k = page + OOT_VR_HOOK_FN_CELL; k < page + OOT_VR_HOOK_FN_CELL + 8; k++) if (m->base[k]) runtime_fail("OOT VR: data page already in use (hooks)");
    uint32_t words[OOT_HOOK_BODY_WORDS];
    const char *e = oot_hook_body(b + body, b + page, OOT_VR_HOOK_FN_CELL, words);
    if (e) runtime_fail(e);
    memcpy(m->base + body, words, sizeof words);
    void *host = runtime_hle_wrap_named((void *)oot_vr_hook_dispatch, "our OOT VR hooks");
    memcpy(m->base + page + OOT_VR_HOOK_FN_CELL, &host, 8);
    oot_vr_hook_stubs = b + stubs;
    const char *log = getenv("TOTK_VR_BLADELOG"); const int logging = log && *log == '1';
    int installed = 0;
    for (unsigned k = 0; k < OOT_VR_HOOK_COUNT; k++) {
        const oot_hook_site *h = &OOT_VR_HOOKS[k];
        uint32_t have, stub[OOT_HOOK_STUB_WORDS]; memcpy(&have, m->base + h->site, 4);
        if (have != h->word) { fprintf(stderr, "[vr] OOT hook at main+%#llx: %08x where %08x is expected\n", (unsigned long long)h->site, have, h->word); runtime_fail("OOT VR: a hook site differs from verified update 131072"); }
        if (h->log_only && !logging) continue;
        const uint64_t at = stubs + (uint64_t)k * OOT_HOOK_STUB_WORDS * 4;
        e = oot_hook_stub(have, h->after, b + at, b + h->site, b + body, stub);
        const uint32_t to = oot_vr_b(h->site, at);
        if (e) runtime_fail(e);
        if (!to) runtime_fail("OOT VR: a hook's stub is out of branch range");
        memcpy(m->base + at, stub, sizeof stub);
        memcpy(m->base + h->site, &to, 4);
        installed++;
    }
    fprintf(stderr, "[vr] OOT: %d small hooks on the shared body (%llu bytes of text padding left)\n", installed, (unsigned long long)(limit - end));
}

int runtime_oot_vr_hooks_on(void) { return oot_vr_hook_stubs != 0; }
static int oot_vr_wanted(void) {
    const char *e = getenv("TOTK_VR_CONTROL");
    return e && *e && !getenv("TOTK_EXPORT_IOS");
}

static void oot_vr_install(Module *m) {
    if (!oot_vr_wanted()) return;
    uint32_t have, fov_have, player_have, fill_have, place_have;
    memcpy(&have, m->base + OOT_VR_CAMERA_SITE, 4);
    memcpy(&fov_have, m->base + OOT_VR_FOV_SITE, 4);
    memcpy(&player_have, m->base + OOT_VR_PLAYER_SITE, 4);
    memcpy(&fill_have, m->base + OOT_VR_FILL_SITE, 4);
    memcpy(&place_have, m->base + OOT_VR_PLACE_SITE, 4);
    if (have != OOT_VR_CAMERA_WORD) runtime_fail("OOT VR: the gameplay view site differs from verified update 131072");
    if (fov_have != OOT_VR_FOV_WORD) runtime_fail("OOT VR: the field-of-view load differs from verified update 131072");
    if (player_have != OOT_VR_PLAYER_WORD) runtime_fail("OOT VR: the player update site differs from verified update 131072");
    if (fill_have != OOT_VR_FILL_WORD) runtime_fail("OOT VR: the view fill site differs from verified update 131072");
    if (place_have != OOT_VR_PLACE_WORD) runtime_fail("OOT VR: the actor place site differs from verified update 131072");
    const uint64_t page = (m->file_size + 0x3fff) & ~0x3fffULL, rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    const uint64_t pad = (m->text_end + 15) & ~15ULL, call_stub = pad + OOT_VR_CALL_STUB, fov_stub = pad + OOT_VR_FOV_STUB;
    const uint64_t player_stub = pad + OOT_VR_PLAYER_STUB, fill_stub = pad + OOT_VR_FILL_STUB, place_stub = pad + OOT_VR_PLACE_STUB;
    const uintptr_t b = (uintptr_t)m->base;
    uint32_t call[OOT_VR_CALL_WORDS], fov[OOT_VR_FOV_WORDS], player[OOT_VR_CALL_WORDS], fill[OOT_VR_CALL_WORDS], place[OOT_VR_CALL_WORDS];
    const char *e = oot_vr_call_veneer(have, b + call_stub, b + OOT_VR_CAMERA_SITE, b + page, OOT_VR_FN_CELL, call);
    if (!e) e = oot_vr_fov_veneer(fov_have, b + fov_stub, b + OOT_VR_FOV_SITE, b + page, OOT_VR_FOV_CELL, fov);
    if (!e) e = oot_vr_host_veneer(player_have, b + player_stub, b + OOT_VR_PLAYER_SITE, b + page, OOT_VR_PLAYER_FN_CELL, 19, 19, player);
    if (!e) e = oot_vr_host_veneer(fill_have, b + fill_stub, b + OOT_VR_FILL_SITE, b + page, OOT_VR_FILL_FN_CELL, 28, 28, fill);
    if (!e) e = oot_vr_host_veneer(place_have, b + place_stub, b + OOT_VR_PLACE_SITE, b + page, OOT_VR_PLACE_FN_CELL, 22, 21, place);
    if (e) runtime_fail(e);
    if (call_stub + sizeof call > fov_stub || fov_stub + sizeof fov > player_stub || player_stub + sizeof player > fill_stub ||
        fill_stub + sizeof fill > place_stub || place_stub + sizeof place > rx_end || page + OOT_VR_PLACE_FN_CELL + 8 > m->size)
        runtime_fail("OOT VR: no room left for the veneers");
    for (uint64_t k = call_stub; k < place_stub + sizeof place; k++) if (m->base[k]) runtime_fail("OOT VR: text padding already in use");
    for (uint64_t k = page + OOT_VR_FOV_CELL; k < page + OOT_VR_PLACE_FN_CELL + 8; k++) if (m->base[k]) runtime_fail("OOT VR: data page already in use");
    void *host = runtime_hle_wrap_named((void *)oot_vr_camera, "our OOT VR camera");
    void *host_player = runtime_hle_wrap_named((void *)oot_vr_player, "our OOT VR player update");
    void *host_fill = runtime_hle_wrap_named((void *)oot_vr_fill, "our OOT VR view fill");
    void *host_place = runtime_hle_wrap_named((void *)oot_vr_place, "our OOT VR weapon place");
    memcpy(m->base + page + OOT_VR_FN_CELL, &host, 8);
    memcpy(m->base + page + OOT_VR_PLAYER_FN_CELL, &host_player, 8);
    memcpy(m->base + page + OOT_VR_FILL_FN_CELL, &host_fill, 8);
    memcpy(m->base + page + OOT_VR_PLACE_FN_CELL, &host_place, 8);
    oot_vr_fov_cell = (_Atomic uint32_t *)(void *)(m->base + page + OOT_VR_FOV_CELL);
    memcpy(m->base + call_stub, call, sizeof call);
    memcpy(m->base + fov_stub, fov, sizeof fov);
    memcpy(m->base + player_stub, player, sizeof player);
    memcpy(m->base + fill_stub, fill, sizeof fill);
    memcpy(m->base + place_stub, place, sizeof place);
    const uint32_t to_call = oot_vr_b(OOT_VR_CAMERA_SITE, call_stub), to_fov = oot_vr_b(OOT_VR_FOV_SITE, fov_stub),
                   to_player = oot_vr_b(OOT_VR_PLAYER_SITE, player_stub), to_fill = oot_vr_b(OOT_VR_FILL_SITE, fill_stub),
                   to_place = oot_vr_b(OOT_VR_PLACE_SITE, place_stub);
    if (!to_call || !to_fov || !to_player || !to_fill || !to_place) runtime_fail("OOT VR: veneer out of branch range");
    memcpy(m->base + OOT_VR_CAMERA_SITE, &to_call, 4);
    memcpy(m->base + OOT_VR_FOV_SITE, &to_fov, 4);
    memcpy(m->base + OOT_VR_PLAYER_SITE, &to_player, 4);
    memcpy(m->base + OOT_VR_FILL_SITE, &to_fill, 4);
    {   /* The weapon's actor goes with the hand while the blade makes its own hits (oot_vr_sword.h); TOTK_VR_SWORD_FOLLOW=0
           leaves the game's code there as it is, and the hits with the button. */
        const char *follow = getenv("TOTK_VR_SWORD_FOLLOW");
        if (!(follow && *follow == '0')) { memcpy(m->base + OOT_VR_PLACE_SITE, &to_place, 4); oot_vr_hooks_install(m, pad, page); }
        else fprintf(stderr, "[vr] OOT: TOTK_VR_SWORD_FOLLOW=0: the weapon stays the game's, no hooks for the blade\n");
    }
    {   /* the projected-size scale (level of detail), see OOT_VR_LOD_SITES */
        const char *off = getenv("OOT_VR_LOD");
        const uint64_t lod_stub = pad + OOT_VR_LOD_STUB;
        if (!(off && *off == '0')) {
            if (lod_stub + OOT_VR_LOD_WORDS * 4 > rx_end || page + OOT_VR_LOD_CELL + 4 > m->size) runtime_fail("OOT VR: no room left for the level-of-detail veneers");
            for (uint64_t k = lod_stub; k < lod_stub + OOT_VR_LOD_WORDS * 4; k++) if (m->base[k]) runtime_fail("OOT VR: text padding already in use (level of detail)");
            for (uint64_t k = page + OOT_VR_LOD_CELL; k < page + OOT_VR_LOD_CELL + 4; k++) if (m->base[k]) runtime_fail("OOT VR: data page already in use (level of detail)");
            const float one = 1; memcpy(m->base + page + OOT_VR_LOD_CELL, &one, 4);
            for (int i = 0; i < 1; i++) {
                uint32_t at, next; memcpy(&at, m->base + OOT_VR_LOD_SITES[i], 4); memcpy(&next, m->base + OOT_VR_LOD_SITES[i] + 4, 4);
                if (at != OOT_VR_LOD_WORD || next != OOT_VR_LOD_NEXT) runtime_fail("OOT VR: the projected-size scale store differs from verified update 131072");
                const uint64_t stub = lod_stub + (uint64_t)i * OOT_VR_LOD_WORDS * 4;
                const uint32_t adrp = oot_vr_adrp(16, b + stub + 4, b + page), back = oot_vr_b(stub + 24, OOT_VR_LOD_SITES[i] + 4), to = oot_vr_b(OOT_VR_LOD_SITES[i], stub);
                if (!adrp || !back || !to) runtime_fail("OOT VR: level-of-detail veneer out of range");
                const uint32_t w[OOT_VR_LOD_WORDS] = {
                    0xfc1f0fffu,                                             /* str d31, [sp, #-16]! */
                    adrp,                                                    /* adrp x16, <data page> */
                    0xbd400000u | ((OOT_VR_LOD_CELL / 4) << 10) | (16u << 5) | 31u,   /* ldr s31, [x16, #factor] */
                    0x1e3f0842u,                                             /* fmul s2, s2, s31 */
                    OOT_VR_LOD_WORD,                                         /* str s2, [x19, #0x1e0] */
                    0xfc4107ffu,                                             /* ldr d31, [sp], #16 */
                    back };
                memcpy(m->base + stub, w, sizeof w);
                memcpy(m->base + OOT_VR_LOD_SITES[i], &to, 4);
            }
            oot_vr_lod_cell = (_Atomic uint32_t *)(void *)(m->base + page + OOT_VR_LOD_CELL);
            fprintf(stderr, "[vr] OOT: the render view's projected-size scale is taken at main+0x33af7c (level of detail as without VR)\n");
        }
    }
    oot_vr_base = m->base;
    g_vr_headlook = &oot_vr_cam;
    fprintf(stderr, "[vr] OOT: the head turns the gameplay view (main+0x%llx), the headset's field of view at main+0x%llx\n",
            (unsigned long long)OOT_VR_CAMERA_SITE, (unsigned long long)OOT_VR_FOV_SITE);
}
