/* Ocarina of Time in VR: bombs in the player's hands, the game's side (vr.md rows 65-66; HookshotVR's way, held-items.md 5, ideas and
   addresses only, GPL-3). The decisions are zeldaVR/runtime/oot_vr_bomb.h (unit-tested). Included by oot_vr_hand_items.h.
   Where each piece is, read on our image:
     - every bomb's own update: the hook at the entry of ExecuteBarrelBombAI's update, main+0x26384b0 (pacibsp); its x0 is the AI node
       and [[[x0+8]+8]+0x1f0] the bomb actor (main+0x26384d4..0x26384e4). Its calls are made there, on the bomb's own thread:
         place      main+0x4c6630(physics = *(actor+0x270)+0x50, &{x, y, z, 0}, &rotation[9], 1); it reads no s0 (both paths write it
                    first, main+0x4c6664 / 0x4c6788);
         velocity   main+0x12dbe0(body, &v[3], 0), the body = actor+0x270 -> +0x50 -> +0x20 -> +0x190 (main+0x973a50), as the bomb AI
                    itself calls it (main+0x26386b0..0x26386c0); spin main+0x12b790(body, &w[3]) reads [x1] the same way;
         layer      main+0x127840(body, layer), as the game's layer node main+0x2902bb0 calls it (w1 the layer; that x0 is the body is
                    HookshotVR's note, not read here);
         fuse frame main+0x37b5d70(controller, slot 0, 0, 0, s0 = frame) (w1 the slot, s0 the frame: main+0x26202f0 at 0x2620384); the
                    controller actor+0x270 -> +0x40 -> +0x18 -> +0x18 (main+0x1081f60); its current command, as main+0x37b5d00 reads
                    it: count +0x18 > 0, *(*(controller+0x20)) + 0x10;
         effects    asleep main+0x974100(xlink, 0x40000000), awake main+0x974060(xlink, 0x40000000), xlink = *(actor+0x270)+0x68
                    (main+0x974040); 0x40000000 is HookshotVR's own reason bit, the game's AI uses bit 0;
     - Link's lift let go: main+0x2e50db0(lifter = *(Link+0x270)+0x2c8, 1, s0 = 1.0, s1 = 0), as the game's own node main+0x28e8700
       makes it (main+0x973230 for the lifter), from Link's update;
     - the Goron's Bracelet and the gauntlets: the pouch's counts (main+0x5963e0, oot_vr_wheel_host.h) every 500 ms;
     - Bomb Flowers' fruits waiting on their plants run no bomb AI: found by name from every actor's update (oot_vr.h oot_vr_player).
   The fuse frame and the lift's let-go take floats, and the shared hook body passes x0-x4 only (oot_vr_hooks.h): two thunks of three
   words after the hook stubs, `fmov s0, w4; fmov s1, w3; b <the function>`, take s0 from x4 and s1 from x3 (words 1e270080 and
   1e270061, assembled by clang). Without room for them the bombs keep the game's own fuse and lift. */
#pragma once
#include "../zeldaVR/runtime/oot_vr_bomb.h"

int vr_oot_bomb_hands(void);   /* zeldaVR/gfx/totk_vr.m, the VR page */
int vr_oot_bomb_throw_mode(void);
int vr_oot_bomb_strength(void);
int vr_oot_bomb_pull(void);
int vr_oot_bomb_reach_dm(void);
int vr_oot_bomb_cone(void);
int vr_oot_bomb_flowers(void);

static const uint64_t OOT_BOMB_AI = 0x26384b0, OOT_BOMB_PLACE = 0x4c6630, OOT_BOMB_VELOCITY = 0x12dbe0, OOT_BOMB_SPIN = 0x12b790, OOT_BOMB_LAYER_FN = 0x127840,
                      OOT_BOMB_FRAME = 0x37b5d70, OOT_BOMB_SLEEP = 0x974100, OOT_BOMB_WAKE = 0x974060, OOT_LIFTER_FALL = 0x2e50db0;
