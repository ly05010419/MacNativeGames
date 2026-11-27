/* The bow and the slingshot, as the game has them (2026-11-27, vr.md rows 54 and 55; HookshotVR's notes, aim-items.md 0.3, 0.4
   and 1.6, GPL-3: facts and addresses, no code). Read for the VR side's grip, which takes the bow out with the game's ZR and puts
   it away with the game's sheathe-all (oot_vr_putaway.h):
   - Out: Link's equipment slot 3 (the ranged weapon; EquipmentUser = components +0x200, slot handles +0x20 + 0x18 x slot, as
     oot_vr_sword.h reads slots 0 and 1) holds an actor named Weapon_Range_Bow... or Weapon_Range_SlingShot, and the game calls it
     drawn (its Equipment component, oot_equip_drawn). Slot 3 is the one the third-person aim asks through (oot_vr_aim.h), whose
     answers came back in our runs (oot-vr2, "[vr-aim] the game's own answer"), so on our image it holds the slingshot.
   - On the item button: main+0x17b6e0(pouch, 1 child / 0 adult, 0) gives the item's number, the call oot_vr_wheel_host.h makes to
     check a choice (run wheel2: it answered the item chosen, 4 and 0). The bow is 30 (also 46, 47, 48: the fire, ice and light
     arrows are the bow with that arrow), the slingshot 4 (HookshotVR's AssignableItemEnum, the same numbers as our wheel's). Asked
     every 100 ms from Link's hit-sensor update, on the game's thread; the answer is read on the next update.
   The hand items (oot_vr_hand_items.h, rows 63-67) read the same answer (merge 2026-11-27: one question for both).
   TOTK_VR_BLADELOG=1 logs every answer; a change is logged always (one line). Included by oot_vr.h, after oot_vr_wheel_host.h. */
#pragma once

enum { OOT_BOW_SLOT = 3, OOT_ITEM_SLINGSHOT = 4, OOT_ITEM_BOW = 30, OOT_ITEM_FIRE_ARROW = 46, OOT_ITEM_LIGHT_ARROW = 48 };
static const uint64_t OOT_ITEM_SELECTION_SLOT = 0x4e7cfa8;   /* the holder of the item button's selection byte (+0x5c0) */
static const uint64_t OOT_BOW_FRESH_NS = 150000000ull, OOT_BOW_ASK_NS = 100000000ull, OOT_BOW_ANSWER_NS = 1000000000ull;
static _Atomic int oot_bow_kind;              /* 0 none, 1 the bow, 2 the slingshot: drawn in Link's hand */
static _Atomic uint64_t oot_bow_seen_at;      /* Link's update that read it */
static _Atomic int oot_bow_item = -1;         /* the item on the button, -1 not known */
static _Atomic uint64_t oot_bow_item_at;
/* Link's update only: */
static oot_guest_call oot_bow_ask_call;
static int oot_bow_asked;
static uint64_t oot_bow_asked_at;

/* 1 the bow, 2 the slingshot, 0 neither is out (or Link's update has not run these 150 ms). */
int runtime_oot_vr_bow_out(void) {
    const uint64_t at = atomic_load(&oot_bow_seen_at);
    return at && oot_vr_now() - at < OOT_BOW_FRESH_NS ? atomic_load(&oot_bow_kind) : 0;
}
/* 1 the bow (or an arrow kind), 2 the slingshot, 0 something else, -1 not known: what the item button holds. */
int runtime_oot_vr_bow_selected(void) {
    const uint64_t at = atomic_load(&oot_bow_item_at);
    if (!at || oot_vr_now() - at > OOT_BOW_ANSWER_NS) return -1;
    const int item = atomic_load(&oot_bow_item);
    return item == OOT_ITEM_SLINGSHOT ? 2 : item == OOT_ITEM_BOW || (item >= OOT_ITEM_FIRE_ARROW && item <= OOT_ITEM_LIGHT_ARROW) ? 1 : 0;
}
/* The kind of arrow on the button: 0 normal, 1 fire, 2 ice, 3 light (for the arrow's effects; -1 not the bow). */
int runtime_oot_vr_arrow_kind(void) {
    const int item = atomic_load(&oot_bow_item);
    return item == OOT_ITEM_BOW ? 0 : item >= OOT_ITEM_FIRE_ARROW && item <= OOT_ITEM_LIGHT_ARROW ? item - OOT_ITEM_FIRE_ARROW + 1 : -1;
}

