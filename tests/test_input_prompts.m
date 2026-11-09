#include "input_prompts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int runtime_setting_word(const char *name, char *word, size_t size) {
    (void)name; (void)word; (void)size; return 0;
}
void runtime_setting_save(const char *name, const char *word, const char *what) {
    (void)name; (void)word; (void)what;
}
static unsigned mask_hash(const PromptBindings *b, unsigned cp) {
    unsigned char pixels[64 * 64]; memset(pixels, 0, sizeof pixels);
    assert(runtime_prompt_draw(b, cp, pixels, 64, 64, 64));
    unsigned h = 2166136261u;
    for (unsigned i = 0; i < sizeof pixels; i++) h = (h ^ pixels[i]) * 16777619u;
    return h;
}
int main(void) {
    PromptBindings b;
    uint64_t first = runtime_prompt_snapshot(&b);
    assert(b.device == PROMPT_KEYBOARD);
    unsigned keyboard = mask_hash(&b, 0xe040);
    runtime_prompt_note_input(PROMPT_KEYBOARD);
    assert(runtime_prompt_snapshot(&b) == first);
    runtime_prompt_note_input(PROMPT_PLAYSTATION);
    assert(runtime_prompt_snapshot(&b) > first && b.device == PROMPT_PLAYSTATION);
    unsigned ps = mask_hash(&b, 0xe040); assert(ps != keyboard);
    runtime_prompt_note_input(PROMPT_XBOX); runtime_prompt_snapshot(&b);
    unsigned xbox = mask_hash(&b, 0xe040); assert(xbox != ps && xbox != keyboard);
    runtime_prompt_style_set(PROMPT_KEYBOARD);
    uint64_t fixed = runtime_prompt_snapshot(&b);
    runtime_prompt_note_input(PROMPT_PLAYSTATION);
    assert(runtime_prompt_snapshot(&b) == fixed && b.device == PROMPT_KEYBOARD);
    KeyMap map; keymap_default(&map); map.slot[KEYMAP_A][0] = 3;
    runtime_prompt_keyboard(&map);
    assert(runtime_prompt_snapshot(&b) > fixed);
    assert(mask_hash(&b, 0xe040) != keyboard);
    unsigned char letter[64 * 64];
    assert(runtime_prompt_draw(&b, 0xe040, letter, 64, 64, 64));
    int top = 0, bottom = 0;
    for (unsigned y = 20; y < 26; y++) for (unsigned x = 16; x < 48; x++) {
        top += letter[y * 64 + x] < 128;
        bottom += letter[(y + 18) * 64 + x] < 128;
    }
    assert(top > bottom); /* F's horizontal bars must not be upside down. */
    runtime_prompt_style_set(PROMPT_AUTO); runtime_prompt_snapshot(&b);
    assert(b.device == PROMPT_PLAYSTATION);
    for (unsigned cp = 0xe040; cp <= 0xe068; cp++) {
        PromptIcon icon;
        if (prompt_resolve(&b, cp, &icon)) (void)mask_hash(&b, cp);
    }
    puts("live prompt state and mask cases passed");
}
