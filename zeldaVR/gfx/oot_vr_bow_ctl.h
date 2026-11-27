/* zeldaVR, Ocarina of Time: the bow and the slingshot in the player's hands, the pad's side (vr.md rows 54, 55, 60, 61; HookshotVR's
   notes, GPL-3: facts and ideas, no code). The VR page's four options, the string's solution as the graphics side last made it
   (games/oot/third_party/d2r_gfx/gfx/nvn/oot_vr_bow.inc), and the grip that takes, holds and puts away the bow
   (zeldaVR/runtime/oot_vr_bowgrip.h), stepped on the pad's thread from oot_gamepad. Included by totk_vr.m. */
#include "../runtime/oot_vr_bowgrip.h"
#include "../runtime/oot_vr_string.h"
extern int runtime_oot_vr_bow_out(void) __attribute__((weak_import));        /* games/oot/runtime/oot_vr_bowstate.h */
extern int runtime_oot_vr_bow_selected(void) __attribute__((weak_import));
extern void runtime_oot_vr_put_away_cancel(void) __attribute__((weak_import));   /* games/oot/runtime/oot_vr_putaway.h */

/* A grip takes the bow out and holds it, letting go puts it away (option bow_grip; HookshotVR's built-in default is on, ours off
   since the merge of 2026-11-27: it takes over the grips and ZR, so it waits until it is verified in the game). */
int vr_oot_bow_grip(void) { return vr_option("bow_grip", 0) != 0; }
void vr_oot_set_bow_grip(int on) { vr_option_set("bow_grip", on != 0); VLOG("the bow: %s", on ? "a grip takes it and holds it" : "the item button takes it"); }
/* What grabs the string with the hand that does not hold the bow (option string_input: 0 trigger or grip, HookshotVR's default; 1 the
   trigger; 2 the grip; 3 none: no string, the item button draws and shoots as before 2026-11-27, our default since the merge of that
   day: the play players have now until the string is verified in the game). */
int vr_oot_string_input(void) { const int v = vr_option("string_input", 3); return v < 0 || v > 3 ? 3 : v; }
void vr_oot_set_string_input(int v) { vr_option_set("string_input", v < 0 || v > 3 ? 3 : v);
    VLOG("the string: %s", v == 1 ? "grabbed with the trigger" : v == 2 ? "grabbed with the grip" : v == 3 ? "not used, the item button shoots" : "grabbed with the trigger or the grip"); }
/* The bow's size in the hand, % (option bow_size, 80..200; HookshotVR's default 130, ours 100 since the merge of 2026-11-27: the
   bow as players see it now; the slingshot keeps its own). */
int vr_oot_bow_size(void) { const int v = vr_option("bow_size", 100); return v < 80 ? 80 : v > 200 ? 200 : v; }
void vr_oot_set_bow_size(int v) { vr_option_set("bow_size", v < 80 ? 80 : v > 200 ? 200 : v); VLOG("the bow: %d%% of its size", vr_oot_bow_size()); }
/* How strongly a fire, ice or light arrow's effects show on the arrow on the VR string, % (option arrow_fx, 0..100, 20: HookshotVR's). */
int vr_oot_arrow_fx(void) { const int v = vr_option("arrow_fx", 20); return v < 0 ? 0 : v > 100 ? 100 : v; }
void vr_oot_set_arrow_fx(int v) { vr_option_set("arrow_fx", v < 0 ? 0 : v > 100 ? 100 : v); VLOG("magic arrow effects: %d%%", vr_oot_arrow_fx()); }

/* The string's solution, from the graphics side once a game frame while the bow or slingshot is in the player's hand. */
static _Atomic int g_bow_flags, g_bow_hand_drawn = -1, g_bow_hand_held = -1;
static _Atomic uint32_t g_bow_draw_bits, g_bow_shots;
static _Atomic uint64_t g_bow_at;
void vr_oot_bow_publish(int flags, float draw, int fired, int bow_hand) {
    uint32_t b; memcpy(&b, &draw, 4); atomic_store(&g_bow_draw_bits, b);
    atomic_store(&g_bow_flags, flags); atomic_store(&g_bow_hand_drawn, bow_hand);
    if (fired) atomic_fetch_add(&g_bow_shots, 1);
    atomic_store(&g_bow_at, now_ns());
}
static int oot_bow_fresh(void) { const uint64_t at = atomic_load(&g_bow_at); return at && now_ns() - at < 100000000ull; }
/* The hand the grip took the bow with (the graphics side puts it there), -1 none. */
int vr_oot_bow_hand(void) { return atomic_load(&g_bow_hand_held); }
/* The hand the bow is drawn in these last frames (the aim's mark), -1 none. */
static int oot_bow_hand_drawn(void) { return oot_bow_fresh() ? atomic_load(&g_bow_hand_drawn) : -1; }
int vr_oot_bow_menu(void) { return oot_wheel_is_open(); }

