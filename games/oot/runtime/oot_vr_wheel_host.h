/* The item wheel's game side (2026-11-25; zeldaVR/runtime/oot_vr_wheel.h has the list and the stick). Two of the game's own
   functions, called from Link's own update through the shared hook body (oot_vr_hooks.h), never from another thread:
     main+0x5963e0 (pouch manager, a pointer to a pointer to the name, 0, 0) -> how many of that the pouch holds, the count the
       game's own "how many by actor name" query uses; the manager is *(main+0x4e78e28);
     main+0x1211c00 (pouch, 1 for a child / 0, the item's number, 0, 1) puts an item on the item button, as the game's own quick
       select does; the pouch is *(*(main+0x4c7e0d0));
     main+0x17b6e0 (pouch, 1 for a child / 0, 0) -> the number of the item on the item button, asked three updates after a
       choice so that the log says what the game made of it (asked in the same update it still answers the item before: run
       wheel1, 2026-11-25).
   The bottles and Link's age are saved flags, read from the game's tables (oot_gamedata.h). Addresses and arguments from
   HookshotVR's notes (host/hos_main.c, vr/menu/radial_host.c, vr/input/quick_select_host.c, GPL-3: facts only, no code), the
   three entries and how they take their arguments read in our image (2026-11-25). The VR side asks for a fresh list while the wheel is open and names the item chosen. Included by oot_vr.h. */
#pragma once
#include "../zeldaVR/runtime/oot_vr_wheel.h"

static const uint64_t OOT_POUCH_MANAGER_SLOT = 0x4e78e28, OOT_POUCH_SLOT = 0x4c7e0d0, OOT_POUCH_COUNT = 0x5963e0, OOT_ITEM_SELECT = 0x1211c00,
                      OOT_ITEM_ON_BUTTON = 0x17b6e0;
static struct { pthread_mutex_t mu; uint8_t owned[OOT_WHEEL_ITEM_COUNT]; int child; uint64_t at; } oot_wheel = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic int oot_wheel_refresh, oot_wheel_pending = -1;
/* Link's update only: */
static oot_guest_call oot_wheel_calls[OOT_WHEEL_ITEM_COUNT], oot_wheel_select_call, oot_wheel_check_call;
static const char *oot_wheel_names[OOT_WHEEL_ITEM_COUNT];
static int oot_wheel_asked, oot_wheel_checking = -1, oot_wheel_check_wait, oot_wheel_check_asked;

void runtime_oot_vr_wheel_refresh(void) { atomic_store(&oot_wheel_refresh, 1); }
void runtime_oot_vr_wheel_select(int item) { if (item >= 0) atomic_store(&oot_wheel_pending, item); }
/* What Link owns, row by row of OOT_WHEEL_ITEMS, and his age; 0 before the first answer. age_ns: how old the answer is. */
int runtime_oot_vr_wheel(uint8_t owned[OOT_WHEEL_ITEM_COUNT], int *child, uint64_t *age_ns) {
    pthread_mutex_lock(&oot_wheel.mu);
    const uint64_t at = oot_wheel.at;
    memcpy(owned, oot_wheel.owned, sizeof oot_wheel.owned); *child = oot_wheel.child;
    pthread_mutex_unlock(&oot_wheel.mu);
    if (!at) return 0;
    *age_ns = oot_vr_now() - at;
    return 1;
}

