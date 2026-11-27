/* Per-port mode state. No device or UI dependencies; all callers serialize on
   the input event queue. Connection changes must clear this state. */
#pragma once
#include "controller_map.h"
typedef struct {
    uint64_t previous, tap_until[CONTROLLER_BUTTONS];
    uint8_t toggle[CONTROLLER_BUTTONS], moved[CONTROLLER_BUTTONS];
} ControllerModeState;
static inline uint64_t controller_state_apply(ControllerModeState *state,
        const uint16_t *map, const uint8_t *modes, uint64_t physical,
        int moving, int connected, uint64_t now, int *pulse_started) {
    uint64_t edges = physical & ~state->previous, synthetic = 0, suppressed = 0;
    state->previous = physical;
    *pulse_started = 0;
    for (int i = 0; i < CONTROLLER_BUTTONS; ++i) {
        uint64_t bit = UINT64_C(1) << i;
        int kind = controller_mode_kind(modes[i]);
        if (kind == CONTROLLER_TAP && (edges & bit)) {
            state->tap_until[i] = now + UINT64_C(400000000);
            *pulse_started = 1;
        }
        if (kind == CONTROLLER_TOGGLE) {
            if (edges & bit) { state->toggle[i] ^= 1; state->moved[i] = !!moving; }
            if (state->toggle[i] && moving) state->moved[i] = 1;
            if (state->toggle[i] && ((state->moved[i] && !moving) || !connected)) state->toggle[i] = 0;
        }
        if (kind == CONTROLLER_AIM && (physical & bit)) {
            uint64_t fire = UINT64_C(1) << controller_mode_fire(modes[i]);
            suppressed |= fire;
            if (!(physical & fire)) synthetic |= map[i];
        }
        int active = kind == CONTROLLER_TAP ? now < state->tap_until[i] :
                     kind == CONTROLLER_TOGGLE ? state->toggle[i] : 0;
        if (active) synthetic |= map[i];
    }
    uint64_t held = physical & ~suppressed;
    for (int i = 0; i < CONTROLLER_BUTTONS; ++i)
        if (modes[i] != CONTROLLER_HOLD) held &= ~(UINT64_C(1) << i);
    return controller_map_apply(map, held) | synthetic;
}
