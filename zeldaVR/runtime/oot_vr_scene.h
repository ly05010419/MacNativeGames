/* Ocarina of Time in VR: what the player sees while a scene of the game plays (vr.md rows 75-79, 71), without the runtime
   (tests/test_oot_vr_scene.c runs it). The ideas follow HookshotVR's description of its event handling (camera-ui.md 4.1-4.8;
   vr/camera/events_host.c and view_host.c, GPL-3: ideas, thresholds and rules only, no code):
     - a style for each scene: the player's own eyes, the game's picture on a flat screen, or in 3D from the director's camera,
       a theatre seat or the spot Link stood on (oot_scene_style);
     - the lighting of a story scene, which the game sets for each shot, followed, held from the first shot, or left as the
       area's (oot_palette_step);
     - whether the game culls from the player's view while a 3D scene plays (oot_scene_cull_headset);
     - after a scene, the player's heading turned by as much as the game turned Link (oot_realign_*).
   Views are rows R|t, world to view, looking down -z; a level view of yaw a looks along (sin a, 0, -cos a) (oot_vr_view.h).
   Link's facing f is atan2(forward x, forward z), 0 toward +z: the yaw that looks where he faces is pi - f, so a turn of Link
   by d turns the player's heading by -d. Times in nanoseconds, lengths in metres, angles in radians. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "oot_vr_view.h"
#include "oot_vr_comfort.h"   /* OOT_EVENT_* and oot_event_class */

/* ---- the options (the VR page) ---- */
enum { OOT_CUT_FLAT = 0, OOT_CUT_SCREEN = 1, OOT_CUT_3D = 2 };                                       /* cutscene_view */
enum { OOT_VANTAGE_DIRECTOR = 0, OOT_VANTAGE_THEATRE = 1, OOT_VANTAGE_SPOT = 2, OOT_VANTAGE_FOLLOW = 3 }; /* cutscene_3d */
enum { OOT_LIGHT_AUTO = 0, OOT_LIGHT_EVERY = 1, OOT_LIGHT_HELD = 2, OOT_LIGHT_AREA = 3 };              /* cutscene_light */
enum { OOT_REALIGN_OFF = 0, OOT_REALIGN_QUICK = 1, OOT_REALIGN_SNAP = 2, OOT_REALIGN_FOLLOW = 3 };     /* realign */
typedef struct {
    int view, vantage, light, cull_game, realign, item_keep, ocarina_hands;
} oot_scene_opts;
static inline oot_scene_opts oot_scene_defaults(void) {   /* HookshotVR's defaults (camera-ui.md 4.4); ours are the scenes as now (oot_scene_today) */
    const oot_scene_opts o = { OOT_CUT_3D, OOT_VANTAGE_DIRECTOR, OOT_LIGHT_AUTO, 0, OOT_REALIGN_QUICK, 1, 1 };
    return o;
}

/* The VR page's defaults since the merge of 2026-11-27: the scenes as players have them now (zeldaVR/gfx/oot_vr_scene_draw.h). */
static inline oot_scene_opts oot_scene_today(void) {
    const oot_scene_opts o = { OOT_CUT_SCREEN, OOT_VANTAGE_DIRECTOR, OOT_LIGHT_EVERY, 1, OOT_REALIGN_OFF, 0, 0 };
    return o;
}

/* ---- the style ---- */
enum { OOT_STYLE_EYES, OOT_STYLE_FLAT, OOT_STYLE_DIRECTOR, OOT_STYLE_THEATRE, OOT_STYLE_SPOT };
static inline const char *oot_style_name(int s) {
    static const char *const n[] = { "the player's eyes", "the game's picture on a screen", "3D from the director's camera", "3D from a theatre seat", "3D from Link's spot" };
    return s >= 0 && s <= OOT_STYLE_SPOT ? n[s] : "?";
}
/* An interaction (a talk) whose lighting is a story scene's is one (the new game's opening plays the Deku Tree's under its own
   name): the palette's name starts Dm_ or DmF_ and is not a system one. */
