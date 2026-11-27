/* Ocarina of Time in VR: the items held in the player's hands, the game's side (vr.md rows 63-67; HookshotVR's way, ideas and
   addresses only, GPL-3). The decisions are pure and unit-tested (zeldaVR/runtime/oot_vr_hookshot.h, oot_vr_boomerang.h,
   oot_vr_bottle.h, oot_vr_bomb.h); here they get their input and their answers reach the game:
     - the hands: the graphics side sends them once a frame in the game's world (oot_vr_arms.inc, runtime_oot_vr_hands_world);
     - the item on the item button: the game's own query, as QueryGetSelectedAssignableItem (main+0x2a1d940, read on our image) makes
       it: main+0x17b6e0(pouch = *(*(main+0x4c7e0d0)), the selection byte *(*(main+0x4e7cfa8) + 0x5c0), 0) -> 0..0x35, -1 none,
       asked every 100 ms from Link's update, answered in the next (since the merge of 2026-11-27 asked once for both users, by
       oot_vr_bowstate.h, and read here);
     - Link's action state, components, actor: his own update (oot_vr.h);
     - buttons: the pad's thread adds and takes away what the items say (runtime_oot_vr_item_buttons, zeldaVR/gfx/totk_vr.m);
     - calls, all from Link's hit-sensor update (oot_vr.h oot_link_calc), on the game's thread: the put-away main+0x4a8690(Link),
       the bottle's per-slot draw main+0x6b6820(EquipmentUser, 1) and stow main+0xe34c70(EquipmentUser, 1) (entries, pacibsp, and how
       both take x0 = the EquipmentUser and x1 = the slot read on our image).
   Hooks (oot_vr.h OOT_VR_HOOKS, each word read on our image):
     main+0x2fec420 fsub s2, s2, s10 (in main+0x2fec340, the aim check's update, x19 = the check from main+0x2fec378 to its end, its
       +0x38 the actor whose components +0x200 hold the equipment, read at main+0x2fec374 / 0x2fec46c): s11 s12 s10 = the camera's
       place, s0 s1 s2 = where it looks minus that, normalised just after (main+0x2fec42c..0x2fec464): the hookshot's line in the hand;
     main+0x2fed8a4 stp s1, s2, [x19, #0xa8]: the check's start (+0xa4..+0xac, written by it and the stp before) = the hand's line;
     main+0x2a0b2a8 add x0, x19, #0x20 (in main+0x2a0b220, QueryGetCameraForwardDir's calc): the point straight ahead of the camera
       has just been written at the site's sp+0x14 by main+0x2b94c50 (its x0 = sp+0x14 at main+0x2a0b284), x21 = the node's actor
       (main+0x2a0b240); copied out at main+0x2a0b2ac: the boomerang's flight is told the point along the player's aim;
     main+0xe34cb0 ldr w8, [x25, #0x10] (in the stow main+0xe34c70: x25 the slot's handle, x0 the EquipmentUser, x1 the slot): x8 =
       0xffffffff takes the stow's own "no item in the handle" return (main+0xe34cb4), so the held bottle stays drawn.
   Everything fails safe: no fresh hands, no fresh item, a read that does not look right, and nothing is added or taken away. */
#pragma once
#include "../zeldaVR/runtime/oot_vr_hookshot.h"
#include "../zeldaVR/runtime/oot_vr_boomerang.h"
#include "../zeldaVR/runtime/oot_vr_bottle.h"

int vr_oot_hookshot_hands(void);   /* zeldaVR/gfx/totk_vr.m, the VR page */
int vr_oot_boomerang_hands(void);
int vr_oot_boomerang_throw(void);
int vr_oot_bottle_hands(void);

static const uint64_t OOT_EQUIP_DRAW = 0x6b6820, OOT_EQUIP_STOW = 0xe34c70;   /* OOT_ITEM_SELECTION_SLOT: oot_vr_bowstate.h */
static const uint64_t OOT_IH_HANDS_FRESH = 150000000ull, OOT_IH_ITEM_FRESH = 500000000ull, OOT_IH_ASK_GAP = 100000000ull;

