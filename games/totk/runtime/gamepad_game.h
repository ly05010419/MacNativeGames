/* Tears of the Kingdom's (and Breath of the Wild's, Donkey Kong Bananza's and Pokopia's) own part of the controller input:
   the hooks the shared runtime/gamepad.m and gamepad_core.h call (shared/README.md). Included once, by gamepad.m, after
   its state (gamepadState) and before gamepad_core.h. */
#ifndef TOTK_GAMEPAD_GAME_H
#define TOTK_GAMEPAD_GAME_H
#include "input_prompts.h"

/* Mouse mode (the settings panel, mouse.txt): the mouse turns the camera and aims the gyro; its buttons are physical
   buttons 17-21 (mouse lines in controls.txt). */
#define GAMEPAD_MOUSE_MODE 1

/* ---- the live button prompts (runtime/input_prompts.m): the device used last decides the icons ---- */
#define GAMEPAD_GAME_PROMPTS 1
static inline void gamepad_game_layout(const uint16_t map[CONTROLLER_BUTTONS]) { runtime_prompt_controller(map); }
static inline void gamepad_game_mouse_mode(int on) { runtime_prompt_mouse_mode(on); }
static inline void gamepad_game_mouse_moved(void) { runtime_prompt_note_input(PROMPT_KEYBOARD); }
/* A controller's state in one refresh: a prompt change only when it really moved. Activity is kept per controller, so a
   second pad's resting stick does not keep looking like a fresh movement against the first pad's. type: the player's
   pick (the settings panel), which decides how the face buttons map, so it decides the labels too. */
static inline void gamepad_game_pad_input(GCController *controller, uint64_t physical, const float axes[4], int type) {
    typedef struct { uint64_t buttons; float axes[4]; } PromptActivity;
    static NSMapTable<GCController *, NSMutableData *> *promptActivity;
    if (!promptActivity) promptActivity = [NSMapTable weakToStrongObjectsMapTable];
    NSMutableData *history = [promptActivity objectForKey:controller];
    if (!history) {
        history = [NSMutableData dataWithLength:sizeof(PromptActivity)];
        [promptActivity setObject:history forKey:controller];
    }
    PromptActivity *previous = history.mutableBytes;
    int meaningful = prompt_activity_changed(previous->buttons, physical, previous->axes, axes);
    previous->buttons = physical;
    memcpy(previous->axes, axes, sizeof previous->axes);
    if (!meaningful) return;
    if (type == GAMEPAD_TYPE_AUTO) type = gamepad_type_detect(controller.productCategory.UTF8String, controller.vendorName.UTF8String);
    runtime_prompt_note_input(type == GAMEPAD_TYPE_PLAYSTATION ? PROMPT_PLAYSTATION : type == GAMEPAD_TYPE_XBOX ? PROMPT_XBOX : PROMPT_SWITCH);
}
static inline void gamepad_game_hid_input(uint64_t buttons, const float axes[4]) {   /* the generic HID pads are Switch-style */
    static struct { uint64_t buttons; float axes[4]; } previous;
    if (prompt_activity_changed(previous.buttons, buttons, previous.axes, axes)) runtime_prompt_note_input(PROMPT_SWITCH);
    previous.buttons = buttons;
    memcpy(previous.axes, axes, sizeof previous.axes);
}
static inline void gamepad_game_dsu_input(int active) { if (active) runtime_prompt_note_input(PROMPT_SWITCH); }

/* ---- the ability wheel: L held (keys or controller) opens it, and the mouse points at an ability (mouse_look.h).
   Breath of the Wild has no such wheel. ---- */
#define GAMEPAD_GAME_POINTING 1
static inline int gamepad_game_mouse_points(void) {
#ifndef GAME_BOTW
    return (int)(((runtime_keyboard_buttons() | gamepadState.buttons) >> 6) & 1);
#else
    return 0;
#endif
}

/* ---- zeldaVR: set by gfx/nvn/totk_vr.m while VR runs ---- */
#define GAMEPAD_GAME_VR 1
void (*runtime_vr_recenter)(void);   /* the L3 + R3 tap recentres the headset (straight ahead and the 6DoF origin) */
int (*runtime_vr_pad)(int plain, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);   /* the VR thread's controllers */
static inline void (*gamepad_game_chord_tap(void))(void) { return __atomic_load_n(&runtime_vr_recenter, __ATOMIC_ACQUIRE); }
/* The Quest 3 controllers (gfx/nvn/totk_vr.m vr_gamepad), merged like the touch screen; plain: by name, for the panel. */
static inline void gamepad_game_merge(RuntimeGamepadState *out, int plain) {
    uint64_t vb; int32_t vlx, vly, vrx, vry;
    int (*pad)(int, uint64_t *, int32_t *, int32_t *, int32_t *, int32_t *) = __atomic_load_n(&runtime_vr_pad, __ATOMIC_ACQUIRE);
    if (!pad || !pad(plain, &vb, &vlx, &vly, &vrx, &vry)) return;
    out->buttons |= vb; out->connected = 1;
    if (abs(vlx) > abs(out->lx)) out->lx = vlx;
    if (abs(vly) > abs(out->ly)) out->ly = vly;
    if (abs(vrx) > abs(out->rx)) out->rx = vrx;
    if (abs(vry) > abs(out->ry)) out->ry = vry;
}
#endif
