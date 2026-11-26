/* Ocarina of Time in VR, first person: when the view moves in a way the player's own head did not, for the dark ring that
   narrows the view then (the comfort options), and the view's height in a crawl space. Without the runtime
   (tests/test_oot_vr_comfort.c runs it). The four causes and the idea of a ring per cause follow HookshotVR's description
   (vr/comfort/comfort_host.c, vr/camera/crawl_host.c, GPL-3: ideas only, no code).
   Metres, seconds, radians; times in nanoseconds where named so. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { OOT_COMFORT_MOVE = 1, OOT_COMFORT_TURN = 2, OOT_COMFORT_AIR = 4, OOT_COMFORT_CARRIED = 8, OOT_COMFORT_CAUSES = 4 };
typedef struct {
    int have, falling;
    uint64_t at, stick_at, still_since, seen[OOT_COMFORT_CAUSES];
    float pos[3];
} oot_comfort;

/* One reading of Link's place (now, when it was taken), the left stick's push 0..1 and how far the view was turned for
   the player since the last reading. Returns the causes that hold now (each for 300 ms after it was last seen). */
static inline uint64_t oot_comfort_since(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }   /* a time a little before the last one is "now" */
static inline int oot_comfort_step(oot_comfort *s, uint64_t now, const float pos[3], float stick, float turned) {
    /* each cause holds for 300 ms after it was last seen: at 150 ms the ring went on and off five times in a second while Link
       walked against a fountain's rim (run new1) */
    const uint64_t HOLD = 300000000ull, PUSHED = 150000000ull;
    if (stick > 0.2f) s->stick_at = now;
    if (!s->still_since || stick > 0.2f) s->still_since = now;
    if (fabsf(turned) > 0.003f) s->seen[1] = now;                              /* a snap, or smooth turning */
    if (pos) {
        const float dt = s->have ? (float)oot_comfort_since(now, s->at) / 1e9f : 0;
        if (s->have && dt > 0.012f && dt < 0.25f) {
            const float d[3] = { pos[0] - s->pos[0], pos[1] - s->pos[1], pos[2] - s->pos[2] };
            const float ground = sqrtf(d[0] * d[0] + d[2] * d[2]) / dt, vy = d[1] / dt;
            if (ground > 30 || fabsf(vy) > 30) s->falling = 0;               /* a warp or a load: no movement */
            else {
                if (ground > 0.3f && oot_comfort_since(now, s->stick_at) < PUSHED) s->seen[0] = now;
                s->falling = vy < -3.0f ? s->falling + 1 : 0;
                if (s->falling >= 3) s->seen[2] = now;
                if ((ground > 1.0f && oot_comfort_since(now, s->still_since) > 400000000ull) || ground > 6.0f) s->seen[3] = now;
            }
        } else if (s->have && dt >= 0.25f) s->falling = 0;
        if (!s->have || dt > 0.012f) { memcpy(s->pos, pos, sizeof s->pos); s->at = now; s->have = 1; }
    }
    int causes = 0;
    for (int i = 0; i < OOT_COMFORT_CAUSES; i++) if (s->seen[i] && oot_comfort_since(now, s->seen[i]) <= HOLD) causes |= 1 << i;
    return causes;
}

/* The ring: levels[i] is the player's setting for cause i (0 off, 1 low, 2 medium, 3 high). The strongest level among the
   causes that hold; where the dark begins, as the tangent of the angle from straight ahead (0: no ring). */
static inline int oot_comfort_level(int causes, const int levels[OOT_COMFORT_CAUSES]) {
    int level = 0;
    for (int i = 0; i < OOT_COMFORT_CAUSES; i++) if ((causes >> i & 1) && levels[i] > level) level = levels[i];
    return level > 3 ? 3 : level;
}
static inline float oot_comfort_inner(int level) { return level <= 0 ? 0 : level == 1 ? 0.95f : level == 2 ? 0.70f : 0.45f; }
/* The ring fades in and out over 0.15 s; its width follows the level smoothly. */
typedef struct { float amount, inner; } oot_vignette;
static inline void oot_vignette_step(oot_vignette *v, int level, float dt) {
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    v->amount += level > 0 ? dt / 0.15f : -dt / 0.15f;
    v->amount = v->amount < 0 ? 0 : v->amount > 1 ? 1 : v->amount;
    if (level > 0) { const float want = oot_comfort_inner(level); v->inner = v->inner > 0 ? v->inner + (want - v->inner) * (1 - expf(-dt / 0.1f)) : want; }
    else if (v->amount <= 0) v->inner = 0;
}

/* A crawl space: the game says Link crawls (his flag and his view height, games/oot/runtime/oot_vr.h); the eyes come down
   from standing to the crawl's height and back up over 0.35 s. How far down the eyes are now. */
typedef struct { float amount; } oot_crawl;
static inline float oot_crawl_eye(float game_height) { return !(game_height > 0) ? 0.30f : game_height < 0.15f ? 0.15f : game_height > 0.50f ? 0.50f : game_height; }
static inline float oot_crawl_drop(oot_crawl *c, int crawling, float standing_eye, float game_height, float dt) {
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    c->amount += crawling ? dt / 0.35f : -dt / 0.35f;
    c->amount = c->amount < 0 ? 0 : c->amount > 1 ? 1 : c->amount;
    const float down = standing_eye - oot_crawl_eye(game_height), a = c->amount;
    return (down > 0 ? down : 0) * a * a * (3 - 2 * a);
}

/* What kind of scene of the game is playing, from its name (the event manager's): a story scene has the game's own camera,
   in everything else the player keeps his own eyes. */
enum { OOT_EVENT_NONE, OOT_EVENT_OCARINA, OOT_EVENT_ITEM_GET, OOT_EVENT_SYSTEM, OOT_EVENT_STORY, OOT_EVENT_TALK };
static inline int oot_event_starts(const char *name, const char *prefix) { return !strncmp(name, prefix, strlen(prefix)); }
static inline int oot_event_class(const char *name) {
    if (!name) return OOT_EVENT_NONE;
    if (!*name) return OOT_EVENT_TALK;                                       /* playing, its name not readable */
    if (oot_event_starts(name, "Dm_SY_Ocarina")) return OOT_EVENT_OCARINA;
    if (oot_event_starts(name, "Dm_SY_Get") || oot_event_starts(name, "Dm_SY_Treasurebox") || !strcmp(name, "Dm_SY_ShopGet")) return OOT_EVENT_ITEM_GET;
    if (oot_event_starts(name, "Dm_SY_") || oot_event_starts(name, "DmF_SY_")) return OOT_EVENT_SYSTEM;
    if (oot_event_starts(name, "Dm_") || oot_event_starts(name, "DmF_") || oot_event_starts(name, "Opening_")) return OOT_EVENT_STORY;
    return OOT_EVENT_TALK;
}
/* Does the game's camera take the view for this kind: its story scenes, and an item it shows the player. */
static inline int oot_event_takes_view(int kind) { return kind == OOT_EVENT_STORY || kind == OOT_EVENT_ITEM_GET; }