static struct { pthread_mutex_t mu; oot_ih_frame f; } oot_ih_hands = { .mu = PTHREAD_MUTEX_INITIALIZER };
void runtime_oot_vr_hands_world(const oot_ih_frame *f) {   /* the graphics side, once a frame */
    pthread_mutex_lock(&oot_ih_hands.mu); oot_ih_hands.f = *f; oot_ih_hands.f.at = oot_vr_now(); pthread_mutex_unlock(&oot_ih_hands.mu);
    static int said; if (!said) { said = 1; fprintf(stderr, "[vr-items] the hands reach the game: left %d, right %d, head %d\n", f->hand[0].have, f->hand[1].have, f->have_head); }
}
/* What the pad's thread applies, and what the graphics side shows. */
static struct { pthread_mutex_t mu; oot_ih_buttons b; uint64_t at; int item_hand, aim, aim_hand; } oot_ih_pad = { .mu = PTHREAD_MUTEX_INITIALIZER, .item_hand = -1 };
static int oot_ih_pad_fresh(void) { return oot_ih_pad.at && oot_vr_now() - oot_ih_pad.at < 200000000ull; }   /* under its lock */
int runtime_oot_vr_item_buttons(uint32_t *add, uint32_t *mask, int *triggers) {
    pthread_mutex_lock(&oot_ih_pad.mu);
    const int fresh = oot_ih_pad_fresh();
    *add = fresh ? oot_ih_pad.b.add : 0; *mask = fresh ? oot_ih_pad.b.mask : 0; *triggers = fresh ? oot_ih_pad.b.triggers : 0;
    pthread_mutex_unlock(&oot_ih_pad.mu);
    return fresh;
}
int runtime_oot_vr_item_hand(void) {
    pthread_mutex_lock(&oot_ih_pad.mu); const int h = oot_ih_pad_fresh() ? oot_ih_pad.item_hand : -1; pthread_mutex_unlock(&oot_ih_pad.mu);
    return h;
}
/* The boomerang is being aimed: the reticle along that hand's line (-1 the headset's). */
int runtime_oot_vr_boomerang_aim(int *hand) {
    pthread_mutex_lock(&oot_ih_pad.mu); const int on = oot_ih_pad_fresh() && oot_ih_pad.aim; *hand = oot_ih_pad.aim_hand; pthread_mutex_unlock(&oot_ih_pad.mu);
    return on;
}

/* Link's update only: */
static oot_guest_call oot_ih_calls[2];
static int oot_ih_item = -1;
static uint64_t oot_ih_item_at;
static oot_hook_state oot_ih_hook; static oot_boom_state oot_ih_boom; static oot_bottle_state oot_ih_bottle;
static int oot_ih_started;
/* The hooks' (any game thread): */
static struct { pthread_mutex_t mu; float o[3], d[3]; uint64_t at; float target[3]; uint64_t thrown_at; } oot_ih_lines = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uintptr_t oot_ih_bottle_user;   /* the EquipmentUser whose slot 1 the hook keeps drawn, 0 none */
static _Atomic int oot_ih_scene;               /* a scene of the game plays (the event manager's +104) */

static const char *oot_ih_hand_name(int h) { return h == 0 ? "left" : h == 1 ? "right" : "?"; }
/* The item on the item button. Merge 2026-11-27: one question for the bow's grip and these items, asked by oot_vr_bowstate.h
   (oot_bowstate_calc, earlier in the same chain, in the game's own form) every 100 ms; here its last answer is read. */
static oot_guest_call *oot_ih_select(uint64_t now) {
    (void)now;
    const uint64_t at = atomic_load(&oot_bow_item_at);
    if (!at || at == oot_ih_item_at) return NULL;
    const int a = atomic_load(&oot_bow_item), item = a >= 0 && a <= 0x35 ? a : -1;
    if (item != oot_ih_item) fprintf(stderr, "[vr-items] %.3f the item button holds item %d (was %d)\n", (double)at / 1e9, item, oot_ih_item);
    oot_ih_item = item; oot_ih_item_at = at;
    return NULL;
}
#include "oot_vr_bombs.h"

