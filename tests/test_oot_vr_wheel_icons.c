/* The item wheel's icons: which of the game's icons a slot shows, and a bottle's content (zeldaVR/runtime/oot_vr_wheel_icons.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_wheel_icons.h"

static const oot_wheel_item *item(int number) { for (int i = 0; i < OOT_WHEEL_ITEM_COUNT; i++) if (OOT_WHEEL_ITEMS[i].item == number) return &OOT_WHEEL_ITEMS[i]; return NULL; }
static int same(const char *a, const char *b) { return a && b && !strcmp(a, b); }

int main(void) {
    /* every row has an icon (a bottle once its content is known); printed, tests/test_oot_vr_wheel_icons.py checks that the
       packaging script makes each */
    for (int i = 0; i < OOT_WHEEL_ITEM_COUNT; i++) {
        const char *icon = oot_wheel_icon(&OOT_WHEEL_ITEMS[i], OOT_BOTTLE_EMPTY);
        assert(icon);
        printf("icon %s\n", icon);
    }
    for (int c = OOT_BOTTLE_EMPTY; c <= OOT_BOTTLE_LAST; c++) printf("icon %s\n", oot_bottle_content_of(c)->icon);
    /* the names that are not the pouch's */
    assert(same(oot_wheel_icon(item(2), -2), "Item_Bomb"));
    assert(same(oot_wheel_icon(item(8), -2), "Armor_GlassTruth"));
    assert(same(oot_wheel_icon(item(46), -2), "Pouch_Range_BowAndArrow_Fire"));
    assert(same(oot_wheel_icon(item(48), -2), "Pouch_Range_BowAndArrow_Light"));
    assert(same(oot_wheel_icon(item(30), -2), "Weapon_Range_Bow"));
    assert(same(oot_wheel_icon(item(1003), -2), "Weapon_Sword_Biggoron"));
    assert(same(oot_wheel_icon(item(1021), -2), "Armor_Adult_Normal"));
    /* bottles: by what they hold, the game's order (main+0x404b80's answers); full milk is the "Milk" icon */
    assert(oot_wheel_is_bottle(14) && oot_wheel_is_bottle(17) && oot_wheel_is_bottle(50) && !oot_wheel_is_bottle(18) && !oot_wheel_is_bottle(13));
    assert(same(oot_wheel_icon(item(14), -1), "Item_Bottle"));
    assert(same(oot_wheel_icon(item(15), 0), "Item_Bottle_RedPotion"));
    assert(same(oot_wheel_icon(item(16), 3), "Item_Bottle_BlueFire"));
    assert(same(oot_wheel_icon(item(17), 5), "Item_Bottle_Letter"));
    assert(same(oot_wheel_icon(item(50), 9), "Item_Bottle_Fairy"));
    assert(same(oot_wheel_icon(item(50), 10), "Item_Bottle_Milk"));
    assert(same(oot_wheel_icon(item(50), 11), "Item_Bottle_HalfMilk"));
    /* an answer the game does not give, or none yet: no icon, the name is drawn */
    assert(!oot_wheel_icon(item(14), OOT_BOTTLE_UNKNOWN) && !oot_wheel_icon(item(14), 12) && !oot_wheel_icon(item(14), -3));
    assert(!oot_wheel_icon(NULL, 0));
    /* the label: a bottle with its content, others as before */
    char text[64];
    oot_wheel_label(item(15), 0, 0, text, sizeof text); assert(!strcmp(text, "Bottle 2 · Red Potion"));
    oot_wheel_label(item(14), -1, 1, text, sizeof text); assert(!strcmp(text, "瓶子 1 · 空瓶"));
    oot_wheel_label(item(14), OOT_BOTTLE_UNKNOWN, 0, text, sizeof text); assert(!strcmp(text, "Bottle 1"));
    oot_wheel_label(item(7), 3, 0, text, sizeof text); assert(!strcmp(text, "Megaton Hammer"));
    oot_wheel_label(item(15), 0, 0, text, 8); assert(strlen(text) == 7);
    puts("wheel icons: ok");
    return 0;
}
