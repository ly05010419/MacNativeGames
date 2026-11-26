/* The comfort ring's causes, the crawl's eye height, the kinds of scene (zeldaVR/runtime/oot_vr_comfort.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_comfort.h"

#define MS 1000000ull
static oot_comfort s; static uint64_t t; static float p[3];
static void reset(void) { memset(&s, 0, sizeof s); t = 9000 * MS; p[0] = p[1] = p[2] = 0; oot_comfort_step(&s, t, p, 0, 0); }
/* ms of readings every 22 ms: Link moving at (vx, vy) m/s, the stick's push, the view turned by `turn` each reading */
static int go(unsigned ms, float vx, float vy, float stick, float turn) {
    int causes = 0;
    for (unsigned k = 0; k < ms; k += 22) { t += 22 * MS; p[0] += vx * 0.022f; p[1] += vy * 0.022f; causes = oot_comfort_step(&s, t, p, stick, turn); }
    return causes;
}

int main(void) {
    reset(); assert(go(500, 0, 0, 0, 0) == 0);                                     /* standing */
    assert(go(300, 3, 0, 1, 0) == OOT_COMFORT_MOVE);                                /* the stick walks him */
    assert(go(200, 0, 0, 0, 0) & OOT_COMFORT_MOVE); assert(go(200, 0, 0, 0, 0) == 0);   /* held 300 ms, then over */
    reset(); go(300, 3, 0, 1, 0);                                                    /* stop and go against a wall: one stretch, no flicker */
    for (int k = 0; k < 5; k++) { assert(go(110, 0, 0, 1, 0) & OOT_COMFORT_MOVE); assert(go(66, 3, 0, 1, 0) & OOT_COMFORT_MOVE); }
    /* the same reading asked for again, its time worked out a few microseconds earlier (new2: the ring went on and off at every
       headset frame): still moving */
    reset(); go(300, 3, 0, 1, 0);
    assert(oot_comfort_step(&s, t - 5000, p, 1, 0) & OOT_COMFORT_MOVE); assert(oot_comfort_step(&s, t - 9000, p, 1, 0) & OOT_COMFORT_MOVE);
    reset(); go(100, 0, 0, 0, 0);
    assert(go(66, 0, 0, 0, 0.03f) == OOT_COMFORT_TURN);                             /* the view turned for him */
    reset(); assert(go(44, 0, -6, 0, 0) == 0); assert(go(44, 0, -6, 0, 0) & OOT_COMFORT_AIR);   /* a fall, after three readings */
    reset(); assert(!(go(300, 0, -2, 0, 0) & OOT_COMFORT_AIR));                     /* down a slope: no fall */
    reset(); assert(go(300, 2, 0, 0, 0) == 0); assert(go(200, 2, 0, 0, 0) == OOT_COMFORT_CARRIED);   /* moved with the stick at rest for 400 ms */
    reset(); assert(go(100, 8, 0, 1, 0) == (OOT_COMFORT_MOVE | OOT_COMFORT_CARRIED));   /* faster than he runs: a ride */
    reset(); go(100, 3, 0, 1, 0); p[0] += 500; t += 22 * MS; assert(!(oot_comfort_step(&s, t, p, 0, 0) & OOT_COMFORT_CARRIED));   /* a warp is no movement */
    reset(); t += 2000 * MS; p[0] += 3; assert(oot_comfort_step(&s, t, p, 0, 0) == 0);   /* nor a gap between readings */

    const int off[4] = { 0, 0, 0, 0 }, mixed[4] = { 1, 3, 0, 2 };
    assert(oot_comfort_level(OOT_COMFORT_MOVE | OOT_COMFORT_TURN, off) == 0);
    assert(oot_comfort_level(OOT_COMFORT_MOVE, mixed) == 1 && oot_comfort_level(OOT_COMFORT_MOVE | OOT_COMFORT_TURN, mixed) == 3);
    assert(oot_comfort_level(OOT_COMFORT_AIR, mixed) == 0 && oot_comfort_level(OOT_COMFORT_CARRIED, mixed) == 2);
    assert(oot_comfort_inner(0) == 0 && oot_comfort_inner(1) > oot_comfort_inner(2) && oot_comfort_inner(2) > oot_comfort_inner(3));
    oot_vignette v = { 0, 0 };
    oot_vignette_step(&v, 2, 0.05f); assert(v.amount > 0.3f && v.amount < 0.4f && v.inner == 0.70f);
    for (int i = 0; i < 10; i++) oot_vignette_step(&v, 2, 0.05f); assert(v.amount == 1);
    oot_vignette_step(&v, 3, 0.05f); assert(v.inner < 0.70f && v.inner > 0.45f);    /* the width eases to the new level */
    for (int i = 0; i < 10; i++) oot_vignette_step(&v, 0, 0.05f); assert(v.amount == 0 && v.inner == 0);

    /* a crawl: the eyes come down to the game's own height for it, held between 15 and 50 cm, and back up */
    oot_crawl c = { 0 };
    assert(oot_crawl_eye(0.29f) == 0.29f && oot_crawl_eye(0.05f) == 0.15f && oot_crawl_eye(0.91f) == 0.50f && oot_crawl_eye(0) == 0.30f);
    assert(oot_crawl_drop(&c, 0, 1.0f, 0.91f, 0.02f) == 0);
    float d = 0; for (int i = 0; i < 9; i++) d = oot_crawl_drop(&c, 1, 1.0f, 0.29f, 0.02f); assert(d > 0.2f && d < 0.5f);
    for (int i = 0; i < 20; i++) d = oot_crawl_drop(&c, 1, 1.0f, 0.29f, 0.02f); assert(fabsf(d - 0.71f) < 1e-5f);
    for (int i = 0; i < 30; i++) d = oot_crawl_drop(&c, 0, 1.0f, 0.91f, 0.02f); assert(d == 0);
    assert(oot_crawl_drop(&c, 1, 0.2f, 0.29f, 10.0f) == 0);                          /* eyes already lower than the crawl's: nothing */

    assert(oot_event_class(NULL) == OOT_EVENT_NONE && oot_event_class("") == OOT_EVENT_TALK);
    assert(oot_event_class("Dm_SY_Ocarina_Start") == OOT_EVENT_OCARINA);
    assert(oot_event_class("Dm_SY_GetInEvent") == OOT_EVENT_ITEM_GET && oot_event_class("Dm_SY_TreasureboxSmall") == OOT_EVENT_ITEM_GET && oot_event_class("Dm_SY_ShopGet") == OOT_EVENT_ITEM_GET);
    assert(oot_event_class("Dm_SY_BancChangeArrive") == OOT_EVENT_SYSTEM && oot_event_class("DmF_SY_Something") == OOT_EVENT_SYSTEM);
    assert(oot_event_class("Dm_Kokiri_0030") == OOT_EVENT_STORY && oot_event_class("DmF_Deku_BossIn") == OOT_EVENT_STORY && oot_event_class("Opening_Introduction") == OOT_EVENT_STORY);
    assert(oot_event_class("Npc_Kokiri_012_Child_C3_T") == OOT_EVENT_TALK && oot_event_class("BottomOfTheWell_GetGlassTruth") == OOT_EVENT_TALK);
    assert(oot_event_takes_view(OOT_EVENT_STORY) && oot_event_takes_view(OOT_EVENT_ITEM_GET) && !oot_event_takes_view(OOT_EVENT_TALK) && !oot_event_takes_view(OOT_EVENT_SYSTEM) && !oot_event_takes_view(OOT_EVENT_OCARINA));
    puts("comfort: moving, turning, falling, carried; the ring's level and fade; the crawl's eye height; the kinds of scene");
    return 0;
}