static inline int oot_palette_is_story(const char *palette) {
    if (!palette) return 0;
    if (!strncmp(palette, "Dm_SY_", 6) || !strncmp(palette, "DmF_SY_", 7)) return 0;
    return !strncmp(palette, "Dm_", 3) || !strncmp(palette, "DmF_", 4);
}
/* A story cinematic by the older, narrower name rule: Dm_ or DmF_, an area, "_", then a digit (Dm_Kokiri_0030). An item get may
   play under any other event's name (a chest's own, an NPC's talk), never under these or the ocarina's. */
static inline int oot_name_is_cinematic(const char *name) {
    if (!name) return 0;
    const char *p = !strncmp(name, "DmF_", 4) ? name + 4 : !strncmp(name, "Dm_", 3) ? name + 3 : NULL;
    if (!p || !strncmp(p, "SY_", 3)) return 0;
    const char *u = strchr(p, '_');
    return u && u > p && u[1] >= '0' && u[1] <= '9';
}
static inline int oot_item_get_may_play(int kind, const char *name) {
    if (kind == OOT_EVENT_NONE || kind == OOT_EVENT_OCARINA || kind == OOT_EVENT_STORY) return 0;
    return !oot_name_is_cinematic(name);
}

/* How a scene is shown. first_person: the player is at Link's eyes when no scene plays; far: the game's camera is far from Link's
   eyes (oot_scene_far); have_spot: there is a spot of Link's to stand on. Third person keeps the VR view on the game's camera. */
static inline int oot_scene_style(int kind, const oot_scene_opts *o, int first_person, int far, int have_spot) {
    if (kind == OOT_EVENT_NONE || (kind == OOT_EVENT_OCARINA && o->ocarina_hands)) return OOT_STYLE_EYES;
    if (o->view == OOT_CUT_FLAT) return OOT_STYLE_FLAT;
    int vantage = OOT_STYLE_EYES;
    switch (o->vantage) {
        case OOT_VANTAGE_DIRECTOR: vantage = OOT_STYLE_DIRECTOR; break;
        case OOT_VANTAGE_THEATRE: vantage = OOT_STYLE_THEATRE; break;
        case OOT_VANTAGE_SPOT: vantage = have_spot ? OOT_STYLE_SPOT : OOT_STYLE_DIRECTOR; break;
        default: vantage = OOT_STYLE_EYES;
    }
    if (kind == OOT_EVENT_STORY) {
        if (o->view == OOT_CUT_SCREEN) return OOT_STYLE_FLAT;
        return first_person ? vantage : OOT_STYLE_EYES;
    }
    /* "screen" is the scenes as before 2026-11-27 (merge of that day): the game's camera took story scenes and item gets (the
       item get only when it is not kept before the eyes), the player kept his eyes through the rest, a far talk too */
    if (o->view == OOT_CUT_SCREEN) return kind == OOT_EVENT_ITEM_GET && !o->item_keep ? OOT_STYLE_FLAT : OOT_STYLE_EYES;
    if (kind == OOT_EVENT_TALK && first_person && far) return o->vantage >= OOT_VANTAGE_SPOT ? OOT_STYLE_EYES : vantage;
    return OOT_STYLE_EYES;   /* talks, signs, doors, arrivals, item gets: never a flat picture for a dialogue */
}
static inline int oot_style_is_3d(int s) { return s == OOT_STYLE_DIRECTOR || s == OOT_STYLE_THEATRE || s == OOT_STYLE_SPOT; }

/* The game's camera far from Link's eyes: over 8 m, and near again only under 6 m. */
static inline int oot_scene_far(int *far, float distance) {
    if (!isfinite(distance)) return *far;
    if (*far) { if (distance < 6.0f) *far = 0; }
    else if (distance > 8.0f) *far = 1;
    return *far;
}

/* ---- the 3D vantages: an eye and a level heading, the head's own turn and movement on top ---- */
/* The level heading that looks from the camera toward its target, 0 when it looks straight up or down. */
static inline int oot_scene_heading(const float pos[3], const float target[3], float *yaw) {
    const float bx = pos[0] - target[0], bz = pos[2] - target[2];
    if (!isfinite(bx) || !isfinite(bz) || bx * bx + bz * bz < 1e-8f) return 0;
    *yaw = atan2f(-bx, bz);   /* forward = -back = (sin a, -cos a) */
    return 1;
}
/* A theatre seat: the first shot's camera, kept while the director's camera stays within 35 m of it; a new place for the scene
   moves the seat there, looking as that shot looks. */