/* The hookshot's line for the two aim hooks: the check must be Link's, the line fresh. */
static int oot_ih_hook_line(uint64_t check, float o[3], float d[3]) {
    uint64_t owner = 0;
    if (!check || !oot_vr_safe_read((uintptr_t)check + 0x38, &owner, 8) || !owner || owner != atomic_load_explicit(&oot_link_actor, memory_order_relaxed)) return 0;
    pthread_mutex_lock(&oot_ih_lines.mu);
    const int fresh = oot_ih_lines.at && oot_vr_now() - oot_ih_lines.at < 100000000ull;
    memcpy(o, oot_ih_lines.o, 12); memcpy(d, oot_ih_lines.d, 12);
    pthread_mutex_unlock(&oot_ih_lines.mu);
    return fresh && oot_ih_finite(o, 3) && oot_ih_finite(d, 3);
}
static void oot_ih_set_s(oot_hook_regs *r, int reg, float v) { memcpy(r->q[reg], &v, 4); }
static oot_guest_call *oot_ih_hook_aim(oot_hook_regs *r) {   /* main+0x2fec420, after */
    float o[3], d[3];
    if (!oot_ih_hook_line(r->x[19], o, d)) return NULL;
    oot_ih_set_s(r, 11, o[0]); oot_ih_set_s(r, 12, o[1]); oot_ih_set_s(r, 10, o[2]);
    oot_ih_set_s(r, 0, d[0]); oot_ih_set_s(r, 1, d[1]); oot_ih_set_s(r, 2, d[2]);
    static _Atomic int lines; if (atomic_fetch_add(&lines, 1) < 5) fprintf(stderr, "[vr-hookshot] the game's aim check goes along the hand: from %.2f %.2f %.2f toward %.3f %.3f %.3f\n",
                                                                         (double)o[0], (double)o[1], (double)o[2], (double)d[0], (double)d[1], (double)d[2]);
    return NULL;
}
static oot_guest_call *oot_ih_hook_start(oot_hook_regs *r) {   /* main+0x2fed8a4, after */
    float o[3], d[3];
    if (!oot_ih_hook_line(r->x[19], o, d)) return NULL;
    memcpy((void *)(uintptr_t)(r->x[19] + 0xa4), o, 12);
    return NULL;
}
static oot_guest_call *oot_ih_boom_aim(oot_hook_regs *r) {   /* main+0x2a0b2a8, after */
    static _Atomic uintptr_t boomerang;   /* the actor last found to be the boomerang */
    pthread_mutex_lock(&oot_ih_lines.mu);
    const int fresh = oot_ih_lines.thrown_at && oot_vr_now() - oot_ih_lines.thrown_at < 1500000000ull;
    float t[3]; memcpy(t, oot_ih_lines.target, 12);
    pthread_mutex_unlock(&oot_ih_lines.mu);
    const uintptr_t actor = (uintptr_t)r->x[21];
    if (!fresh || !actor || !oot_ih_finite(t, 3)) return NULL;
    if (actor != atomic_load(&boomerang)) { if (!oot_actor_named(actor, "Weapon_Boomerang")) return NULL; atomic_store(&boomerang, actor); }
    float *at = (float *)(void *)((uint8_t *)r + OOT_HOOK_FRAME + 16 + 0x14), was[3]; memcpy(was, at, 12);
    memcpy(at, t, 12);
    static _Atomic int lines; if (atomic_fetch_add(&lines, 1) < 20) fprintf(stderr, "[vr-boomerang] %.3f the flight heads for %.2f %.2f %.2f (the game's own point was %.2f %.2f %.2f)\n",
                                                                          (double)oot_vr_now() / 1e9, (double)t[0], (double)t[1], (double)t[2], (double)was[0], (double)was[1], (double)was[2]);
    return NULL;
}
static oot_guest_call *oot_ih_bottle_keep(oot_hook_regs *r) {   /* main+0xe34cb0, after */
    const uintptr_t user = atomic_load_explicit(&oot_ih_bottle_user, memory_order_relaxed);
    if (!user || r->x[0] != user || (uint32_t)r->x[1] != 1 || atomic_load_explicit(&oot_ih_scene, memory_order_relaxed)) return NULL;
    r->x[8] = 0xffffffffull;
    static _Atomic int lines; if (atomic_fetch_add(&lines, 1) < 3) fprintf(stderr, "[vr-bottle] the game's own stow of the held bottle is refused (it stays drawn)\n");
    return NULL;
}

