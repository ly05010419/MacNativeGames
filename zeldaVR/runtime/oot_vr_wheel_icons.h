/* Ocarina of Time in VR: which of the game's own icons the item wheel shows for a slot, and what a bottle holds (tests/
   test_oot_vr_wheel_icons.c runs it). The icons are the pause menu's (UI/Tex/Icon/<name>.bntx.zs in the player's RomFS),
   made into extracted/oot/vr-icons/<name>.png when the App is packaged (games/oot/scripts/make_vr_icons.py); the wheel
   draws a slot's name when its icon is not there (zeldaVR/gfx/oot_vr_wheel_draw.h).
   Which icon stands for which item follows HookshotVR's notes (its radial item table, vr/menu/radial_items.h; GPL-3: facts
   only, no code); every name was checked present in the RomFS (2026-11-27).
   A bottle's content is the game's own answer, main+0x404b80 (pouch, the bottle's item number) (games/oot/runtime/
   oot_vr_wheel_host.h): read in our image, it hashes the saved field "Bottle<A..E>" and compares the hash with the game's own
   content names, in this order, returning -1 Empty, 0 RedPotion, 1 GreenPotion, 2 BluePotion, 3 BlueFire, 4 Fish, 5 Letter,
   6 BigPoe, 7 Poe, 8 Bug, 9 Fairy, 10 MaxMilk, 11 HalfMilk (the names are a string table at main+0x3eeaf90). The icon of
   each is Item_Bottle_<name>, except Empty (Item_Bottle) and MaxMilk: the game's own files call full milk "Milk"
   (Item_Bottle_Milk, Weapon_Bottle_Milk; no MaxMilk file exists). */
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "oot_vr_wheel.h"

enum { OOT_BOTTLE_UNKNOWN = -2, OOT_BOTTLE_EMPTY = -1, OOT_BOTTLE_LAST = 11 };
typedef struct { const char *icon, *zh, *en; } oot_bottle_content;
/* By content + 1 (0 the empty bottle). */
static const oot_bottle_content OOT_BOTTLE_CONTENTS[OOT_BOTTLE_LAST + 2] = {
    { "Item_Bottle", "空瓶", "Empty" },
    { "Item_Bottle_RedPotion", "红色药水", "Red Potion" },
    { "Item_Bottle_GreenPotion", "绿色药水", "Green Potion" },
    { "Item_Bottle_BluePotion", "蓝色药水", "Blue Potion" },
    { "Item_Bottle_BlueFire", "蓝色火焰", "Blue Fire" },
    { "Item_Bottle_Fish", "鱼", "Fish" },
    { "Item_Bottle_Letter", "露托的信", "Ruto's Letter" },
    { "Item_Bottle_BigPoe", "大幽灵", "Big Poe" },
    { "Item_Bottle_Poe", "幽灵", "Poe" },
    { "Item_Bottle_Bug", "虫子", "Bug" },
    { "Item_Bottle_Fairy", "妖精", "Fairy" },
    { "Item_Bottle_Milk", "龙龙牛奶", "Lon Lon Milk" },
    { "Item_Bottle_HalfMilk", "龙龙牛奶（半瓶）", "Lon Lon Milk (half)" },
};

static inline int oot_wheel_is_bottle(int item) { return item == 14 || item == 15 || item == 16 || item == 17 || item == 50; }
/* What a bottle holds, or NULL when the answer is not one the game gives (OOT_BOTTLE_UNKNOWN: not asked or not answered). */
static inline const oot_bottle_content *oot_bottle_content_of(int content) {
    return content >= OOT_BOTTLE_EMPTY && content <= OOT_BOTTLE_LAST ? &OOT_BOTTLE_CONTENTS[content + 1] : NULL;
}

/* The items for the item button whose icon is not named like the pouch's own count name. */
static const struct { int item; const char *icon; } OOT_WHEEL_ICON_NAMES[] = {
    { 2, "Item_Bomb" },   /* the three bomb bags: the bomb */
    { 8, "Armor_GlassTruth" },
    { 46, "Pouch_Range_BowAndArrow_Fire" },
    { 47, "Pouch_Range_BowAndArrow_Ice" },
    { 48, "Pouch_Range_BowAndArrow_Light" },
};
/* The icon's name for a row of OOT_WHEEL_ITEMS; a bottle's by its content (OOT_BOTTLE_UNKNOWN: none, its name is drawn). */
static inline const char *oot_wheel_icon(const oot_wheel_item *it, int content) {
    if (!it) return NULL;
    if (oot_wheel_is_bottle(it->item)) { const oot_bottle_content *c = oot_bottle_content_of(content); return c ? c->icon : NULL; }
    for (size_t i = 0; i < sizeof OOT_WHEEL_ICON_NAMES / sizeof *OOT_WHEEL_ICON_NAMES; i++)
        if (OOT_WHEEL_ICON_NAMES[i].item == it->item) return OOT_WHEEL_ICON_NAMES[i].icon;
    return it->owned;   /* the rest (the equipment too): the icon has the pouch's own name */
}
/* The name drawn for a slot: a bottle with what it holds ("Bottle 1 · Red Potion"). */
static inline void oot_wheel_label(const oot_wheel_item *it, int content, int zh, char *out, size_t size) {
    if (!size) return;
    const oot_bottle_content *c = it && oot_wheel_is_bottle(it->item) ? oot_bottle_content_of(content) : NULL;
    if (!it) out[0] = 0;
    else if (c) snprintf(out, size, "%s · %s", zh ? it->zh : it->en, zh ? c->zh : c->en);
    else snprintf(out, size, "%s", zh ? it->zh : it->en);
}
