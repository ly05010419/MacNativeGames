/* zeldaVR for Ocarina of Time: the game's side of the ocarina in the player's hands (vr.md rows 69, 70; the hands are
   zeldaVR/runtime/oot_vr_ocarina.h). Included by oot_vr.h after oot_vr_wheel_host.h (it uses the pouch's count call).
     - whether the game waits for notes: its note-wait action's update, main+0x2889280 (pacibsp; x0 the action: main+0x2889298
       `ldrb w8, [x0, #0x18]` / `tbz w8, #1, ...` returns at once unless bit 1 says the action runs; the kind of wait is the u32 at
       +0x20, stored by the action's start main+0x28891b4 `stp w8, w9, [x0, #0x20]` and compared with 0xd and 3 at main+0x2889324 /
       0x28893c4). HookshotVR's notes (held-items.md 7.1): kinds 1 and 3 a field ocarina, 2 a song lesson, 7 the Skull Kids, 10 / 11
       the scarecrow, 13 a dialogue's. A wait not seen for 500 ms is none;
     - whether Link has an ocarina: the pouch's count of Item_Ocarina_Time and Item_Ocarina_Fairy (main+0x5963e0, as the item wheel
       asks it, oot_vr_wheel_host.h; the names are HookshotVR's equipment table's), every 2 s while the option is on. */
#pragma once
static _Atomic uint64_t oot_ocarina_wait_at;
static _Atomic int oot_ocarina_wait_kind, oot_ocarina_owned;
static oot_guest_call *oot_ocarina_tick(oot_hook_regs *r) {
    const uintptr_t action = (uintptr_t)r->x[0];
    uint8_t flags = 0; uint32_t kind = 0;
    if (!action || !oot_vr_safe_read(action + 0x18, &flags, 1) || !oot_vr_safe_read(action + 0x20, &kind, 4) || !(flags & 2) || kind > 63) return NULL;   /* kinds above 13 go the general way at main+0x2889328 */
    const uint64_t now = oot_vr_now(), was = atomic_exchange(&oot_ocarina_wait_at, now);
    const int had = atomic_exchange(&oot_ocarina_wait_kind, (int)kind);
    if (!was || now - was > 500000000ull || had != (int)kind) { static int lines; if (lines < 100) { lines++; fprintf(stderr, "[vr-ocarina] %.2f s: the game waits for notes (kind %u)\n", (double)now / 1e9, kind); } }
    return NULL;
}
int runtime_oot_vr_ocarina(int *owned, int *waiting, int *kind) {
    const uint64_t at = atomic_load(&oot_ocarina_wait_at);
    *waiting = at && oot_vr_now() - at < 500000000ull;
    *kind = atomic_load(&oot_ocarina_wait_kind);
    *owned = atomic_load(&oot_ocarina_owned);
    return 1;
}
/* Link's hit-sensor update: last time's counts, then this time's (every 2 s). */
static oot_guest_call oot_ocarina_calls[2];
static const char *oot_ocarina_names[2] = { "Item_Ocarina_Time", "Item_Ocarina_Fairy" };
static oot_guest_call *oot_ocarina_calc(void) {
    static int asked; static uint64_t at;
    if (asked) {
        const int owned = (int32_t)oot_ocarina_calls[0].answer > 0 ? 2 : (int32_t)oot_ocarina_calls[1].answer > 0 ? 1 : 0;
        static int answered;
        if (atomic_exchange(&oot_ocarina_owned, owned) != owned || !answered++) fprintf(stderr, "[vr-ocarina] Link has %s\n", owned == 2 ? "the Ocarina of Time" : owned ? "the Fairy Ocarina" : "no ocarina");
        asked = 0;
    }
    const uint64_t now = oot_vr_now();
    if (!atomic_load(&oot_scene_opt[6]) || now - at < 2000000000ull) return NULL;
    at = now;
    uint64_t manager = 0;
    if (!oot_vr_safe_read((uintptr_t)oot_vr_base + OOT_POUCH_MANAGER_SLOT, &manager, 8) || !manager) return NULL;
    for (int i = 0; i < 2; i++) oot_ocarina_calls[i] = (oot_guest_call){ (uint64_t)((uintptr_t)oot_vr_base + OOT_POUCH_COUNT), manager, (uint64_t)(uintptr_t)&oot_ocarina_names[i], 0, 0, 0, i ? NULL : &oot_ocarina_calls[1], 0 };
    asked = 1;
    return &oot_ocarina_calls[0];
}
