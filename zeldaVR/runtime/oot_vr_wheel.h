/* Ocarina of Time in VR: the item wheel, without the runtime (tests/test_oot_vr_wheel.c runs it). A ring of the items Link
   owns and can use at his age, floating in front of the player: the right stick pushed toward one highlights it, the stick
   let back to the middle chooses it, and the game's own call puts it on the item button (games/oot/runtime/oot_vr_sword.h).
   The idea of a radial wheel chosen by the stick, the item numbers and the names the game counts them under follow
   HookshotVR's notes (vr/menu/radial_host.c, radial_items.h, GPL-3: facts and ideas, no code). The labels are ours. */
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { OOT_WHEEL_BOTH, OOT_WHEEL_ADULT, OOT_WHEEL_CHILD };
typedef struct {
    int item;             /* the game's number for the item button */
    const char *owned;    /* owned when the pouch holds this (NULL: by the saved flag below) */
    uint32_t flag;        /* a saved Bool's hash (the bottles) */
    int age;
    const char *zh, *en;
    int gear;             /* 0 an item for the item button; 1 a sword, 2 a shield, 3 a tunic: the equipment wheel's, worn by the game's own call */
} oot_wheel_item;
static const oot_wheel_item OOT_WHEEL_ITEMS[] = {
    { 30, "Weapon_Range_Bow", 0, OOT_WHEEL_ADULT, "弓", "Bow", 0 },
    { 46, "Item_Arrow_Fire", 0, OOT_WHEEL_ADULT, "火之箭", "Fire Arrows", 0 },
    { 47, "Item_Arrow_Ice", 0, OOT_WHEEL_ADULT, "冰之箭", "Ice Arrows", 0 },
    { 48, "Item_Arrow_Light", 0, OOT_WHEEL_ADULT, "光之箭", "Light Arrows", 0 },
    { 6, "Item_Longshot", 0, OOT_WHEEL_ADULT, "长钩爪", "Longshot", 0 },
    { 5, "Item_Hookshot", 0, OOT_WHEEL_ADULT, "钩爪", "Hookshot", 0 },
    { 2, "Item_BombBag_Small", 0, OOT_WHEEL_BOTH, "炸弹", "Bombs", 0 },
    { 2, "Item_BombBag_Medium", 0, OOT_WHEEL_BOTH, "炸弹", "Bombs", 0 },
    { 2, "Item_BombBag_Large", 0, OOT_WHEEL_BOTH, "炸弹", "Bombs", 0 },
    { 9, "Item_Bombchu", 0, OOT_WHEEL_BOTH, "炸弹老鼠", "Bombchus", 0 },
    { 7, "Item_Hammer", 0, OOT_WHEEL_ADULT, "百万吨锤", "Megaton Hammer", 0 },
    { 1, "Item_DekuNut", 0, OOT_WHEEL_BOTH, "德库果实", "Deku Nuts", 0 },
    { 0, "Item_DekuStick", 0, OOT_WHEEL_CHILD, "德库木棒", "Deku Stick", 0 },
    { 4, "Weapon_Range_SlingShot", 0, OOT_WHEEL_CHILD, "妖精弹弓", "Fairy Slingshot", 0 },
    { 3, "Item_Boomerang", 0, OOT_WHEEL_CHILD, "回旋镖", "Boomerang", 0 },
    { 8, "Item_GlassTruth", 0, OOT_WHEEL_BOTH, "真实之镜", "Lens of Truth", 0 },
    { 10, "Item_DinFire", 0, OOT_WHEEL_BOTH, "迪恩之火", "Din's Fire", 0 },
    { 11, "Item_FaroreWind", 0, OOT_WHEEL_BOTH, "法罗尔之风", "Farore's Wind", 0 },
    { 12, "Item_NayruLove", 0, OOT_WHEEL_BOTH, "奈鲁之爱", "Nayru's Love", 0 },
    { 13, "Item_MagicBean", 0, OOT_WHEEL_CHILD, "魔法豆", "Magic Beans", 0 },
    { 14, NULL, 0xf39ce410u, OOT_WHEEL_BOTH, "瓶子 1", "Bottle 1", 0 },
    { 15, NULL, 0x000ff6eau, OOT_WHEEL_BOTH, "瓶子 2", "Bottle 2", 0 },
    { 16, NULL, 0xf63b1194u, OOT_WHEEL_BOTH, "瓶子 3", "Bottle 3", 0 },
    { 17, NULL, 0xb7f052ceu, OOT_WHEEL_BOTH, "瓶子 4", "Bottle 4", 0 },
    { 50, NULL, 0xe27b136du, OOT_WHEEL_BOTH, "瓶子 5", "Bottle 5", 0 },
    /* the equipment wheel (vr.md row 87; HookshotVR's equipment table: names, kinds, ages and which hides which; its ocarina rows are
       left out, we have no ocarina in the hands). The numbers are only the wheel's own, from 1000. */
    { 1001, "Weapon_Sword_Kokiri", 0, OOT_WHEEL_CHILD, "科基利剑", "Kokiri Sword", 1 },
    { 1002, "Weapon_Sword_Master", 0, OOT_WHEEL_ADULT, "大师之剑", "Master Sword", 1 },
    { 1003, "Weapon_Sword_Biggoron", 0, OOT_WHEEL_ADULT, "大哥隆之剑", "Biggoron's Sword", 1 },
    { 1004, "Weapon_Sword_Giant", 0, OOT_WHEEL_ADULT, "巨人之刀", "Giant's Knife", 1 },
    { 1005, "Weapon_Sword_BrokenGiant", 0, OOT_WHEEL_ADULT, "断掉的巨人之刀", "Broken Giant's Knife", 1 },
    { 1011, "Weapon_Shield_Deku", 0, OOT_WHEEL_CHILD, "德库盾", "Deku Shield", 2 },
    { 1012, "Weapon_Shield_Hylian", 0, OOT_WHEEL_BOTH, "海利亚之盾", "Hylian Shield", 2 },
    { 1013, "Weapon_Shield_Mirror", 0, OOT_WHEEL_ADULT, "镜之盾", "Mirror Shield", 2 },
    { 1021, "Armor_Adult_Normal", 0, OOT_WHEEL_ADULT, "科基利服", "Kokiri Tunic", 3 },
    { 1022, "Armor_Adult_Goron", 0, OOT_WHEEL_ADULT, "哥隆服", "Goron Tunic", 3 },
    { 1023, "Armor_Adult_Zora", 0, OOT_WHEEL_ADULT, "卓拉服", "Zora Tunic", 3 },
};
enum { OOT_WHEEL_ITEM_COUNT = sizeof OOT_WHEEL_ITEMS / sizeof *OOT_WHEEL_ITEMS, OOT_WHEEL_SLOTS = 20 };