static const uint32_t OOT_BOMB_SLEEP_REASON = 0x40000000u;
static const char *const OOT_BRACELETS[4] = { "Item_Gauntlet_Goron", "Armor_Child_Gauntlet_Goron", "Armor_Adult_Gauntlet_Silver", "Armor_Adult_Gauntlet_Gold" };

static struct { pthread_mutex_t mu; oot_bombs s; oot_bomb_opts opts; int started; } oot_bm = { .mu = PTHREAD_MUTEX_INITIALIZER };
static uintptr_t oot_bm_thunk_frame, oot_bm_thunk_fall;   /* the float thunks, 0 none */
static _Atomic int oot_bm_on, oot_bm_scan;               /* the bomb hook works; the flower scan works */
static _Atomic uint32_t oot_bm_feet[3];

/* After the hook stubs (oot_vr.h oot_vr_hooks_install): the two thunks; the end of what was written. */
static uint64_t oot_bombs_thunks_install(Module *m, uint64_t at, uint64_t limit) {
    at = (at + 15) & ~15ull;
    const uint64_t to[2] = { OOT_BOMB_FRAME, OOT_LIFTER_FALL };
    if (at + 24 > limit) { fprintf(stderr, "[vr-bomb] no room for the float thunks: bombs keep the game's own fuse and lift\n"); return at; }
    for (uint64_t k = at; k < at + 24; k++) if (m->base[k]) { fprintf(stderr, "[vr-bomb] the thunks' room is in use: bombs keep the game's own fuse and lift\n"); return at; }
    for (int i = 0; i < 2; i++) {
        const uint64_t t = at + 12 * (uint64_t)i;
        const uint32_t w[3] = { 0x1e270080u, 0x1e270061u, oot_vr_b(t + 8, to[i]) };   /* fmov s0, w4; fmov s1, w3; b */
        if (!w[2]) { fprintf(stderr, "[vr-bomb] a float thunk is out of branch range\n"); return at; }
        memcpy(m->base + t, w, sizeof w);
    }
    oot_bm_thunk_frame = (uintptr_t)m->base + at; oot_bm_thunk_fall = (uintptr_t)m->base + at + 12;
    return at + 24;
}
static uint32_t oot_bm_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static int oot_bm_read(uintptr_t a, void *out, size_t n) { return a && oot_vr_safe_read(a, out, n); }
static uintptr_t oot_bm_ptr(uintptr_t a) { uint64_t v = 0; return oot_bm_read(a, &v, 8) ? (uintptr_t)v : 0; }
static void oot_bm_feet_now(float f[3]) { for (int i = 0; i < 3; i++) f[i] = oot_vr_bits_float(atomic_load(&oot_bm_feet[i])); }
static int oot_bm_where(uintptr_t actor, float origin[3], float rot[9]) {
    return oot_bm_read(actor + OOT_ACTOR_POS, origin, 12) && oot_bm_read(actor + OOT_ACTOR_ROT, rot, 36) && oot_ih_finite(origin, 3) && oot_ih_finite(rot, 9);
}
static void oot_bm_say(int ev, int hand, uintptr_t actor, float speed) {
    static const char *const what[] = { "", "held", "thrown", "lit", "pulled toward the hand", "taken from the bag (the game's ZR)", "none came out of the bag",
                                        "the game asked to pick a Bomb Flower (its A)", "Bomb Flower picked into the hand", "Bomb Flower not picked (Link not beside it, or not facing it)", "let go mid-pull" };
    static _Atomic int lines;
    if (ev <= 0 || ev > OOT_BOMB_EV_DROPPED || atomic_fetch_add(&lines, 1) >= 400) return;
    if (ev == OOT_BOMB_EV_THROWN) fprintf(stderr, "[vr-bomb] %.3f %#llx thrown from the %s hand at %.1f m/s\n", (double)oot_vr_now() / 1e9, (unsigned long long)actor, oot_ih_hand_name(hand), (double)speed);
    else fprintf(stderr, "[vr-bomb] %.3f %#llx %s (%s hand)\n", (double)oot_vr_now() / 1e9, (unsigned long long)actor, what[ev], oot_ih_hand_name(hand));
}

