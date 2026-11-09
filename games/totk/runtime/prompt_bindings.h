/* Pure input-to-prompt model. No renderer, files, or platform APIs. */
#ifndef TOTK_PROMPT_BINDINGS_H
#define TOTK_PROMPT_BINDINGS_H
#include "keyboard_map.h"
#include "controller_map.h"
#include <math.h>

typedef enum {
    PROMPT_AUTO, PROMPT_KEYBOARD, PROMPT_XBOX, PROMPT_PLAYSTATION, PROMPT_SWITCH,
    PROMPT_DEVICE_COUNT,
    PROMPT_STYLE_OFF = PROMPT_DEVICE_COUNT   /* a panel choice, not a device: the game's own icons stay */
} PromptDevice;
typedef struct {
    PromptDevice device;
    KeyMap keyboard;
    uint16_t controller[CONTROLLER_BUTTONS];
    int mouse_enabled;
} PromptBindings;
typedef struct {
    PromptDevice device;
    unsigned count;
    int input[4];              /* Mac key codes / mouse inputs, or physical pad buttons. */
} PromptIcon;

/* Fresh input selects a pad; a held button or resting/noisy stick does not
   steal prompts back immediately after keyboard input. */
static inline int prompt_activity_changed(uint64_t before, uint64_t now, const float previous[4], const float axes[4]) {
    if (now & ~before) return 1;
    for (unsigned i = 0; i < 4; i++)
        if (fabsf(axes[i]) > .3f && fabsf(axes[i] - previous[i]) > .12f) return 1;
    return 0;
}

/* The icon font's code points are independent of language. -1 leaves a glyph
   unchanged; in particular, this does not replace arbitrary private-use text. */
static inline int prompt_glyph_action(unsigned cp) {
    switch (cp) {
        case 0xe040: return KEYMAP_A; case 0xe041: return KEYMAP_B;
        case 0xe042: return KEYMAP_X; case 0xe043: return KEYMAP_Y;
        case 0xe044: return KEYMAP_L; case 0xe045: return KEYMAP_R;
        case 0xe046: return KEYMAP_ZL; case 0xe047: return KEYMAP_ZR;
        case 0xe04b: return KEYMAP_PLUS; case 0xe04c: return KEYMAP_MINUS;
        case 0xe056: return KEYMAP_LSTICK; case 0xe057: return KEYMAP_RSTICK;
        case 0xe060: return KEYMAP_DUP; case 0xe061: return KEYMAP_DDOWN;
        case 0xe062: return KEYMAP_DLEFT; case 0xe063: return KEYMAP_DRIGHT;
        default: return -1;
    }
}

static inline int prompt_action_input(const PromptBindings *b, int action) {
    if (action < 0 || action >= KEYMAP_ACTIONS) return -1;
    if (b->device == PROMPT_KEYBOARD) {
        for (int slot = 0; slot < KEYMAP_SLOTS; slot++) {
            int input = b->keyboard.slot[action][slot];
            if (input < KEYMAP_INPUTS && (input < KEYMAP_MOUSE_FIRST || b->mouse_enabled)) return input;
        }
    } else if (action < CONTROLLER_SWITCH_BUTTONS) {
        for (int i = 0; i < CONTROLLER_MOUSE_FIRST; i++)
            if (b->controller[i] & (1u << action)) return i;
    }
    return -1;
}

static inline int prompt_resolve(const PromptBindings *b, unsigned cp, PromptIcon *out) {
    if (b->device <= PROMPT_AUTO || b->device >= PROMPT_DEVICE_COUNT) return 0;
    *out = (PromptIcon){.device = b->device};
    int actions[4], count = 1, action = prompt_glyph_action(cp);
    if (action >= 0) actions[0] = action;
    else {
        switch (cp) {
            case 0xe049: count = 4; actions[0] = KEYMAP_DUP; actions[1] = KEYMAP_DLEFT;
                actions[2] = KEYMAP_DDOWN; actions[3] = KEYMAP_DRIGHT; break;
            case 0xe064: count = 2; actions[0] = KEYMAP_DUP; actions[1] = KEYMAP_DDOWN; break;
            case 0xe065: count = 2; actions[0] = KEYMAP_DLEFT; actions[1] = KEYMAP_DRIGHT; break;
            default: {
                /* Movement is a stick for pads; keyboard prompts name the
                   configured directional keys. Odd code points are camera. */
                if (!((cp >= 0xe050 && cp <= 0xe055) || (cp >= 0xe058 && cp <= 0xe05f) || cp == 0xe068)) return 0;
                if (b->device != PROMPT_KEYBOARD) {
                    out->count = 1; out->input[0] = cp & 1 || cp == 0xe068 ? 5 : 4;
                    return 1;
                }
                int base = cp & 1 || cp == 0xe068 ? KEYMAP_CAMERA_UP : KEYMAP_MOVE_UP;
                switch (cp & ~1u) {
                    case 0xe050: actions[0] = base + 1; break;
                    case 0xe052: actions[0] = base; break;
                    case 0xe054: count = 2; actions[0] = base + 2; actions[1] = base + 3; break;
                    case 0xe058: actions[0] = base + 3; break;
                    case 0xe05a: actions[0] = base + 2; break;
                    case 0xe05c: count = 2; actions[0] = base; actions[1] = base + 1; break;
                    default: count = 4; actions[0] = base; actions[1] = base + 2;
                        actions[2] = base + 1; actions[3] = base + 3; break;
                }
            }
        }
    }
    out->count = (unsigned)count;
    for (int i = 0; i < count; i++) out->input[i] = prompt_action_input(b, actions[i]);
    return 1;
}
#endif