typedef struct { int have; float eye[3], yaw; } oot_seat;
static inline void oot_seat_step(oot_seat *s, const float pos[3], const float target[3]) {
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return;
    const float d[3] = { pos[0] - s->eye[0], pos[1] - s->eye[1], pos[2] - s->eye[2] };
    if (s->have && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= 35.0f * 35.0f) return;
    float yaw;
    if (!oot_scene_heading(pos, target, &yaw)) yaw = s->have ? s->yaw : (float)M_PI;   /* +z ahead when it never said */
    memcpy(s->eye, pos, sizeof s->eye); s->yaw = yaw; s->have = 1;
}
/* The view before the head's rotation: level rows of yaw, the eye plus the head's own movement (world axes). */
static inline void oot_scene_view(const float eye[3], float yaw, const float off[3], float out[12]) {
    const float p[3] = { eye[0] + off[0], eye[1] + off[1], eye[2] + off[2] };
    oot_level_rows(yaw, out);
    vr_view_translation(out, p);
}

/* ---- the game culls from the player's view (option cull_game 0) while a 3D scene plays, or a story scene from Link's eyes ---- */
static inline int oot_scene_cull_headset(int style, int kind, int first_person, int cull_game) {
    if (!first_person || cull_game) return 0;
    return oot_style_is_3d(style) || (style == OOT_STYLE_EYES && kind == OOT_EVENT_STORY);
}

/* ---- the lighting of a story scene (the WorldMgr's demo palette) ---- */
/* The way this scene is lit: a flat picture and the director's camera show each shot as the game lit it; elsewhere Auto holds the
   first shot's light (another shot's light, fitted to the director's camera, looks like a box of other light from a seat). */
static inline int oot_light_mode(int style, int option, int third_person) {
    if (style == OOT_STYLE_FLAT) return OOT_LIGHT_EVERY;
    if (option == OOT_LIGHT_AUTO) return style == OOT_STYLE_DIRECTOR || third_person ? OOT_LIGHT_EVERY : OOT_LIGHT_HELD;
    return option >= OOT_LIGHT_EVERY && option <= OOT_LIGHT_AREA ? option : OOT_LIGHT_EVERY;
}
enum { OOT_PALETTE_NAME = 64, OOT_PAL_NONE = 0, OOT_PAL_SET = 1, OOT_PAL_RESET = 2 };
typedef struct {
    int active;                       /* the palette shown is ours */
    int have_held;
    char ours[OOT_PALETTE_NAME];      /* what we set ("" after our reset) */
    char game[OOT_PALETTE_NAME];      /* the game's own last one ("" none) */
    char held[OOT_PALETTE_NAME];      /* the first shot's */
} oot_palette;
static inline void oot_palette_copy(char *to, const char *from) { size_t n = 0; while (from && from[n] && n + 1 < OOT_PALETTE_NAME) { to[n] = from[n]; n++; } to[n] = 0; }
/* One look at the palette the WorldMgr shows (current, "" none). Returns what to do: OOT_PAL_SET with *name, OOT_PAL_RESET, or
   nothing. When the scene ends with our palette still showing, the area's light comes back (the game's own end of the scene resets
   it too; we cannot see whether it did before we look, so the area's light, never a shot's, is what we leave). */