/* From the hook at the entry of Link's hit sensor's update: last update's answers, then this update's calls (NULL: none). */
static oot_guest_call *oot_wheel_calc(void) {
    const uintptr_t base = (uintptr_t)oot_vr_base;
    if (oot_wheel_check_asked) {   /* the choice of a few updates ago: what the game says is on the button now */
        const int now_on = (int)(int32_t)oot_wheel_check_call.answer;
        fprintf(stderr, "[vr-wheel] %.3f the item button now holds item %d (%s the one chosen, %d)\n", (double)oot_vr_now() / 1e9, now_on,
                now_on == oot_wheel_checking ? "it is" : "NOT", oot_wheel_checking);
        oot_wheel_check_asked = 0; oot_wheel_checking = -1;
    }
    if (oot_wheel_asked) {
        uint8_t owned[OOT_WHEEL_ITEM_COUNT]; int child = 1, lines = 0; char text[400] = "";
        uint64_t holder = 0, flags = 0;
        if (!oot_vr_safe_read(base + OOT_VR_GAMEDATA_SLOT, &holder, 8) || !holder || !oot_vr_safe_read((uintptr_t)holder, &flags, 8)) flags = 0;
        for (int i = 0; i < OOT_WHEEL_ITEM_COUNT; i++) {
            int have = 0;
            if (OOT_WHEEL_ITEMS[i].owned) have = (int32_t)oot_wheel_calls[i].answer > 0;
            else if (flags) (void)oot_gd_bool(oot_vr_safe_read, (uintptr_t)flags, OOT_WHEEL_ITEMS[i].flag, &have);
            owned[i] = have != 0;
            if (have && lines < 380) lines += snprintf(text + lines, sizeof text - (size_t)lines, " %s", OOT_WHEEL_ITEMS[i].en);
        }
        if (flags) (void)oot_gd_bool(oot_vr_safe_read, (uintptr_t)flags, OOT_GD_CHILD, &child);
        pthread_mutex_lock(&oot_wheel.mu);
        const int changed = memcmp(owned, oot_wheel.owned, sizeof owned) || child != oot_wheel.child || !oot_wheel.at;
        memcpy(oot_wheel.owned, owned, sizeof owned); oot_wheel.child = child; oot_wheel.at = oot_vr_now();
        pthread_mutex_unlock(&oot_wheel.mu);
        static int said;
        if (changed && said < 20) { said++; fprintf(stderr, "[vr-wheel] Link (%s) owns:%s\n", child ? "a child" : "an adult", text[0] ? text : " nothing for the item button"); }
        oot_wheel_asked = 0;
    }
    oot_guest_call *head = NULL, **link = &head;
    const int item = atomic_exchange(&oot_wheel_pending, -1);
    if (item >= 0) {
        uint64_t slot = 0, pouch = 0; int child;
        pthread_mutex_lock(&oot_wheel.mu); child = oot_wheel.child; pthread_mutex_unlock(&oot_wheel.mu);
        if (oot_vr_safe_read(base + OOT_POUCH_SLOT, &slot, 8) && slot && oot_vr_safe_read((uintptr_t)slot, &pouch, 8) && pouch) {
            oot_wheel_select_call = (oot_guest_call){ (uint64_t)(base + OOT_ITEM_SELECT), pouch, child ? 1u : 0u, (uint64_t)item, 0, 0, NULL, 1 };
            *link = &oot_wheel_select_call; link = &oot_wheel_select_call.next;
            oot_wheel_checking = item; oot_wheel_check_wait = 3; oot_wheel_check_asked = 0;
            fprintf(stderr, "[vr-wheel] %.3f item %d goes on the item button (%s)\n", (double)oot_vr_now() / 1e9, item, child ? "child" : "adult");
        } else fprintf(stderr, "[vr-wheel] item %d not chosen: the pouch is not there\n", item);
    }
    else if (oot_wheel_checking >= 0 && !oot_wheel_check_asked && --oot_wheel_check_wait <= 0) {
        uint64_t slot = 0, pouch = 0; int child;
        pthread_mutex_lock(&oot_wheel.mu); child = oot_wheel.child; pthread_mutex_unlock(&oot_wheel.mu);
        if (oot_vr_safe_read(base + OOT_POUCH_SLOT, &slot, 8) && slot && oot_vr_safe_read((uintptr_t)slot, &pouch, 8) && pouch) {
            oot_wheel_check_call = (oot_guest_call){ (uint64_t)(base + OOT_ITEM_ON_BUTTON), pouch, child ? 1u : 0u, 0, 0, 0, NULL, 0 };
            *link = &oot_wheel_check_call; link = &oot_wheel_check_call.next; oot_wheel_check_asked = 1;
        } else oot_wheel_checking = -1;
    }
    if (atomic_exchange(&oot_wheel_refresh, 0)) {
        uint64_t manager = 0;
        if (oot_vr_safe_read(base + OOT_POUCH_MANAGER_SLOT, &manager, 8) && manager) {
            for (int i = 0; i < OOT_WHEEL_ITEM_COUNT; i++) {
                if (!OOT_WHEEL_ITEMS[i].owned) continue;
                oot_wheel_names[i] = OOT_WHEEL_ITEMS[i].owned;
                oot_wheel_calls[i] = (oot_guest_call){ (uint64_t)(base + OOT_POUCH_COUNT), manager, (uint64_t)(uintptr_t)&oot_wheel_names[i], 0, 0, 0, NULL, 0 };
                *link = &oot_wheel_calls[i]; link = &oot_wheel_calls[i].next;
            }
            oot_wheel_asked = 1;
        } else { static int said; if (!said) { said = 1; fprintf(stderr, "[vr-wheel] the game's pouch manager is not there: the wheel has no list\n"); } }
    }
    return head;
}