/* From Link's update: is the bow or the slingshot out. */
static void oot_bow_note(uintptr_t components) {
    uint8_t state = 0;
    const uintptr_t actor = oot_vr_equip_actor(components, OOT_BOW_SLOT, &state);
    const int kind = !actor || !oot_equip_drawn(actor) ? 0 : oot_actor_named(actor, "Weapon_Range_SlingShot") ? 2 : oot_actor_named(actor, "Weapon_Range_Bow") ? 1 : 0;
    const int was = atomic_exchange(&oot_bow_kind, kind);
    atomic_store(&oot_bow_seen_at, oot_vr_now());
    if (kind != was) {
        static int lines;
        if (lines++ < 200) fprintf(stderr, "[vr-bow] %.3f %s (slot %d actor %#llx, Link's action state %#x)\n", (double)oot_vr_now() / 1e9,
                                   kind == 2 ? "the slingshot is out" : kind == 1 ? "the bow is out" : was == 2 ? "the slingshot is put away" : "the bow is put away",
                                   OOT_BOW_SLOT, (unsigned long long)actor, atomic_load(&oot_vr_action_now));
    }
}

/* Link's hit-sensor update (oot_link_calc): the last answer, then the next question. */
static oot_guest_call *oot_bowstate_calc(void) {
    const uintptr_t base = (uintptr_t)oot_vr_base;
    const uint64_t now = oot_vr_now();
    if (oot_bow_asked) {
        oot_bow_asked = 0;
        const int item = (int)(int32_t)oot_bow_ask_call.answer;
        if (item >= -1 && item <= 0x35) {
            const int was = atomic_exchange(&oot_bow_item, item);
            atomic_store(&oot_bow_item_at, now);
            static int lines;
            if ((item != was || oot_blade_log_on()) && lines++ < 200)
                fprintf(stderr, "[vr-bow] %.3f the item button holds item %d (%s)\n", (double)now / 1e9, item,
                        item == OOT_ITEM_SLINGSHOT ? "the slingshot" : item == OOT_ITEM_BOW ? "the bow" : item >= OOT_ITEM_FIRE_ARROW && item <= OOT_ITEM_LIGHT_ARROW ? "the bow with magic arrows" : "not the bow");
        } else { static int said; if (!said++) fprintf(stderr, "[vr-bow] the item button's answer %d is not an item number: not used\n", item); }
    }
    if (!base || now - oot_bow_asked_at < OOT_BOW_ASK_NS) return NULL;
    uint64_t slot = 0, pouch = 0, holder = 0, flags = 0; int child = runtime_oot_vr_child(); uint8_t selection = 0;
    if (!oot_vr_safe_read(base + OOT_POUCH_SLOT, &slot, 8) || !slot || !oot_vr_safe_read((uintptr_t)slot, &pouch, 8) || !pouch) return NULL;
    /* The second argument as the game's own QueryGetSelectedAssignableItem passes it (main+0x2a1d940, read on our image 2026-11-27:
       ldrb w1, [*(main+0x4e7cfa8), #0x5c0] at main+0x2a1d974, then bl main+0x17b6e0 with w2 = 0); the hand items' author's form
       (oot_vr_hand_items.h). Without that byte, Link's age as before (the wheel's form, run wheel2). */
    uint64_t sel_holder = 0;
    const int by_game = oot_vr_safe_read(base + OOT_ITEM_SELECTION_SLOT, &sel_holder, 8) && sel_holder && oot_vr_safe_read((uintptr_t)sel_holder + 0x5c0, &selection, 1);
    if (!by_game && oot_vr_safe_read(base + OOT_VR_GAMEDATA_SLOT, &holder, 8) && holder && oot_vr_safe_read((uintptr_t)holder, &flags, 8) && flags)
        (void)oot_gd_bool(oot_vr_safe_read, (uintptr_t)flags, OOT_GD_CHILD, &child);
    oot_bow_asked_at = now; oot_bow_asked = 1;
    oot_bow_ask_call = (oot_guest_call){ (uint64_t)(base + OOT_ITEM_ON_BUTTON), pouch, by_game ? selection : child ? 1u : 0u, 0, 0, 0, NULL, 0 };
    return &oot_bow_ask_call;
}