/* The wheel's slots for what is owned: rows of the table, one a game item (the three bomb bags are one slot), none of the
   other age, the Hookshot left out once the Longshot is owned; gear 1: the equipment wheel's rows instead, the Giant's Knife
   left out once Biggoron's Sword is owned and the broken one once either is. owned[i]: row i's pouch count or flag says so. */
static inline int oot_wheel_owned(const uint8_t owned[OOT_WHEEL_ITEM_COUNT], int item) {
    for (int i = 0; i < OOT_WHEEL_ITEM_COUNT; i++) if (OOT_WHEEL_ITEMS[i].item == item && owned[i]) return 1;
    return 0;
}
static inline int oot_wheel_list(const uint8_t owned[OOT_WHEEL_ITEM_COUNT], int child, int gear, int rows[OOT_WHEEL_SLOTS]) {
    int n = 0;
    const int longshot = oot_wheel_owned(owned, 6), biggoron = oot_wheel_owned(owned, 1003), giant = oot_wheel_owned(owned, 1004);
    for (int i = 0; i < OOT_WHEEL_ITEM_COUNT && n < OOT_WHEEL_SLOTS; i++) {
        const oot_wheel_item *it = &OOT_WHEEL_ITEMS[i];
        if ((it->gear != 0) != (gear != 0)) continue;
        if (!owned[i] || (it->age == OOT_WHEEL_ADULT && child) || (it->age == OOT_WHEEL_CHILD && !child)) continue;
        if (it->item == 5 && longshot) continue;
        if ((it->item == 1004 && biggoron) || (it->item == 1005 && (biggoron || giant))) continue;
        int twice = 0;
        for (int k = 0; k < n; k++) if (OOT_WHEEL_ITEMS[rows[k]].item == it->item) twice = 1;
        if (!twice) rows[n++] = i;
    }
    return n;
}

/* The stick over the wheel. Slot 0 is at the top and they go round clockwise. Pushed past 0.55 the stick highlights the slot
   it points at; let back under 0.25 after that, it chooses the highlighted one. */
typedef struct { int armed, slot; } oot_wheel_pick;
static inline int oot_wheel_slot_of(float x, float y, int n) {
    if (n <= 0) return -1;
    float a = atan2f(x, y); if (a < 0) a += 2 * (float)M_PI;
    return (int)floorf(a / (2 * (float)M_PI / (float)n) + 0.5f) % n;
}
/* One reading of the stick: the slot chosen now, or -1. p->slot is the highlighted one (-1 none). */
static inline int oot_wheel_step(oot_wheel_pick *p, float x, float y, int n) {
    const float push = sqrtf(x * x + y * y);
    if (n <= 0) { p->armed = 0; p->slot = -1; return -1; }
    if (push > 0.55f) { p->armed = 1; p->slot = oot_wheel_slot_of(x, y, n); return -1; }
    if (p->armed && push < 0.25f) { const int chosen = p->slot; p->armed = 0; p->slot = -1; return chosen; }
    if (!p->armed) p->slot = -1;
    return -1;
}
/* Where slot k of n sits on a ring of this radius: x to the right, y up. */
static inline void oot_wheel_place(int k, int n, float radius, float out[2]) {
    const float a = n > 0 ? (float)k * 2 * (float)M_PI / (float)n : 0;
    out[0] = sinf(a) * radius; out[1] = cosf(a) * radius;
}
