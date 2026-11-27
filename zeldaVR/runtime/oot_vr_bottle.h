/* Ocarina of Time in VR: a bottle in the player's hand (vr.md row 67), without the runtime (tests/test_oot_vr_bottle.c runs it).
   HookshotVR's simple bottle (held-items.md 6.2-6.3; GPL-3, ideas and the game's values only):
     - a fresh grip (in no gesture's zone) takes out the bottle on the item button: the game's own per-slot draw of equipment slot 1
       (main+0x6b6820), again every 300 ms until the bottle's Equipment says drawn, never while a scene of the game plays;
     - while held (being drawn or drawn), the game's own stow of slot 1 is refused (the game side's hook at main+0xe34cb0): Link's
       own AI asks it every frame, and before it was refused while drawing too, it overwrote the draw's request (batch16);
     - the trigger uses it, the game's own way: the hold is lifted and the bottle stowed (main+0xe34c70); once it no longer reads
       drawn the game's ZR is tapped for 130 ms (8 frames; held longer it put Link in a stance with the shield out) and the game takes
       the bottle out itself and plays its drink, swing or scene. Over 400 ms after that scene ends, or 1.5 s after the press with no
       scene; the stow not taken within 1 s drops the press. Then it is drawn into the hand again;
     - let go: the hold lifted, stowed every 300 ms while it reads drawn, at most four times.
   HookshotVR drinks potions, milk, fairies and Poes by calls of its own (heal, magic, content) with no scene; that needs the game's
   recovery request, not checked on our image, so here every content goes through the game's own use (its drink scene plays).
   The real ZR is the bottle's while one is selected (HookshotVR). */
#pragma once
#include "oot_vr_handitem.h"

enum { OOT_BOTTLE_IDLE, OOT_BOTTLE_HELD, OOT_BOTTLE_USING, OOT_BOTTLE_STOWING };
static const uint64_t OOT_BOTTLE_MS = 1000000ull;
typedef struct {
    int phase, hand, ready, scene_seen;
    uint64_t draw_at, use_at, tap_at, scene_end, stow_at;
    int stows;
    oot_ih_press grip[2], trigger;
} oot_bottle_state;
enum { OOT_BOTTLE_EV_TOOK = 1, OOT_BOTTLE_EV_USE, OOT_BOTTLE_EV_TAPPED, OOT_BOTTLE_EV_USED, OOT_BOTTLE_EV_DROPPED, OOT_BOTTLE_EV_LET_GO, OOT_BOTTLE_EV_GAVE_UP };
typedef struct {
    oot_ih_buttons b;
    int holding;          /* hand + 1 */
    int draw, stow;       /* make that call now */
    int keep_drawn;       /* the stow hook refuses the game's own stow of slot 1 */
    int event;
} oot_bottle_out;