static inline int oot_palette_step(oot_palette *p, int mode, int playing, const char *current, const char **name) {
    if (!current) current = "";
    *name = NULL;
    if (!p->active || strcmp(current, p->ours)) { if (strcmp(current, p->game)) oot_palette_copy(p->game, current); }
    if (!playing) {
        const int ours = p->active && !strcmp(current, p->ours) && current[0];
        memset(p, 0, sizeof *p);
        return ours ? OOT_PAL_RESET : OOT_PAL_NONE;
    }
    if (mode == OOT_LIGHT_EVERY) {
        if (p->active && !strcmp(current, p->ours)) {
            char game[OOT_PALETTE_NAME]; oot_palette_copy(game, p->game);
            p->active = 0; p->ours[0] = 0; p->have_held = 0; p->held[0] = 0;
            if (game[0] && strcmp(game, current)) { oot_palette_copy(p->held, game); *name = p->held; return OOT_PAL_SET; }
            return current[0] ? OOT_PAL_RESET : OOT_PAL_NONE;
        }
        p->active = 0; p->have_held = 0;
        return OOT_PAL_NONE;
    }
    if (mode == OOT_LIGHT_HELD) {
        if (!p->have_held) { if (current[0]) { oot_palette_copy(p->held, current); p->have_held = 1; } return OOT_PAL_NONE; }
        if (current[0] && strcmp(current, p->held)) { oot_palette_copy(p->ours, p->held); p->active = 1; *name = p->held; return OOT_PAL_SET; }
        return OOT_PAL_NONE;
    }
    if (mode == OOT_LIGHT_AREA && current[0]) { p->ours[0] = 0; p->active = 1; return OOT_PAL_RESET; }
    return OOT_PAL_NONE;
}

/* ---- re-aligning the heading after a scene (first person) ---- */
static const float OOT_REALIGN_MIN = 0.0872665f;   /* 5 degrees: smaller turns at the end are dropped */
static const float OOT_REALIGN_RATE = 5.0f;         /* radians a second while easing */
typedef struct {
    int have, busy_was, due;
    float facing, pos[3], turn, easing;
    uint64_t link_at;
} oot_realign;
/* Link's own update (upright, his facing and place). event: a scene plays; stick: the left stick moved him within 100 ms;
   item_get: turns now are the game showing an item (with "keep your view" they are dropped, and those before in this stretch). */
static inline void oot_realign_note(oot_realign *s, uint64_t now, float facing, const float pos[3], int event, int stick, int item_get) {
    if (!isfinite(facing) || !isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return;
    const uint64_t since = s->have ? now - s->link_at : 0;
    const int gap = s->have && since > 500000000ull, stale = s->have && since > 150000000ull;
    if (s->have && (event || s->busy_was || stale)) {
        const float d[3] = { pos[0] - s->pos[0], pos[1] - s->pos[1], pos[2] - s->pos[2] };
        const float moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (item_get) s->turn = 0;
        else if (!stick || gap) s->turn = oot_wrap_pi(s->turn + oot_wrap_pi(facing - s->facing));
        if (fabsf(s->turn) >= OOT_REALIGN_MIN) {
            if (gap) { if (s->due < 3) s->due = 3; }            /* back after a load */
            else if (event && moved > 1.0f && s->due < 2) s->due = 2;   /* a warp */
        }
    }
    if ((s->busy_was || stale) && !event && s->due < 1) s->due = 1;   /* the stretch ends */
    s->busy_was = event; s->facing = facing; memcpy(s->pos, pos, sizeof s->pos); s->link_at = now; s->have = 1;
}
/* The camera, each update: how much to add to Link's facing's change, in Link's facing (the caller turns the heading by minus it).
   shown: the view is the player's own (not the flat picture); came_back: it is again after a flat stretch; locked: lock-on owns
   the heading; dt: seconds since the last update. */
static inline float oot_realign_take(oot_realign *s, uint64_t now, float dt, int option, int locked, int shown, int came_back) {
    if (!(dt > 0) || dt > 0.05f) dt = dt > 0.05f ? 0.05f : 0;
    if (option == OOT_REALIGN_OFF || locked) { s->turn = s->easing = 0; s->due = 0; return 0; }
    const int busy = s->busy_was || (s->have && now - s->link_at > 150000000ull);
    float out = 0;
    if (option == OOT_REALIGN_FOLLOW && busy && shown) { s->easing += s->turn; s->turn = 0; }
    if (s->due || came_back) {
        if (fabsf(s->turn) >= OOT_REALIGN_MIN) {
            if (s->due >= 2 || !shown || option == OOT_REALIGN_SNAP) out += s->turn;
            else s->easing += s->turn;
        }
        s->turn = 0; s->due = 0;
    }
    if (shown && s->easing != 0) {
        const float most = OOT_REALIGN_RATE * dt, step = s->easing > most ? most : s->easing < -most ? -most : s->easing;
        out += step; s->easing -= step;
        if (fabsf(s->easing) < 1e-5f) s->easing = 0;
    }
    return out;
}
