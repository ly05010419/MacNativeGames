/* Ocarina of Time's own part of the controller input: the hooks the shared runtime/gamepad.m and gamepad_core.h call
   (shared/README.md). Included once, by gamepad.m, after its state (gamepadState) and before gamepad_core.h. */
#ifndef OOT_GAMEPAD_GAME_H
#define OOT_GAMEPAD_GAME_H

/* Mouse mode (the settings panel, mouse.txt): the mouse turns the camera and aims the gyro; its buttons are physical
   buttons 17-21 (mouse lines in controls.txt). */
#define GAMEPAD_MOUSE_MODE 1

/* Pointing while L is held, as in the copy of games/totk's gamepad.m this game had (there it is Tears of the Kingdom's
   ability wheel): the mouse then moves a stick that stays where it is put instead of turning the camera (mouse_look.h). */
#define GAMEPAD_GAME_POINTING 1
static inline int gamepad_game_mouse_points(void) { return (int)(((runtime_keyboard_buttons() | gamepadState.buttons) >> 6) & 1); }

/* zeldaVR's linkage (zeldaVR/gfx/totk_vr.m, linked but not enabled for this title): it sets these while VR runs. */
#define GAMEPAD_GAME_VR 1
void (*runtime_vr_recenter)(void);   /* the L3 + R3 tap recentres the headset */
int (*runtime_vr_pad)(int plain, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);   /* the VR thread's controllers */
static inline void (*gamepad_game_chord_tap(void))(void) { return __atomic_load_n(&runtime_vr_recenter, __ATOMIC_ACQUIRE); }
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

/* The input trace (OOT_INPUT_TRACE=1 or "flag input_trace 1", runtime/input.c, docs/gpu-profiling.md): bounded lines at
   each step from GameController to the game's read; diagnostics only. */
#define GAMEPAD_GAME_TRACE 1
#define GAMEPAD_TRACE_TAG "[oot-input-trace]"
static inline uint64_t gamepad_game_trace_row(void) { return runtime_input_trace_next(); }
#endif