static inline void oot_bottle_init(oot_bottle_state *s) { memset(s, 0, sizeof *s); s->hand = -1; }
static inline int oot_bottle_stow_due(oot_bottle_state *s, uint64_t now) {
    if (s->stow_at && now - s->stow_at < 300 * OOT_BOTTLE_MS) return 0;
    s->stow_at = now; s->stows++;
    return 1;
}
/* drawn: the bottle actor in slot 1 reads drawn (its Equipment's state 1). scene: a scene of the game is playing. */
static inline void oot_bottle_step(oot_bottle_state *s, const oot_ih_in *in, int active, int drawn, int scene, oot_bottle_out *o) {
    memset(o, 0, sizeof *o);
    const uint64_t now = in->now;
    if (!active || !in->hands) {
        if (s->phase == OOT_BOTTLE_HELD || s->phase == OOT_BOTTLE_USING) { s->phase = drawn ? OOT_BOTTLE_STOWING : OOT_BOTTLE_IDLE; s->stows = 0; s->stow_at = 0; s->hand = -1; }
        for (int i = 0; i < 2; i++) s->grip[i].closed = 0;
        s->trigger.closed = 0;
        if (s->phase == OOT_BOTTLE_STOWING) { if (!drawn || s->stows >= 4) s->phase = OOT_BOTTLE_IDLE; else o->stow = oot_bottle_stow_due(s, now); }
        return;
    }
    const oot_ih_frame *f = in->hands;
    int closing[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) closing[i] = oot_ih_hand_ok(&f->hand[i]) && oot_ih_press_step(&s->grip[i], f->hand[i].grip, 0.55f, 0.35f);
    o->b.mask |= OOT_IH_ZR;
    if (s->phase == OOT_BOTTLE_IDLE || s->phase == OOT_BOTTLE_STOWING) {
        for (int i = 1; i >= 0; i--) {
            if (!closing[i] || !oot_ih_free_zone(&f->hand[i], i == 1)) continue;
            s->hand = i; s->phase = OOT_BOTTLE_HELD; s->draw_at = 0; s->trigger.closed = 0;
            o->event = OOT_BOTTLE_EV_TOOK;
            break;
        }
    }
    if (s->phase == OOT_BOTTLE_STOWING) {
        if (!drawn) s->phase = OOT_BOTTLE_IDLE;
        else if (s->stows >= 4) { s->phase = OOT_BOTTLE_IDLE; o->event = OOT_BOTTLE_EV_GAVE_UP; }
        else o->stow = oot_bottle_stow_due(s, now);
        return;
    }
    if (s->phase == OOT_BOTTLE_IDLE) return;
    const oot_ih_hand *h = &f->hand[s->hand];
    const int pressed = oot_ih_press_step(&s->trigger, oot_ih_hand_ok(h) ? h->trigger : 0, 0.55f, 0.35f);
    o->b.triggers |= 1 << s->hand;
    o->holding = s->hand + 1;
    if (s->phase == OOT_BOTTLE_HELD) {
        if (!s->grip[s->hand].closed) {
            s->phase = OOT_BOTTLE_STOWING; s->stows = 0; s->stow_at = 0; s->hand = -1; o->holding = 0; o->event = OOT_BOTTLE_EV_LET_GO;
            if (drawn) o->stow = oot_bottle_stow_due(s, now); else s->phase = OOT_BOTTLE_IDLE;
            return;
        }
        o->keep_drawn = 1;   /* while it is being drawn too: Link's AI asks a stow of slot 1 every frame, which turned the draw's request
                                (2) into a stow (4) before the game took it (batch15/16, 2026-11-27: refused, it came out in 28 ms) */
        if (drawn) {
            if (pressed) {   /* used: the hold lifted, stowed; the game takes it out again itself */
                s->phase = OOT_BOTTLE_USING; s->use_at = now; s->ready = 0; s->scene_seen = 0; s->scene_end = 0; s->stows = 0; s->stow_at = 0;
                o->keep_drawn = 0; o->stow = oot_bottle_stow_due(s, now); o->event = OOT_BOTTLE_EV_USE;
            }
        } else if (!scene && (!s->draw_at || now - s->draw_at >= 300 * OOT_BOTTLE_MS)) { s->draw_at = now; o->draw = 1; }
        return;
    }
    /* using */
    if (!s->ready) {
        if (!drawn) { s->ready = 1; s->tap_at = now; o->event = OOT_BOTTLE_EV_TAPPED; }
        else if (now - s->use_at > 1000 * OOT_BOTTLE_MS) { s->phase = OOT_BOTTLE_HELD; s->draw_at = 0; o->event = OOT_BOTTLE_EV_DROPPED; return; }
        else o->stow = oot_bottle_stow_due(s, now);
    }
    if (s->ready) {
        if (now - s->tap_at < 130 * OOT_BOTTLE_MS) o->b.add |= OOT_IH_ZR;
        if (scene) { s->scene_seen = 1; s->scene_end = 0; }
        else if (s->scene_seen && !s->scene_end) s->scene_end = now;
        if ((s->scene_seen && s->scene_end && now - s->scene_end >= 400 * OOT_BOTTLE_MS) || (!s->scene_seen && now - s->use_at >= 1500 * OOT_BOTTLE_MS)) {
            s->phase = OOT_BOTTLE_HELD; s->draw_at = 0; o->event = OOT_BOTTLE_EV_USED;
        }
    }
}
