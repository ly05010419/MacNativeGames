#include "prompt_bindings.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    float resting[4] = {0}, drift[4] = {.1f, -.2f}, moved[4] = {.7f};
    assert(!prompt_activity_changed(0, 0, resting, drift));
    assert(prompt_activity_changed(0, 1, resting, resting));
    assert(!prompt_activity_changed(1, 1, resting, resting));
    assert(prompt_activity_changed(0, 0, resting, moved));
    assert(!prompt_activity_changed(0, 0, moved, moved));
    PromptBindings b = {.device = PROMPT_KEYBOARD};
    keymap_default(&b.keyboard);
    uint8_t modes[CONTROLLER_BUTTONS]; controller_map_default(b.controller, modes);
    PromptIcon icon;
    assert(prompt_resolve(&b, 0xe040, &icon) && icon.input[0] == 14); /* Talk: E. */
    assert(prompt_resolve(&b, 0xe04b, &icon) && icon.input[0] == 53); /* Menu: Escape. */
    assert(prompt_resolve(&b, 0xe043, &icon) && icon.input[0] == 5);  /* Mouse off: G. */
    b.mouse_enabled = 1;
    assert(prompt_resolve(&b, 0xe043, &icon) && icon.input[0] == 128);
    const char *error = NULL;
    assert(!keymap_parse("a = f\nmove_up = arrow_up\nminus = none\n", &b.keyboard, &error));
    assert(prompt_resolve(&b, 0xe040, &icon) && icon.input[0] == 3); /* Remapped talk. */
    assert(prompt_resolve(&b, 0xe04c, &icon) && icon.input[0] == -1);
    assert(prompt_resolve(&b, 0xe05e, &icon) && icon.count == 4 && icon.input[0] == 126);
    assert(!prompt_resolve(&b, 'A', &icon));
    assert(!prompt_resolve(&b, 0xe048, &icon));
    b.device = PROMPT_XBOX;
    assert(prompt_resolve(&b, 0xe040, &icon) && icon.input[0] == 1); /* Physical east: Xbox B. */
    b.device = PROMPT_PLAYSTATION;
    assert(prompt_resolve(&b, 0xe040, &icon) && icon.input[0] == 1); /* Physical east: Circle. */
    b.controller[0] = 1u << KEYMAP_A;
    b.controller[1] = 0;
    assert(prompt_resolve(&b, 0xe040, &icon) && icon.input[0] == 0); /* Custom south: Cross / A. */
    b.controller[0] = 0;
    assert(prompt_resolve(&b, 0xe040, &icon) && icon.input[0] == -1);
    for (int device = PROMPT_KEYBOARD; device < PROMPT_DEVICE_COUNT; device++) {
        b.device = device;
        for (unsigned cp = 0xe040; cp <= 0xe068; cp++) {
            if (!prompt_resolve(&b, cp, &icon)) continue;
            assert(icon.count > 0 && icon.count <= 4);
            for (unsigned i = 0; i < icon.count; i++)
                assert(icon.input[i] >= -1 && icon.input[i] < KEYMAP_INPUTS);
        }
    }
    b.device = PROMPT_STYLE_OFF;   /* the panel's Off: every glyph stays the game's */
    for (unsigned cp = 0xe040; cp <= 0xe068; cp++) assert(!prompt_resolve(&b, cp, &icon));
    puts("prompt binding cases passed");
}
