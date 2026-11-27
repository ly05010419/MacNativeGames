/* What the player sees while a scene plays (zeldaVR/runtime/oot_vr_scene.h): the style, the vantages, the lighting, the culling
   and the heading after a scene. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_scene.h"

static int near(float a, float b, float e) { return fabsf(a - b) <= e; }
static const uint64_t MS = 1000000ull;

int main(void) {
    oot_scene_opts o = oot_scene_defaults();
    assert(o.view == OOT_CUT_3D && o.vantage == OOT_VANTAGE_DIRECTOR && o.light == OOT_LIGHT_AUTO && !o.cull_game && o.realign == OOT_REALIGN_QUICK && o.item_keep && o.ocarina_hands);
    /* the style: nothing playing, or the ocarina held in the hands, keeps the eyes */
    assert(oot_scene_style(OOT_EVENT_NONE, &o, 1, 0, 1) == OOT_STYLE_EYES);
    assert(oot_scene_style(OOT_EVENT_OCARINA, &o, 1, 0, 1) == OOT_STYLE_EYES);
    /* a story scene in 3D: the vantage chosen, in first person only */
    assert(oot_scene_style(OOT_EVENT_STORY, &o, 1, 0, 1) == OOT_STYLE_DIRECTOR);
    assert(oot_scene_style(OOT_EVENT_STORY, &o, 0, 1, 1) == OOT_STYLE_EYES);
    o.vantage = OOT_VANTAGE_THEATRE; assert(oot_scene_style(OOT_EVENT_STORY, &o, 1, 0, 1) == OOT_STYLE_THEATRE);
    o.vantage = OOT_VANTAGE_SPOT; assert(oot_scene_style(OOT_EVENT_STORY, &o, 1, 0, 1) == OOT_STYLE_SPOT);
    assert(oot_scene_style(OOT_EVENT_STORY, &o, 1, 0, 0) == OOT_STYLE_DIRECTOR);   /* no spot of Link's yet */
    o.vantage = OOT_VANTAGE_FOLLOW; assert(oot_scene_style(OOT_EVENT_STORY, &o, 1, 0, 1) == OOT_STYLE_EYES);
    /* a talk keeps the eyes, unless the game's camera is far: then the director or the seat (Link's spot and follow keep the eyes) */
    o.vantage = OOT_VANTAGE_DIRECTOR;
    assert(oot_scene_style(OOT_EVENT_TALK, &o, 1, 0, 1) == OOT_STYLE_EYES && oot_scene_style(OOT_EVENT_TALK, &o, 1, 1, 1) == OOT_STYLE_DIRECTOR);
    assert(oot_scene_style(OOT_EVENT_TALK, &o, 0, 1, 1) == OOT_STYLE_EYES);
    o.vantage = OOT_VANTAGE_SPOT; assert(oot_scene_style(OOT_EVENT_TALK, &o, 1, 1, 1) == OOT_STYLE_EYES);
    o.vantage = OOT_VANTAGE_DIRECTOR;
    /* item gets, doors, arrivals: the eyes */
    assert(oot_scene_style(OOT_EVENT_ITEM_GET, &o, 1, 1, 1) == OOT_STYLE_EYES && oot_scene_style(OOT_EVENT_SYSTEM, &o, 1, 1, 1) == OOT_STYLE_EYES);
    /* "screen": story scenes flat, the rest the eyes (a far talk too: the scenes as before 2026-11-27); an item get flat unless it
       is kept before the eyes; "flat": every scene flat, the ocarina in the hands still the eyes */
    o.view = OOT_CUT_SCREEN;
    assert(oot_scene_style(OOT_EVENT_STORY, &o, 1, 0, 1) == OOT_STYLE_FLAT && oot_scene_style(OOT_EVENT_STORY, &o, 0, 0, 1) == OOT_STYLE_FLAT);
    assert(oot_scene_style(OOT_EVENT_TALK, &o, 1, 1, 1) == OOT_STYLE_EYES && oot_scene_style(OOT_EVENT_ITEM_GET, &o, 1, 0, 1) == OOT_STYLE_EYES);
    o.item_keep = 0; assert(oot_scene_style(OOT_EVENT_ITEM_GET, &o, 1, 0, 1) == OOT_STYLE_FLAT && oot_scene_style(OOT_EVENT_SYSTEM, &o, 1, 0, 1) == OOT_STYLE_EYES);
    o.item_keep = 1;
    o.view = OOT_CUT_FLAT;
    assert(oot_scene_style(OOT_EVENT_TALK, &o, 1, 0, 1) == OOT_STYLE_FLAT && oot_scene_style(OOT_EVENT_ITEM_GET, &o, 0, 0, 1) == OOT_STYLE_FLAT);
    assert(oot_scene_style(OOT_EVENT_OCARINA, &o, 1, 0, 1) == OOT_STYLE_EYES);
    o.ocarina_hands = 0; assert(oot_scene_style(OOT_EVENT_OCARINA, &o, 1, 0, 1) == OOT_STYLE_FLAT);
    /* our defaults (merge 2026-11-27): the scenes as before; story scenes and item gets on the screen, the ocarina the buttons */
    o = oot_scene_today();
    assert(o.view == OOT_CUT_SCREEN && o.light == OOT_LIGHT_EVERY && o.cull_game && o.realign == OOT_REALIGN_OFF && !o.item_keep && !o.ocarina_hands);
    assert(oot_scene_style(OOT_EVENT_STORY, &o, 1, 0, 1) == OOT_STYLE_FLAT && oot_scene_style(OOT_EVENT_ITEM_GET, &o, 1, 0, 1) == OOT_STYLE_FLAT);
    assert(oot_scene_style(OOT_EVENT_TALK, &o, 1, 1, 1) == OOT_STYLE_EYES && oot_scene_style(OOT_EVENT_SYSTEM, &o, 1, 1, 1) == OOT_STYLE_EYES && oot_scene_style(OOT_EVENT_OCARINA, &o, 1, 0, 1) == OOT_STYLE_EYES);
    assert(!oot_scene_cull_headset(OOT_STYLE_EYES, OOT_EVENT_STORY, 1, o.cull_game) && oot_light_mode(OOT_STYLE_EYES, o.light, 0) == OOT_LIGHT_EVERY);
    o = oot_scene_defaults();
    assert(oot_style_is_3d(OOT_STYLE_DIRECTOR) && oot_style_is_3d(OOT_STYLE_SPOT) && !oot_style_is_3d(OOT_STYLE_EYES) && !oot_style_is_3d(OOT_STYLE_FLAT));

    /* names: the story palette, the cinematic's narrow rule, where an item get may play */
    assert(oot_palette_is_story("Dm_Kokiri_0030_C05") && oot_palette_is_story("DmF_Deku_BossIn_C01") && !oot_palette_is_story("Dm_SY_Get") && !oot_palette_is_story("HyruleField") && !oot_palette_is_story(""));
    assert(oot_name_is_cinematic("Dm_Kokiri_0030") && oot_name_is_cinematic("DmF_Deku_1") && !oot_name_is_cinematic("Dm_SY_Get") && !oot_name_is_cinematic("Dm_MelodyGet_Saria") && !oot_name_is_cinematic("Npc_Kokiri_012"));
    assert(oot_item_get_may_play(OOT_EVENT_TALK, "KokiriForest_TreasureBox_NaviChatOff") && oot_item_get_may_play(OOT_EVENT_ITEM_GET, "Dm_SY_Get"));
    assert(!oot_item_get_may_play(OOT_EVENT_STORY, "Dm_Kokiri_0030") && !oot_item_get_may_play(OOT_EVENT_OCARINA, "Dm_SY_Ocarina") && !oot_item_get_may_play(OOT_EVENT_NONE, ""));
    assert(!oot_item_get_may_play(OOT_EVENT_TALK, "Dm_Kokiri_0030"));

    /* far and near again */
    int far = 0;
    assert(!oot_scene_far(&far, 7.9f) && oot_scene_far(&far, 8.1f) && oot_scene_far(&far, 6.5f) && !oot_scene_far(&far, 5.9f) && !oot_scene_far(&far, NAN));

    /* the director's heading looks from the camera to its target, level */
    float yaw = 0;
    { const float pos[3] = { 0, 5, 10 }, target[3] = { 0, 0, 0 }; assert(oot_scene_heading(pos, target, &yaw) && near(yaw, 0, 1e-5f)); }   /* looks toward -z */
    { const float pos[3] = { -3, 0, 0 }, target[3] = { 0, 2, 0 }; assert(oot_scene_heading(pos, target, &yaw) && near(yaw, (float)M_PI / 2, 1e-5f)); }   /* toward +x */
    { const float pos[3] = { 1, 5, 1 }, target[3] = { 1, 0, 1 }; assert(!oot_scene_heading(pos, target, &yaw)); }   /* straight down */
    {   /* the vantage's view: its rows level and its eye where asked */
        float v[12], eye[3]; const float at[3] = { 2, 3, 4 }, off[3] = { 0.1f, 0, 0 };
        oot_scene_view(at, (float)M_PI / 2, off, v); vr_view_eye(v, eye);
        assert(near(eye[0], 2.1f, 1e-5f) && near(eye[1], 3, 1e-5f) && near(eye[2], 4, 1e-5f));
        assert(near(v[5], 1, 1e-6f) && near(v[1], 0, 1e-6f) && near(v[9], 0, 1e-6f));   /* up is the world's */
        assert(near(-v[8], 1, 1e-5f) && near(-v[10], 0, 1e-5f));                         /* looking along +x */
    }
    {   /* the theatre seat: kept within 35 m, a new one beyond, looking as that shot looks */
        oot_seat s = { 0 }; const float t0[3] = { 0, 0, 0 };
        const float a[3] = { 0, 2, 10 }; oot_seat_step(&s, a, t0); assert(s.have && near(s.eye[2], 10, 1e-6f) && near(s.yaw, 0, 1e-5f));
        const float b[3] = { 20, 2, 10 }; oot_seat_step(&s, b, t0); assert(near(s.eye[0], 0, 1e-6f));      /* 20 m: the same seat */
        const float c[3] = { 50, 2, 10 }; oot_seat_step(&s, c, c); assert(near(s.eye[0], 50, 1e-6f) && near(s.yaw, 0, 1e-5f));   /* moved, heading kept */
        const float d[3] = { NAN, 0, 0 }; oot_seat_step(&s, d, t0); assert(near(s.eye[0], 50, 1e-6f));
    }
    /* culling from the player's view: 3D scenes and story scenes at the eyes, first person, unless the option says the game's */
    assert(oot_scene_cull_headset(OOT_STYLE_DIRECTOR, OOT_EVENT_STORY, 1, 0) && oot_scene_cull_headset(OOT_STYLE_EYES, OOT_EVENT_STORY, 1, 0));
    assert(!oot_scene_cull_headset(OOT_STYLE_EYES, OOT_EVENT_TALK, 1, 0) && !oot_scene_cull_headset(OOT_STYLE_DIRECTOR, OOT_EVENT_STORY, 0, 0) && !oot_scene_cull_headset(OOT_STYLE_THEATRE, OOT_EVENT_STORY, 1, 1));
    assert(!oot_scene_cull_headset(OOT_STYLE_FLAT, OOT_EVENT_STORY, 1, 0));

    /* the lighting's mode */
    assert(oot_light_mode(OOT_STYLE_FLAT, OOT_LIGHT_HELD, 0) == OOT_LIGHT_EVERY && oot_light_mode(OOT_STYLE_DIRECTOR, OOT_LIGHT_AUTO, 0) == OOT_LIGHT_EVERY);
    assert(oot_light_mode(OOT_STYLE_THEATRE, OOT_LIGHT_AUTO, 0) == OOT_LIGHT_HELD && oot_light_mode(OOT_STYLE_THEATRE, OOT_LIGHT_AUTO, 1) == OOT_LIGHT_EVERY);
    assert(oot_light_mode(OOT_STYLE_SPOT, OOT_LIGHT_AREA, 0) == OOT_LIGHT_AREA && oot_light_mode(OOT_STYLE_SPOT, 9, 0) == OOT_LIGHT_EVERY);
    {   /* held: the first shot's light set again when the game sets another; the area's light when the scene ends with ours */
        oot_palette p; memset(&p, 0, sizeof p); const char *name = NULL;
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "", &name) == OOT_PAL_NONE);
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "Dm_Kokiri_0030_C05", &name) == OOT_PAL_NONE);
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "Dm_Kokiri_0030_C05", &name) == OOT_PAL_NONE);
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "Dm_Kokiri_0030_C26", &name) == OOT_PAL_SET && !strcmp(name, "Dm_Kokiri_0030_C05") && !strcmp(p.game, "Dm_Kokiri_0030_C26"));
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "Dm_Kokiri_0030_C05", &name) == OOT_PAL_NONE);   /* ours showing: nothing */
        /* the player turns to the director's camera: every shot again, the game's own last one put back */
        assert(oot_palette_step(&p, OOT_LIGHT_EVERY, 1, "Dm_Kokiri_0030_C05", &name) == OOT_PAL_SET && !strcmp(name, "Dm_Kokiri_0030_C26") && !p.active);
        assert(oot_palette_step(&p, OOT_LIGHT_EVERY, 1, "Dm_Kokiri_0030_C26", &name) == OOT_PAL_NONE);
        /* held again, then the scene ends with ours showing: the area's light */
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "Dm_Kokiri_0030_C26", &name) == OOT_PAL_NONE);
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "Dm_Kokiri_0030_C27", &name) == OOT_PAL_SET && !strcmp(name, "Dm_Kokiri_0030_C26"));
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 0, "Dm_Kokiri_0030_C26", &name) == OOT_PAL_RESET && !p.active && !p.have_held);
        /* ended after the game reset it itself: nothing */
        memset(&p, 0, sizeof p);
        (void)oot_palette_step(&p, OOT_LIGHT_HELD, 1, "A_C1", &name); assert(oot_palette_step(&p, OOT_LIGHT_HELD, 1, "A_C2", &name) == OOT_PAL_SET);
        assert(oot_palette_step(&p, OOT_LIGHT_HELD, 0, "", &name) == OOT_PAL_NONE);
        /* the area's: every palette the game sets is reset; at the end nothing is left to undo */
        memset(&p, 0, sizeof p);
        assert(oot_palette_step(&p, OOT_LIGHT_AREA, 1, "A_C1", &name) == OOT_PAL_RESET && p.active && !strcmp(p.game, "A_C1"));
        assert(oot_palette_step(&p, OOT_LIGHT_AREA, 1, "", &name) == OOT_PAL_NONE);
        assert(oot_palette_step(&p, OOT_LIGHT_AREA, 1, "A_C2", &name) == OOT_PAL_RESET && !strcmp(p.game, "A_C2"));
        assert(oot_palette_step(&p, OOT_LIGHT_AREA, 0, "", &name) == OOT_PAL_NONE);
        /* every shot with nothing of ours: nothing ever */
        memset(&p, 0, sizeof p);
        assert(oot_palette_step(&p, OOT_LIGHT_EVERY, 1, "A_C1", &name) == OOT_PAL_NONE && oot_palette_step(&p, OOT_LIGHT_EVERY, 0, "A_C1", &name) == OOT_PAL_NONE);
    }

    {   /* re-align: Link turned 60 degrees by a talk, no stick: a quick turn after it at 5 rad/s */
        oot_realign r; memset(&r, 0, sizeof r); const float pos[3] = { 0, 0, 0 };
        uint64_t t = 1000 * MS;
        oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        for (int i = 1; i <= 6; i++) { t += 16 * MS; oot_realign_note(&r, t, 0.1745f * (float)i, pos, 1, 0, 0); assert(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_QUICK, 0, 1, 0) == 0); }
        assert(near(r.turn, 1.047f, 1e-3f) && !r.due);
        t += 16 * MS; oot_realign_note(&r, t, 1.047f, pos, 0, 0, 0); assert(r.due == 1);   /* the scene ended */
        float total = 0; int steps = 0;
        for (; steps < 40; steps++) { t += 16 * MS; const float d = oot_realign_take(&r, t, 0.016f, OOT_REALIGN_QUICK, 0, 1, 0); assert(d <= 5.0f * 0.016f + 1e-6f); total += d; if (!r.easing && d == 0 && steps) break; }
        assert(near(total, 1.047f, 1e-3f) && steps >= 12 && steps <= 16);
        /* snap: at once */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, -0.5f, pos, 1, 0, 0); t += 16 * MS; oot_realign_note(&r, t, -0.5f, pos, 0, 0, 0);
        assert(near(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_SNAP, 0, 1, 0), -0.5f, 1e-5f));
        /* a turn the player made with the left stick does not count, nor one under 5 degrees, nor one while locked on */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, 0.8f, pos, 1, 1, 0); t += 16 * MS; oot_realign_note(&r, t, 0.85f, pos, 0, 0, 0);
        assert(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_SNAP, 0, 1, 0) == 0);
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, 1.0f, pos, 1, 0, 0); t += 16 * MS; oot_realign_note(&r, t, 1.0f, pos, 0, 0, 0);
        assert(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_SNAP, 1, 1, 0) == 0 && r.turn == 0 && !r.due);
        /* an item get with "keep your view": its turns, and those before it in the stretch, are dropped */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, 0.5f, pos, 1, 0, 0); t += 16 * MS; oot_realign_note(&r, t, 2.5f, pos, 1, 0, 1);
        t += 16 * MS; oot_realign_note(&r, t, 2.5f, pos, 0, 0, 0);
        assert(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_SNAP, 0, 1, 0) == 0);
        /* a load (Link not seen for over 0.5 s): all of the turn, at once, even with the stick */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 3.0f, pos, 0, 0, 0);
        t += 900 * MS; oot_realign_note(&r, t, -3.0f, pos, 0, 1, 0);   /* wraps: +0.283 */
        assert(r.due == 3 && near(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_QUICK, 0, 1, 0), 0.2832f, 1e-3f));
        /* a warp in a scene: at once */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        { const float far_pos[3] = { 3, 0, 0 }; t += 16 * MS; oot_realign_note(&r, t, 1.2f, far_pos, 1, 0, 0); }
        assert(r.due == 2 && near(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_QUICK, 0, 1, 0), 1.2f, 1e-5f));
        /* follow: eased while the scene plays */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, 0.04f, pos, 1, 0, 0);
        { const float d = oot_realign_take(&r, t, 0.016f, OOT_REALIGN_FOLLOW, 0, 1, 0); assert(near(d, 0.04f, 1e-5f)); }
        /* off: nothing, and the turn forgotten */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, 1.0f, pos, 1, 0, 0); t += 16 * MS; oot_realign_note(&r, t, 1.0f, pos, 0, 0, 0);
        assert(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_OFF, 0, 1, 0) == 0 && r.turn == 0);
        /* the view coming back after a flat stretch takes the turn too; while flat a due turn is applied at once */
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, 0.7f, pos, 1, 0, 0); r.due = 0;
        assert(near(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_QUICK, 0, 1, 1), 0.08f, 1e-5f) && near(r.easing, 0.62f, 1e-4f));
        memset(&r, 0, sizeof r); t = 1000 * MS; oot_realign_note(&r, t, 0, pos, 0, 0, 0);
        t += 16 * MS; oot_realign_note(&r, t, 0.7f, pos, 1, 0, 0); t += 16 * MS; oot_realign_note(&r, t, 0.7f, pos, 0, 0, 0);
        assert(near(oot_realign_take(&r, t, 0.016f, OOT_REALIGN_QUICK, 0, 0, 0), 0.7f, 1e-5f));
        /* not finite: ignored */
        oot_realign_note(&r, t + MS, NAN, pos, 1, 0, 0); assert(isfinite(r.turn));
    }
    puts("scenes: the style for each kind and option, the far camera, the director's heading, the seat, culling, the lighting, re-aligning");
    return 0;
}