/* oot_gamepad, after the pad's own map, before the gestures: the bow's grip. hands: the gesture hands (lefty: swapped), gs the
   gestures' state of the last step. Changes the game's ZR in *bits and takes the bow's grips from the gestures. */
static void oot_bow_pad(uint64_t now, const vr_controls *c, int lefty, int first_person, int wheel_open, oot_hand hands[2], int have_hands,
                        const oot_gesture_state *gs, uint64_t *bits) {
    static oot_bowgrip st; static oot_string_buzz buzz; static uint32_t shots_seen;
    oot_bowgrip_in in; memset(&in, 0, sizeof in);
    in.now = now; in.grip[0] = c->lgrip; in.grip[1] = c->rgrip;
    in.first_person = first_person && have_hands; in.grip_hold = vr_oot_bow_grip();
    in.bow_selected = runtime_oot_vr_bow_selected && runtime_oot_vr_bow_selected() > 0;
    in.bow_out = runtime_oot_vr_bow_out && runtime_oot_vr_bow_out() > 0;
    in.wheel_open = wheel_open;
    in.fresh = oot_bow_fresh();
    const int flags = in.fresh ? atomic_load(&g_bow_flags) : 0;
    in.held = (flags & OOT_STRING_HELD) != 0; in.committed = (flags & OOT_STRING_COMMITTED) != 0;
    in.real_zr = (int)((*bits >> OOT_SW_ZR) & 1);
    for (int k = 0; k < 2 && have_hands; k++) {   /* the gesture hand of controller k */
        const int i = lefty ? 1 - k : k;
        in.claimed[k] = (gs->sword && i == 1) || (gs->shield && i == 0) || (gs->carrying && gs->carry_hand == i) || oot_gesture_behind(&hands[i], i == 1);
    }
    oot_bowgrip_out o; oot_bowgrip_step(&st, &in, &o);
    *bits = (*bits & ~(1ull << OOT_SW_ZR)) | ((uint64_t)(o.zr != 0) << OOT_SW_ZR);
    atomic_store(&g_bow_hand_held, o.bow_hand);
    if (o.bow_hand >= 0 && vr_oot_string_input() != OOT_STRING_GRIP && vr_oot_string_input() != OOT_STRING_NONE) {   /* the drawing hand's trigger grabs the string, not the game's ZL */
        const int pull = 1 - o.bow_hand, zl_from = lefty ? 1 : 0;
        if (pull == zl_from) *bits &= ~(1ull << OOT_SW_ZL);
    }
    for (int k = 0; k < 2 && have_hands; k++) if (o.owns[k]) hands[lefty ? 1 - k : k].grip = 0;   /* the bow's grip is no gesture */
    if (o.put_away && runtime_oot_vr_put_away && runtime_oot_vr_hooks_on && runtime_oot_vr_hooks_on()) runtime_oot_vr_put_away();
    if (o.cancel_put_away && runtime_oot_vr_put_away_cancel) runtime_oot_vr_put_away_cancel();
    if (o.said) VLOG("[vr-bow] %s (%s hand)", o.said, st.bow_hand ? "right" : "left");
    {   /* the draw's rumble on the drawing hand; a shot's on the bow hand */
        const int bow_hand = o.bow_hand >= 0 ? o.bow_hand : oot_bow_hand_drawn();
        float draw; uint32_t b = atomic_load(&g_bow_draw_bits); memcpy(&draw, &b, 4); float a = 0;
        if (bow_hand >= 0 && in.held && oot_string_buzz_step(&buzz, draw, now, &a)) vr_link_send_haptic(1 - bow_hand, 120000000ull, 160, a);
        const uint32_t shots = atomic_load(&g_bow_shots);
        if (shots != shots_seen) { shots_seen = shots; if (bow_hand >= 0) vr_link_send_haptic(bow_hand, 90000000ull, 160, 1.0f); VLOG("[vr-bow] the string's let-go: a shot (%u)", shots); }
    }
}