/* main+0x26384b0: a bomb's own update. */
static oot_guest_call *oot_bomb_ai(oot_hook_regs *r) {
    static _Thread_local oot_guest_call c[6]; static _Thread_local float pos[4], rot_out[9], v[3], none[3];
    if (!atomic_load_explicit(&oot_bm_on, memory_order_relaxed) || !oot_vr_base) return NULL;
    const uintptr_t actor = oot_bm_ptr(oot_bm_ptr(oot_bm_ptr((uintptr_t)r->x[0] + 8) + 8) + 0x1f0);
    float origin[3], rot[9], centre[3], feet[3];
    if (!actor || !oot_bm_where(actor, origin, rot)) return NULL;
    oot_bomb_centre(origin, rot, centre); oot_bm_feet_now(feet);
    const uintptr_t parts = oot_bm_ptr(actor + 0x270), physics = oot_bm_ptr(parts + 0x50), body = oot_bm_ptr(oot_bm_ptr(physics + 0x20) + 0x190);
    const uintptr_t controller = oot_bm_ptr(oot_bm_ptr(oot_bm_ptr(parts + 0x40) + 0x18) + 0x18), xlink = oot_bm_ptr(parts + 0x68);
    uint32_t count = 0; uint64_t command = 0;
    if (controller && oot_bm_read(controller + 0x18, &count, 4) && count > 0 && count < 64) command = oot_bm_ptr(oot_bm_ptr(oot_bm_ptr(controller + 0x20)) + 0x10);
    oot_ih_frame f; pthread_mutex_lock(&oot_ih_hands.mu); f = oot_ih_hands.f; pthread_mutex_unlock(&oot_ih_hands.mu);
    const int hands = f.at && oot_vr_now() - f.at < OOT_IH_HANDS_FRESH;
    const int carrying = (atomic_load(&oot_vr_action_now) & OOT_BOMB_CARRY) != 0;
    oot_bomb_cmd cmd;
    pthread_mutex_lock(&oot_bm.mu);
    int j = -1;
    for (int i = 0; i < OOT_BOMB_MAX && j < 0; i++) if (oot_bm.s.b[i].actor == actor) j = i;
    if (j < 0) {   /* first seen: a fruit by its name (one not found on its plant before) */
        uint64_t text = 0; char name[40] = "";
        if (oot_bm_read(actor + 0x260, &text, 8) && text >= 0x100000000ull && oot_vr_safe_read((uintptr_t)text, name, 24)) name[24] = 0;
        j = oot_bombs_seen(&oot_bm.s, actor, centre, oot_vr_now(), oot_bomb_fruit_name(name), 1);
        static _Atomic int said; if (atomic_fetch_add(&said, 1) < 8) fprintf(stderr, "[vr-bomb] a bomb's own update: %#llx %.24s at %.2f %.2f %.2f\n", (unsigned long long)actor, name, (double)centre[0], (double)centre[1], (double)centre[2]);
    } else (void)oot_bombs_seen(&oot_bm.s, actor, centre, oot_vr_now(), 0, 1);
    if (j >= 0) oot_bomb_own(&oot_bm.s, j, hands ? &f : NULL, rot, feet, command, carrying, oot_vr_now(), &oot_bm.opts, &cmd);
    pthread_mutex_unlock(&oot_bm.mu);
    if (j < 0) return NULL;
    if (cmd.event) oot_bm_say(cmd.event, -1, actor, 0);
    const uintptr_t base = (uintptr_t)oot_vr_base;
    int n = 0;
    if (cmd.place && physics) {
        memcpy(pos, cmd.origin, 12); pos[3] = 0; memcpy(rot_out, rot, 36);
        if (oot_ih_finite(pos, 3)) c[n++] = (oot_guest_call){ base + OOT_BOMB_PLACE, physics, (uint64_t)(uintptr_t)pos, (uint64_t)(uintptr_t)rot_out, 1, 0, NULL, 0 };
    }
    if (cmd.velocity && body && oot_ih_finite(cmd.v, 3)) { memcpy(v, cmd.v, 12); c[n++] = (oot_guest_call){ base + OOT_BOMB_VELOCITY, body, (uint64_t)(uintptr_t)v, 0, 0, 0, NULL, 0 }; }
    if (cmd.stop_spin && body) { memset(none, 0, sizeof none); c[n++] = (oot_guest_call){ base + OOT_BOMB_SPIN, body, (uint64_t)(uintptr_t)none, 0, 0, 0, NULL, 0 }; }
    if (cmd.layer >= 0 && body) c[n++] = (oot_guest_call){ base + OOT_BOMB_LAYER_FN, body, (uint64_t)cmd.layer, 0, 0, 0, NULL, 0 };
    if (cmd.frame && controller && oot_bm_thunk_frame) c[n++] = (oot_guest_call){ oot_bm_thunk_frame, controller, 0, 0, 0, 0, NULL, oot_bm_bits(OOT_BOMB_UNLIT_FRAME) };
    if (cmd.sleep && xlink) c[n++] = (oot_guest_call){ base + (cmd.sleep > 0 ? OOT_BOMB_SLEEP : OOT_BOMB_WAKE), xlink, OOT_BOMB_SLEEP_REASON, 0, 0, 0, NULL, 0 };
    for (int i = 0; i + 1 < n; i++) c[i].next = &c[i + 1];
    return n ? &c[0] : NULL;
}
/* Every actor's update (oot_vr.h oot_vr_player): a Bomb Flower's fruit near Link, waiting on its plant. */
static void oot_bombs_actor(uintptr_t a) {
    enum { NOT = 128 };
    static _Atomic uintptr_t not_actor[NOT]; static _Atomic uint64_t not_at[NOT];
    if (!atomic_load_explicit(&oot_bm_scan, memory_order_relaxed) || !a) return;
    const unsigned k = (unsigned)((a >> 4) * 2654435761u) % NOT;
    const uint64_t now = oot_vr_now();
    if (atomic_load_explicit(&not_actor[k], memory_order_relaxed) == a && now - atomic_load_explicit(&not_at[k], memory_order_relaxed) < 5000000000ull) return;
    uint64_t text = 0; char name[40] = "";
    const int named = oot_bm_read(a + 0x260, &text, 8) && text >= 0x100000000ull && oot_vr_safe_read((uintptr_t)text, name, 24);
    name[24] = 0;
    if (!named || !oot_bomb_fruit_name(name)) { atomic_store(&not_actor[k], a); atomic_store(&not_at[k], now); return; }
    float origin[3], rot[9], centre[3], feet[3];
    if (!oot_bm_where(a, origin, rot)) return;
    oot_bomb_centre(origin, rot, centre); oot_bm_feet_now(feet);
    pthread_mutex_lock(&oot_bm.mu);
    const float look = fmaxf(oot_bm.opts.reach, 1.5f) + 1.0f;
    int seen = 0;
    if (oot_ih_dist(centre, feet) <= look) { int known = 0; for (int i = 0; i < OOT_BOMB_MAX; i++) known |= oot_bm.s.b[i].actor == a; seen = !known; (void)oot_bombs_seen(&oot_bm.s, a, centre, now, 1, 0); }
    pthread_mutex_unlock(&oot_bm.mu);
    static _Atomic int lines;
    if (seen && atomic_fetch_add(&lines, 1) < 40) fprintf(stderr, "[vr-bomb] a Bomb Flower's fruit %s %#llx on its plant at %.2f %.2f %.2f\n", name, (unsigned long long)a, (double)centre[0], (double)centre[1], (double)centre[2]);
}
/* Link's hit-sensor update: the hands' step; its calls (the lift's let-go, the bracelet's counts). */
static oot_guest_call *oot_bombs_link_calc(const oot_ih_in *in, int selected, oot_ih_buttons *out) {
    static oot_guest_call fall, counts[4]; static const char *count_names[4]; static int counts_asked, bracelet; static uint64_t counts_at;
    memset(out, 0, sizeof *out);
    const uintptr_t base = (uintptr_t)oot_vr_base;
    if (counts_asked) { int have = 0; for (int i = 0; i < 4; i++) have |= (int32_t)counts[i].answer > 0; if (have != bracelet) fprintf(stderr, "[vr-bomb] the Goron's Bracelet (or a gauntlet): %s\n", have ? "owned" : "not owned"); bracelet = have; counts_asked = 0; }
    const int on = vr_oot_bomb_hands();   /* in third person too: the step lets go of what the hands held, and each bomb's layer goes back */
    atomic_store(&oot_bm_on, on);
    if (!on) { atomic_store(&oot_bm_scan, 0); return NULL; }
    float feet[3]; uint64_t age = 0;
    if (!runtime_oot_vr_feet(feet, &age) || age > 500000000ull) return NULL;
    for (int i = 0; i < 3; i++) atomic_store(&oot_bm_feet[i], oot_vr_float_bits(feet[i]));
    const oot_bomb_opts p = { vr_oot_bomb_throw_mode(), (float)vr_oot_bomb_strength() / 100.0f, 9.81f, vr_oot_bomb_pull(), (float)vr_oot_bomb_reach_dm() / 10.0f,
                              (float)vr_oot_bomb_cone() * 0.0174533f, vr_oot_bomb_flowers(), bracelet, oot_bm_thunk_frame != 0, 1 };
    atomic_store(&oot_bm_scan, p.flowers && bracelet && in->first_person);
    oot_bombs_out o;
    pthread_mutex_lock(&oot_bm.mu);
    if (!oot_bm.started) { oot_bm.started = 1; oot_bombs_init(&oot_bm.s); }
    oot_bm.opts = p;
    oot_bombs_link(&oot_bm.s, feet, in->now);
    oot_bombs_step(&oot_bm.s, in, &p, selected, feet, &o);
    const uintptr_t event_actor = o.event_bomb > 0 ? oot_bm.s.b[o.event_bomb - 1].actor : 0;
    pthread_mutex_unlock(&oot_bm.mu);
    oot_bm_say(o.event, o.event_hand, event_actor, o.event_speed);
    *out = o.b;
    oot_guest_call *head = NULL, **link = &head;
    const uintptr_t lifter = oot_bm_ptr(oot_bm_ptr(atomic_load_explicit(&oot_link_actor, memory_order_relaxed) + 0x270) + 0x2c8);
    if (o.fall && lifter && oot_bm_thunk_fall) {
        fall = (oot_guest_call){ oot_bm_thunk_fall, lifter, 1, 0, 0, 0, NULL, oot_bm_bits(1.0f) };
        *link = &fall; link = &fall.next;
        static _Atomic int lines; if (atomic_fetch_add(&lines, 1) < 40) fprintf(stderr, "[vr-bomb] %.3f Link's lift let go (the game's own call)\n", (double)in->now / 1e9);
    }
    uint64_t manager = 0;
    if (in->now - counts_at > 500000000ull && oot_vr_safe_read(base + OOT_POUCH_MANAGER_SLOT, &manager, 8) && manager) {
        counts_at = in->now;
        for (int i = 0; i < 4; i++) {
            count_names[i] = OOT_BRACELETS[i];
            counts[i] = (oot_guest_call){ base + OOT_POUCH_COUNT, manager, (uint64_t)(uintptr_t)&count_names[i], 0, 0, 0, NULL, 0 };
            *link = &counts[i]; link = &counts[i].next;
        }
        counts_asked = 1;
    }
    return head;
}
