/* Putting the sword (and whatever else is out) away by the game's own call, not by a button (vr.md row 49; HookshotVR's way,
   ideas and addresses only, GPL-3). The call is the one the game's own event node "OneShotPlayerRequestAllEquipmentSheathed" runs:
   main+0x4a8690(Link's actor). On our image it is a function entry (pacibsp) that reads the actor's components (+0x270) and their
   EquipmentUser (+0x200), and sheathes each drawn slot without the put-away animation; the shield goes with the sword, as with B.
   It must run in Link's update on the game thread: it goes into the chain of calls made from his hit sensor's update (oot_vr.h
   oot_link_calc). The VR side asks (runtime_oot_vr_put_away) when the hand lets the sword go, or the bow or slingshot (vr.md row 54,
   oot_vr_bowstate.h says whether it is out); the ask is made again every 300 ms
   while the weapon is still out, at most four times, then given up with a log line. Included by oot_vr.h, after oot_vr_sword.h and oot_vr_bowstate.h. */
#pragma once

static const uint64_t OOT_SHEATHE_ALL = 0x4a8690;
enum { OOT_PUTAWAY_TRIES = 4 };
static const uint64_t OOT_PUTAWAY_GAP_NS = 300000000ull;
static _Atomic uint64_t oot_putaway_asked;   /* when the VR side asked; 0: nothing asked */
static int oot_putaway_tries;                /* Link's update only */
static uint64_t oot_putaway_last;

void runtime_oot_vr_put_away(void) {   /* the gesture lasts a few frames: one ask until it is done or given up */
    uint64_t none = 0; const uint64_t now = oot_vr_now();
    if (atomic_compare_exchange_strong(&oot_putaway_asked, &none, now)) fprintf(stderr, "[vr-putaway] %.3f asked: the game's own sheathe-all\n", (double)now / 1e9);
}
/* The VR side: is the weapon out in the game's own hand (the game last sent it there). */
int runtime_oot_vr_weapon_out(void) {
    pthread_mutex_lock(&oot_weapon.mu);
    const int out = oot_weapon.actor && oot_weapon.in_hand;
    pthread_mutex_unlock(&oot_weapon.mu);
    return out;
}

/* The grip took the bow again (zeldaVR/runtime/oot_vr_bowgrip.h): an ask whose call has not been made yet is withdrawn. */
void runtime_oot_vr_put_away_cancel(void) {
    const uint64_t asked = atomic_load(&oot_putaway_asked);
    if (asked && !oot_putaway_tries && atomic_compare_exchange_strong(&oot_putaway_asked, &(uint64_t){asked}, 0))
        fprintf(stderr, "[vr-putaway] %.3f withdrawn before the call was made\n", (double)oot_vr_now() / 1e9);
}
/* Link's hit sensor update (oot_link_calc): the call, while asked and the weapon, the bow or the slingshot is still out. */
static oot_guest_call *oot_putaway_calc(void) {
    static oot_guest_call call;
    const uint64_t asked = atomic_load(&oot_putaway_asked);
    if (!asked) return NULL;
    const uint64_t now = oot_vr_now();
    if (!runtime_oot_vr_weapon_out() && !runtime_oot_vr_bow_out()) {
        if (oot_putaway_tries) fprintf(stderr, "[vr-putaway] %.3f put away after %d call%s (%.0f ms after the ask)\n", (double)now / 1e9, oot_putaway_tries,
                                       oot_putaway_tries == 1 ? "" : "s", (double)(now - asked) / 1e6);
        oot_putaway_tries = 0; atomic_store(&oot_putaway_asked, 0);
        return NULL;
    }
    if (oot_putaway_tries >= OOT_PUTAWAY_TRIES) {
        fprintf(stderr, "[vr-putaway] %.3f put away FAILED: the weapon or the bow is still out after %d calls\n", (double)now / 1e9, oot_putaway_tries);
        oot_putaway_tries = 0; atomic_store(&oot_putaway_asked, 0);
        return NULL;
    }
    if (oot_putaway_tries && now - oot_putaway_last < OOT_PUTAWAY_GAP_NS) return NULL;
    const uintptr_t actor = atomic_load_explicit(&oot_link_actor, memory_order_relaxed);
    if (!actor || !oot_vr_base) return NULL;
    oot_putaway_tries++; oot_putaway_last = now;
    call = (oot_guest_call){ (uint64_t)(uintptr_t)(oot_vr_base + OOT_SHEATHE_ALL), (uint64_t)actor, 0, 0, 0, 0, NULL, 0 };
    return &call;
}