static void oot_ih_log(const char *topic, int event, const char *const *what, int n, int hand, uint32_t action) {
    if (event > 0 && event < n) fprintf(stderr, "[vr-%s] %.3f %s (%s hand, action %#x)\n", topic, (double)oot_vr_now() / 1e9, what[event], oot_ih_hand_name(hand), action);
}
/* Link's hit-sensor update: every item's step, its calls in one chain (NULL none). */
static oot_guest_call *oot_hand_items_calc(void) {
    if (!oot_vr_base) return NULL;
    if (!oot_ih_started) { oot_ih_started = 1; oot_hook_init(&oot_ih_hook); oot_boom_init(&oot_ih_boom); oot_bottle_init(&oot_ih_bottle); }
    const uint64_t now = oot_vr_now(); const uintptr_t base = (uintptr_t)oot_vr_base;
    oot_guest_call *head = oot_ih_select(now), **link = head ? &head->next : &head;
    if (head) head->next = NULL;
    oot_ih_frame f; pthread_mutex_lock(&oot_ih_hands.mu); f = oot_ih_hands.f; pthread_mutex_unlock(&oot_ih_hands.mu);
    const int hands = f.at && now - f.at < OOT_IH_HANDS_FRESH;
    const int item = oot_ih_item_at && now - oot_ih_item_at < OOT_IH_ITEM_FRESH ? oot_ih_item : -1, kind = oot_ih_kind(item);
    { uint64_t holder = 0, events = 0, word = 0;
      atomic_store(&oot_ih_scene, oot_vr_safe_read(base + OOT_VR_EVENT_SLOT, &holder, 8) && holder && oot_vr_safe_read((uintptr_t)holder, &events, 8) && events &&
                                  oot_vr_safe_read((uintptr_t)events + 104, &word, 8) && word != 0); }
    oot_ih_in in = { now, hands ? &f : NULL, item, atomic_load(&oot_vr_action_now), runtime_oot_vr_fp_active(), runtime_oot_vr_in_event() };
    const int ready = in.hands && in.first_person && !in.in_event;
    oot_ih_buttons b = { 0, 0, 0 };
    /* the hookshot and longshot */
    oot_hook_out_t ho; const int hook_hand = oot_ih_hook.hand;
    oot_hook_step(&oot_ih_hook, &in, ready && kind == OOT_IH_HOOKSHOT && vr_oot_hookshot_hands(), &ho);
    { static const char *const what[] = { "", "taken", "a shot (the trigger let go)", "let go: the game's put-away", "put away FAILED after four calls" };
      oot_ih_log("hookshot", ho.event, what, 5, ho.event == OOT_HOOK_TOOK ? oot_ih_hook.hand : hook_hand, in.action); }
    oot_ih_merge(&b, &ho.b);
    if (ho.holding > 0) {
        const oot_ih_hand *h = &f.hand[ho.holding - 1];
        pthread_mutex_lock(&oot_ih_lines.mu);
        for (int i = 0; i < 3; i++) { oot_ih_lines.d[i] = h->dir[i]; oot_ih_lines.o[i] = h->pos[i] + 0.05f * h->dir[i]; }   /* 5 cm ahead of the hand */
        oot_ih_lines.at = now;
        pthread_mutex_unlock(&oot_ih_lines.mu);
    }
    /* the boomerang */
    oot_boom_out bo; const int mode = vr_oot_boomerang_throw(), boom_hand = oot_ih_boom.hand;
    oot_boom_step(&oot_ih_boom, &in, ready && kind == OOT_IH_BOOMERANG && vr_oot_boomerang_hands(), mode, &bo);
    oot_ih_merge(&b, &bo.b);
    if (bo.event == OOT_BOOM_EV_THROWN && oot_ih_boom.have_line) {
        float t[3]; const float *start = oot_ih_boom.have_start ? oot_ih_boom.start : oot_ih_boom.line_o;
        oot_boom_target(oot_ih_boom.line_o, oot_ih_boom.line_d, start, t);
        pthread_mutex_lock(&oot_ih_lines.mu); memcpy(oot_ih_lines.target, t, 12); oot_ih_lines.thrown_at = now; pthread_mutex_unlock(&oot_ih_lines.mu);
        fprintf(stderr, "[vr-boomerang] %.3f thrown from the %s hand along %s line %.3f %.3f %.3f: the flight is to head for %.2f %.2f %.2f\n", (double)now / 1e9, oot_ih_hand_name(boom_hand),
                mode == 2 ? "the headset's" : mode == 1 ? "the other hand's" : "its own", (double)oot_ih_boom.line_d[0], (double)oot_ih_boom.line_d[1], (double)oot_ih_boom.line_d[2], (double)t[0], (double)t[1], (double)t[2]);
    } else {
        static const char *const what[] = { "", "taken", "thrown with no line to follow: the game's own aim", "back in the hand", "let go: put away", "put away FAILED after four calls: B" };
        oot_ih_log("boomerang", bo.event, what, 6, bo.event == OOT_BOOM_EV_TOOK ? oot_ih_boom.hand : boom_hand, in.action);
    }
    /* a bottle: the one in equipment slot 1, as the game makes it for the bottle on the item button */
    const uintptr_t components = atomic_load_explicit(&oot_aim_components, memory_order_relaxed);
    uint8_t slot_state = 0; uint64_t user = 0;
    const uintptr_t bottle = components ? oot_vr_equip_actor(components, 1, &slot_state) : 0;
    const int is_bottle = bottle && oot_actor_named(bottle, "Weapon_Bottle") && oot_vr_safe_read(components + OOT_EQUIP_OF_COMPONENTS, &user, 8) && user;
    oot_bottle_out to; const int bottle_hand = oot_ih_bottle.hand;
    oot_bottle_step(&oot_ih_bottle, &in, ready && kind == OOT_IH_BOTTLE && is_bottle && vr_oot_bottle_hands(), is_bottle && oot_equip_drawn(bottle), atomic_load(&oot_ih_scene), &to);
    oot_ih_merge(&b, &to.b);
    atomic_store(&oot_ih_bottle_user, to.keep_drawn && is_bottle ? (uintptr_t)user : 0);
    { static const char *const what[] = { "", "taken out", "used: stowed for the game's own use", "stowed: the game's ZR tapped", "used, back in the hand",
                                          "the stow was not taken: the press dropped", "let go: stowed", "stow FAILED after four calls" };
      oot_ih_log("bottle", to.event, what, 8, to.event == OOT_BOTTLE_EV_TOOK ? oot_ih_bottle.hand : bottle_hand, in.action); }
    int n = 0;
    const uintptr_t link_actor = atomic_load_explicit(&oot_link_actor, memory_order_relaxed);
    if ((ho.put_away || bo.put_away) && link_actor) oot_ih_calls[n++] = (oot_guest_call){ (uint64_t)(base + OOT_SHEATHE_ALL), (uint64_t)link_actor, 0, 0, 0, 0, NULL, 0 };
    if ((to.draw || to.stow) && is_bottle) oot_ih_calls[n++] = (oot_guest_call){ (uint64_t)(base + (to.draw ? OOT_EQUIP_DRAW : OOT_EQUIP_STOW)), user, 1, 0, 0, 0, NULL, 0 };
    for (int i = 0; i < n; i++) { *link = &oot_ih_calls[i]; link = &oot_ih_calls[i].next; }
    /* the bombs */
    oot_ih_buttons bb = { 0, 0, 0 };
    *link = oot_bombs_link_calc(&in, kind == OOT_IH_BOMBS, &bb);
    oot_ih_merge(&b, &bb);
    pthread_mutex_lock(&oot_ih_pad.mu);
    oot_ih_pad.b = b; oot_ih_pad.at = now;
    oot_ih_pad.item_hand = ho.holding > 0 ? ho.holding - 1 : bo.holding > 0 ? bo.holding - 1 : to.holding > 0 ? to.holding - 1 : -1;
    oot_ih_pad.aim = bo.aiming; oot_ih_pad.aim_hand = bo.aim_hand;
    pthread_mutex_unlock(&oot_ih_pad.mu);
    return head;
}
