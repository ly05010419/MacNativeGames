/* Holding the sword without a slash (vr.md row 46; HookshotVR's way, held-items.md 1.5 and its blade_hits.inc read for the facts the
   notes leave open: ideas and addresses only, GPL-3). In first person with the blade making its own hits, the draw gesture asks the
   game's own draw instead of pressing Y (Y draws with Link's own slash), and while the player's hand holds the sword Link is kept
   in his drawn stance and his own slashes are refused, so only the blade's own hits land.

   What our image says (update 131072, main.patched.bin):
   - The draw: main+0xa43030 is a function entry (pacibsp) that reads its x0's components (+0x270) and their EquipmentUser (+0x200)
     and, for w1 == 1, asks the game's own "is it drawn" (main+0x4a8a60) of slots 1 and 3: it is what the event node
     OneShotPlayerRequestEquipmentDrawn calls (HookshotVR), with x0 Link's actor, kind 1 (the sword and the shield), and 1. Its
     third argument is passed as HookshotVR passes it; on the kind-1 path we read, w2 is overwritten at main+0xa4308c before any use
     when Link has the component there, so what it does is not confirmed. It is called from Link's hit-sensor update (oot_link_calc),
     on the game thread, at most twice 300 ms apart, and given up after 1.5 s (HookshotVR's controls: two tries 300 ms apart).
   - The stance: Link's sub selector changes to its drawn child only on a request. Its request handler main+0x2b435b0 (an entry,
     pacibsp; the 45th slot of the class table at main+0x4b0c930, the only table holding it) reads the request's u32 at x1 +0: 0x39
     selects the child byte at selector +0x21, and when the current child (+0x1e) differs and is not 0xff, the handler itself calls
     main+0x74cf90(selector, child, 0) (main+0x2b436bc), after checking the selector's +0x18 bit 1 (else nothing) and bit 13 (set:
     no call). While the hand holds the sword we make that same call at Link's hit-sensor update, under the same two checks. The
     selector is the one its handler was last called with whose class table is main+0x4b0c930; that only Link has one of this class
     is HookshotVR's finding, not checked here.
   - His requests: main+0xc08f0 (an entry, pacibsp) takes x0 = Link's components +0x188, x1 = the request (0x60 bytes); our image
     reads the request's +0x48 and +0x4c as a pair (main+0xc0994, after x9 = x1 + 0x50) into its own copy, so a rewrite at the entry
     is what it uses. +0x48 is the action slot and +0x4c the verb (HookshotVR): while the hand holds the sword, slot 1 is made 0x39
     (drawn) and slot 0's slash 0x43 and its follow-through 0x70 are made his movement 0x28 (HookshotVR's headset, 22 Nov: drawn at
     every take, no slash of his, no lunge, no trail).
   - The drawn timer: the float at Link's player component (components +0x358, whose +0x18 is the actor) +0x1528, kept at 200 or more
     while the hand holds the sword (HookshotVR: his slash sets 200 and it counts down). Our image tests it as a positive float with
     +0x154c and +0x1534 (main+0xbbcf0); that it is this timer is HookshotVR's headset finding, not checked here.
   HookshotVR marks the stance as not fully solved (camera-ui.md section 9 item 7: its hits landed only in action 0x410, and "the call
   that properly puts Link in his drawn stance is being looked for"); this does what its notes support. "Holds" is the VR side's
   report (runtime_oot_vr_sword_held, fresh 200 ms), never while a put-away is asked. Included by oot_vr.h, after oot_vr_putaway.h. */
#pragma once

static const uint64_t OOT_DRAW_EQUIPMENT = 0xa43030, OOT_SUB_CHANGE = 0x74cf90, OOT_SUB_SELECTOR_CLASS = 0x4b0c930;
enum { OOT_AI_OF_COMPONENTS = 0x188, OOT_PLAYER_OF_COMPONENTS = 0x358, OOT_REQ_SLOT = 0x48, OOT_SEL_FLAGS = 0x18, OOT_SEL_NOW = 0x1e,
       OOT_SEL_DRAWN = 0x21, OOT_VERB_MOVE = 0x28, OOT_VERB_DRAWN = 0x39, OOT_VERB_SLASH = 0x43, OOT_VERB_FOLLOW = 0x70,
       OOT_DRAWN_TIMER_AT = 0x1528, OOT_DRAW_TRIES = 2 };
static const float OOT_DRAWN_TIMER = 200.0f;
static const uint64_t OOT_DRAW_GAP_NS = 300000000ull, OOT_DRAW_GIVE_UP_NS = 1500000000ull, OOT_HELD_FRESH_NS = 200000000ull;
static _Atomic uint64_t oot_stance_draw_asked, oot_stance_held_at;
static _Atomic uintptr_t oot_stance_selector, oot_stance_ai;   /* the ai: Link's components +0x188, from his update */
static uint64_t oot_stance_drew_at;   /* Link's update only */
static int oot_stance_draws;

