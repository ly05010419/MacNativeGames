/* The item wheel: what is on it, and the stick over it (zeldaVR/runtime/oot_vr_wheel.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_wheel.h"

static int row_of(const char *owned) { for (int i = 0; i < OOT_WHEEL_ITEM_COUNT; i++) if (OOT_WHEEL_ITEMS[i].owned && !strcmp(OOT_WHEEL_ITEMS[i].owned, owned)) return i; return -1; }
static int has(const int rows[], int n, int item) { for (int k = 0; k < n; k++) if (OOT_WHEEL_ITEMS[rows[k]].item == item) return 1; return 0; }

int main(void) {
    uint8_t owned[OOT_WHEEL_ITEM_COUNT]; int rows[OOT_WHEEL_SLOTS];
    memset(owned, 0, sizeof owned);
    assert(oot_wheel_list(owned, 1, rows) == 0);

    /* a child with sticks, nuts, the slingshot, two bomb bags' worth of names, a bottle, and (never his) a bow */
    owned[row_of("Item_DekuStick")] = owned[row_of("Item_DekuNut")] = owned[row_of("Weapon_Range_SlingShot")] = 1;
    owned[row_of("Item_BombBag_Small")] = owned[row_of("Item_BombBag_Medium")] = 1;
    owned[row_of("Weapon_Range_Bow")] = 1;
    for (int i = 0; i < OOT_WHEEL_ITEM_COUNT; i++) if (OOT_WHEEL_ITEMS[i].item == 14) owned[i] = 1;
    int n = oot_wheel_list(owned, 1, rows);
    assert(n == 5 && has(rows, n, 0) && has(rows, n, 1) && has(rows, n, 4) && has(rows, n, 2) && has(rows, n, 14) && !has(rows, n, 30));
    /* the same things as an adult: no stick, no slingshot, the bow */
    n = oot_wheel_list(owned, 0, rows);
    assert(n == 4 && has(rows, n, 30) && has(rows, n, 1) && has(rows, n, 2) && has(rows, n, 14) && !has(rows, n, 0) && !has(rows, n, 4));
    /* the Longshot takes the Hookshot's place */
    owned[row_of("Item_Hookshot")] = 1; n = oot_wheel_list(owned, 0, rows); assert(has(rows, n, 5));
    owned[row_of("Item_Longshot")] = 1; n = oot_wheel_list(owned, 0, rows); assert(has(rows, n, 6) && !has(rows, n, 5));
    /* everything owned still fits the slots, each game item once */
    memset(owned, 1, sizeof owned); n = oot_wheel_list(owned, 0, rows); assert(n <= OOT_WHEEL_SLOTS);
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) assert(OOT_WHEEL_ITEMS[rows[a]].item != OOT_WHEEL_ITEMS[rows[b]].item);

    /* the stick: slot 0 up, then clockwise */
    assert(oot_wheel_slot_of(0, 1, 4) == 0 && oot_wheel_slot_of(1, 0, 4) == 1 && oot_wheel_slot_of(0, -1, 4) == 2 && oot_wheel_slot_of(-1, 0, 4) == 3);
    assert(oot_wheel_slot_of(0.1f, 1, 6) == 0 && oot_wheel_slot_of(-0.1f, 1, 6) == 0 && oot_wheel_slot_of(0.9f, 0.5f, 6) == 1 && oot_wheel_slot_of(0, 1, 0) == -1);
    oot_wheel_pick p = { 0, -1 };
    assert(oot_wheel_step(&p, 0, 0, 5) == -1 && p.slot == -1);          /* the stick at rest chooses nothing */
    assert(oot_wheel_step(&p, 0.4f, 0, 5) == -1 && p.slot == -1);        /* not far enough */
    assert(oot_wheel_step(&p, 0.9f, 0.1f, 5) == -1 && p.slot == 1);      /* highlighted */
    assert(oot_wheel_step(&p, 0, -0.9f, 5) == -1 && (p.slot == 2 || p.slot == 3));   /* moved on round the ring */
    assert(oot_wheel_step(&p, -0.9f, 0, 5) == -1 && p.slot == 4);
    assert(oot_wheel_step(&p, -0.4f, 0, 5) == -1 && p.slot == 4);        /* on its way back: still highlighted */
    assert(oot_wheel_step(&p, 0, 0, 5) == 4 && p.slot == -1);            /* let go: chosen, once */
    assert(oot_wheel_step(&p, 0, 0, 5) == -1);
    assert(oot_wheel_step(&p, 0.9f, 0, 0) == -1 && p.slot == -1);        /* an empty wheel */

    float at[2];
    oot_wheel_place(0, 4, 2, at); assert(fabsf(at[0]) < 1e-5f && fabsf(at[1] - 2) < 1e-5f);
    oot_wheel_place(1, 4, 2, at); assert(fabsf(at[0] - 2) < 1e-5f && fabsf(at[1]) < 1e-5f);
    puts("the item wheel: what a child or an adult owns, one slot a game item, the stick highlights and letting go chooses");
    return 0;
}
