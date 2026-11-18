/* Super Mario Odyssey's own part of the controller input that the shared runtime/gamepad_core.h calls (shared/README.md;
   this game's runtime/gamepad.m keeps its own two seats). Included once, by gamepad.m, before gamepad_core.h. */
#ifndef ODYSSEY_GAMEPAD_GAME_H
#define ODYSSEY_GAMEPAD_GAME_H

/* Mouse mode (the settings panel, mouse.txt): the mouse turns the camera and aims the gyro; its buttons are physical
   buttons 17-21 (mouse lines in controls.txt). No pointing. */
#define GAMEPAD_MOUSE_MODE 1

/* ---- a worn stick's reach (commit 1b55dea4, 2026-10-25): "reach <lstick|rstick> <up|down|left|right> = <0.3..1>" lines in
   controls.txt (controller_map.h controller_reach_parse) say how far a direction still gets; that half-axis is scaled up
   so its full travel gives full input, on GameController's sticks, a Switch controller's HID sticks and the generic HID
   pads alike (not DSU's). ---- */
#define GAMEPAD_GAME_REACH 1
static float stickReach[8] = {1, 1, 1, 1, 1, 1, 1, 1};   /* lstick up, down, left, right, then rstick; 1 = full travel */
static inline float gamepad_game_stick(int axis, float value) {   /* axis 0 lx, 1 ly, 2 rx, 3 ry; y up positive */
    static const int negative[4] = {2, 1, 6, 5}, positive[4] = {3, 0, 7, 4};
    return value / (value < 0 ? stickReach[negative[axis]] : stickReach[positive[axis]]);
}
static inline int gamepad_game_layout_text(const char *text, const char **error) {
    int line = controller_reach_parse(text, stickReach, error);
    for (int i = 0; i < 8 && !line; i++) if (stickReach[i] < 1.0f)
        fprintf(stderr, "[totk-input] %s %s reaches %.2f: scaled to full input\n", i < 4 ? "left stick" : "right stick",
                (const char *[]){"up", "down", "left", "right"}[i % 4], stickReach[i]);
    return line;
}
#endif