void runtime_oot_vr_sword_draw(void) {
    uint64_t none = 0; const uint64_t now = oot_vr_now();
    if (atomic_compare_exchange_strong(&oot_stance_draw_asked, &none, now)) fprintf(stderr, "[vr-draw] %.3f asked: the game's own draw (no slash)\n", (double)now / 1e9);
}
void runtime_oot_vr_sword_held(int held) { atomic_store_explicit(&oot_stance_held_at, held ? oot_vr_now() : 0, memory_order_relaxed); }
static int oot_stance_want(uint64_t now) {
    const uint64_t at = atomic_load_explicit(&oot_stance_held_at, memory_order_relaxed);
    return at && now >= at && now - at < OOT_HELD_FRESH_NS && !atomic_load_explicit(&oot_putaway_asked, memory_order_relaxed);
}
/* Link's components, 0 when his actor is not known. */
static uintptr_t oot_stance_parts(uintptr_t *actor) {
    uint64_t parts = 0; *actor = atomic_load_explicit(&oot_link_actor, memory_order_relaxed);
    return *actor && oot_scan_read(*actor + 0x270, &parts, 8) ? (uintptr_t)parts : 0;
}

/* Link's hit sensor update (oot_link_calc): the draw while asked, and the stance kept while the hand holds the sword. */
static oot_guest_call *oot_stance_calc(void) {
    static oot_guest_call draw, change;
    uintptr_t actor = 0; const uintptr_t parts = oot_stance_parts(&actor);
    if (!parts || !oot_vr_base) return NULL;
    { uint64_t ai = 0; atomic_store_explicit(&oot_stance_ai, oot_scan_read(parts + OOT_AI_OF_COMPONENTS, &ai, 8) ? (uintptr_t)ai : 0, memory_order_relaxed); }
    const uint64_t now = oot_vr_now(), asked = atomic_load(&oot_stance_draw_asked);
    oot_guest_call *first = NULL, **tail = &first;
    if (asked) {
        uint8_t state = 0; const uintptr_t sword = oot_vr_equip_actor(parts, 0, &state);
        const int drawn = sword && (oot_equip_drawn(sword) || (state & OOT_EQUIP_TO_HAND));
        if (drawn || now - asked > OOT_DRAW_GIVE_UP_NS || (oot_stance_draws >= OOT_DRAW_TRIES && now - oot_stance_drew_at > OOT_DRAW_GAP_NS)) {
            fprintf(stderr, "[vr-draw] %.3f the sword %s %.0f ms after the ask (%d call%s; Link's action state %#x)\n", (double)now / 1e9, drawn ? "is drawn" : "was NOT drawn",
                    (double)(now - asked) / 1e6, oot_stance_draws, oot_stance_draws == 1 ? "" : "s", atomic_load(&oot_vr_action_now));
            oot_stance_draws = 0; atomic_store(&oot_stance_draw_asked, 0);
        } else if (oot_stance_draws < OOT_DRAW_TRIES && (!oot_stance_draws || now - oot_stance_drew_at >= OOT_DRAW_GAP_NS)) {
            oot_stance_draws++; oot_stance_drew_at = now;
            draw = (oot_guest_call){ (uint64_t)(uintptr_t)(oot_vr_base + OOT_DRAW_EQUIPMENT), (uint64_t)actor, 1, 1, 0, 0, NULL, 0 };
            *tail = &draw; tail = &draw.next;
            fprintf(stderr, "[vr-draw] %.3f the game's own draw: main+%#llx(Link, 1, 1), call %d (Link's action state %#x)\n", (double)now / 1e9,
                    (unsigned long long)OOT_DRAW_EQUIPMENT, oot_stance_draws, atomic_load(&oot_vr_action_now));
        }
    }
    static int holding_said;
    if (!oot_stance_want(now)) { if (holding_said) { holding_said = 0; fprintf(stderr, "[vr-stance] %.3f the hand let the sword go: Link's own stance again\n", (double)now / 1e9); } return first; }
    {   /* the drawn timer */
        uint64_t player = 0, owner = 0; float timer = 0;
        if (oot_scan_read(parts + OOT_PLAYER_OF_COMPONENTS, &player, 8) && player && oot_scan_read((uintptr_t)player + 0x18, &owner, 8) && owner == actor &&
            oot_scan_read((uintptr_t)player + OOT_DRAWN_TIMER_AT, &timer, 4) && isfinite(timer) && timer < OOT_DRAWN_TIMER) {
            memcpy((void *)(uintptr_t)(player + OOT_DRAWN_TIMER_AT), &OOT_DRAWN_TIMER, 4);
            if (!holding_said) fprintf(stderr, "[vr-stance] %.3f the hand holds the sword: the drawn timer kept at %.0f (it was %.1f)\n", (double)now / 1e9, (double)OOT_DRAWN_TIMER, (double)timer);
            holding_said = 1;
        }
    }
    const uintptr_t sel = atomic_load_explicit(&oot_stance_selector, memory_order_relaxed);
    uint64_t cls = 0; uint32_t flags = 0; uint8_t now_child = 0, drawn_child = 0;
    if (sel && oot_scan_read(sel, &cls, 8) && cls == (uint64_t)(uintptr_t)(oot_vr_base + OOT_SUB_SELECTOR_CLASS) && oot_scan_read(sel + OOT_SEL_FLAGS, &flags, 4) &&
        oot_scan_read(sel + OOT_SEL_NOW, &now_child, 1) && oot_scan_read(sel + OOT_SEL_DRAWN, &drawn_child, 1) &&
        (flags & 2) && !(flags & 0x2000) && drawn_child != 0xff && now_child != drawn_child) {
        change = (oot_guest_call){ (uint64_t)(uintptr_t)(oot_vr_base + OOT_SUB_CHANGE), (uint64_t)sel, drawn_child, 0, 0, 0, NULL, 0 };
        *tail = &change; tail = &change.next;
        static int lines;
        if (lines++ < 40) fprintf(stderr, "[vr-stance] %.3f Link's sub selector %#llx: child %u -> drawn %u (the game's own change, main+%#llx; action state %#x)\n", (double)now / 1e9,
                                  (unsigned long long)sel, now_child, drawn_child, (unsigned long long)OOT_SUB_CHANGE, atomic_load(&oot_vr_action_now));
    }
    return first;
}
/* main+0xc08f0's entry: Link's own requests, made drawn and without his slash while the hand holds the sword. */
static oot_guest_call *oot_stance_request(oot_hook_regs *r) {
    const uintptr_t ai = atomic_load_explicit(&oot_stance_ai, memory_order_relaxed);
    if (!ai || r->x[0] != ai || !r->x[1] || !oot_stance_want(oot_vr_now())) return NULL;
    uint32_t sv[2]; memcpy(sv, (const void *)(uintptr_t)(r->x[1] + OOT_REQ_SLOT), 8);   /* the function reads these right after */
    const uint32_t slot = sv[0], verb = sv[1];
    const uint32_t want = slot == 1 && verb != OOT_VERB_DRAWN ? (uint32_t)OOT_VERB_DRAWN : slot == 0 && (verb == OOT_VERB_SLASH || verb == OOT_VERB_FOLLOW) ? (uint32_t)OOT_VERB_MOVE : verb;
    if (want == verb) return NULL;
    memcpy((void *)(uintptr_t)(r->x[1] + OOT_REQ_SLOT + 4), &want, 4);
    static _Atomic int lines;
    if (atomic_fetch_add(&lines, 1) < 40 || (slot == 0 && atomic_load(&lines) < 400))
        fprintf(stderr, "[vr-stance] %.3f the game's request to Link: slot %u verb %#x made %#x%s (action state %#x)\n", (double)oot_vr_now() / 1e9, slot, verb, want,
                slot == 0 ? " (his own slash refused)" : "", atomic_load(&oot_vr_action_now));
    return NULL;
}
/* main+0x2b435b0's entry (x0 the selector, x1 the request): which selector is Link's. */
static oot_guest_call *oot_stance_selector_seen(oot_hook_regs *r) {
    const uintptr_t sel = (uintptr_t)r->x[0];
    uint64_t cls = 0;
    if (!sel || !oot_vr_base || !oot_scan_read(sel, &cls, 8) || cls != (uint64_t)(uintptr_t)(oot_vr_base + OOT_SUB_SELECTOR_CLASS)) return NULL;
    if (sel != atomic_exchange(&oot_stance_selector, sel)) {
        uint8_t now_child = 0, drawn_child = 0; oot_scan_read(sel + OOT_SEL_NOW, &now_child, 1); oot_scan_read(sel + OOT_SEL_DRAWN, &drawn_child, 1);
        fprintf(stderr, "[vr-stance] Link's sub selector %#llx (drawn child %u, now %u)\n", (unsigned long long)sel, drawn_child, now_child);
    }
    if (oot_blade_log_on() && r->x[1]) {   /* the requests reaching it, while they change */
        static uint32_t last = ~0u; static int lines; uint32_t v = 0;
        if (oot_scan_read((uintptr_t)r->x[1], &v, 4) && v != last && lines < 150) { lines++; last = v; fprintf(stderr, "[vr-stance] %.3f sub request %#x to Link's sub selector (action state %#x)\n", (double)oot_vr_now() / 1e9, v, atomic_load(&oot_vr_action_now)); }
    }
    return NULL;
}
