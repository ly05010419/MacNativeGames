/* The controller plumbing every game's runtime/gamepad.m shares (shared/README.md, 2026-11-18): the saved choices (layout,
   controller type, settings panel and screenshot buttons, background input), GameController's controllers and their
   physical buttons, the generic HID pads, a Switch controller's sticks and capture button from its HID reports
   (gamepad_feedback.m), mouse mode, the display kept awake, the battery, the capture hook, iPad hardware keyboards and the
   start. What a game does with the buttons stays in its gamepad.m: games/totk, games/oot, games/acnh and games/eow link the
   shared single-player runtime/gamepad.m; games/odyssey has its own two seats.

   The including gamepad.m first declares gamepadLock and its own state, then includes its runtime/gamepad_game.h (the
   game's hooks and switches, defaults below), then this file, and then defines:
     refresh_gamepads()       read every source again (main thread)
     settings_chord_held()    the settings panel's button or chord is held (gamepadLock held)
     gamepad_mapping_reset()  forget taps and toggles: the layout changed, or the settings panel took the controller
     attach_gamepad(c), detach_gamepad(c)   GameController offers or takes a controller (main queue)
   and runtime_gamepad_clear(). */
#ifndef GAMEPAD_CORE_H
#define GAMEPAD_CORE_H
#if TARGET_OS_OSX
#import <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#endif
#include "switch_left_fallback.h"
#include "dsu_client.h"

/* ---- the game's hooks (runtime/gamepad_game.h); a group it leaves out does nothing ---- */
#ifndef GAMEPAD_MOUSE_MODE
#define GAMEPAD_MOUSE_MODE 0   /* 1: mouse mode (gamepad_mouse.h) and mouse lines in controls.txt */
#endif
#ifndef GAMEPAD_GAME_PROMPTS   /* the live button prompts: what was used last, and the layout */
static inline void gamepad_game_layout(const uint16_t map[CONTROLLER_BUTTONS]) { (void)map; }
static inline void gamepad_game_mouse_mode(int on) { (void)on; }
static inline void gamepad_game_mouse_moved(void) {}
static inline void gamepad_game_pad_input(GCController *controller, uint64_t physical, const float axes[4], int type) { (void)controller; (void)physical; (void)axes; (void)type; }
static inline void gamepad_game_hid_input(uint64_t buttons, const float axes[4]) { (void)buttons; (void)axes; }
static inline void gamepad_game_dsu_input(int active) { (void)active; }
#endif
#ifndef GAMEPAD_GAME_POINTING   /* 1 while the mouse points (mouse_look_point) instead of turning the camera */
static inline int gamepad_game_mouse_points(void) { return 0; }
#endif
#ifndef GAMEPAD_GAME_VR   /* a headset: the settings chord tapped (NULL: the chord opens the panel at once), its controllers */
static inline void (*gamepad_game_chord_tap(void))(void) { return NULL; }
static inline void gamepad_game_merge(RuntimeGamepadState *out, int plain) { (void)out; (void)plain; }
#endif
#ifndef GAMEPAD_GAME_REACH   /* a stick axis (0 lx, 1 ly, 2 rx, 3 ry; -1..1, up positive) before the dead zone; the layout's own lines */
static inline float gamepad_game_stick(int axis, float value) { (void)axis; return value; }
static inline int gamepad_game_layout_text(const char *text, const char **error) { (void)text; (void)error; return 0; }
#endif
#ifndef GAMEPAD_GAME_TRACE   /* a diagnostic trace of the input path: the next line's number, 0 off */
static inline uint64_t gamepad_game_trace_row(void) { return 0; }
#define GAMEPAD_TRACE_TAG "[input-trace]"
#endif

/* ---- what the including gamepad.m defines ---- */
static void refresh_gamepads(void);
static int settings_chord_held(void);
static void gamepad_mapping_reset(void);
static void attach_gamepad(GCController *controller);
static void detach_gamepad(GCController *controller);

static BOOL started;
static _Atomic int textCapture;       /* gamepadLock: 1 UI owns input, 2 wait for release, 0 game */
static _Atomic int settingsShortcutEnabled;
static int settingsShortcutDown, settingsShortcutPending;

/* The controller's way to the settings panel (user 2026-10-29): L3 and R3 pressed together by default, or one button the
   player picks in the key bindings (the touchpad, say), kept in controller-settings.txt. It opens or closes the panel as
   soon as it is all down; from there the game gets no controller input until everything is released. A picked button
   drives nothing in the game. (Until 2026-10-29: L2 + R2 + L3 held for a second, user 2026-10-27.) */
#define SETTINGS_CHORD ((1ull << 4) | (1ull << 5))
static _Atomic int settingsButton = -1;   /* physical button (controller_map.h), -1 the chord */
static uint64_t settings_trigger(void) { int b = atomic_load(&settingsButton); return b >= 0 ? 1ull << b : SETTINGS_CHORD; }
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
static __weak GCController *rawLeftController, *rawRightController;
static SwitchLeftFallback rawLeft, rawRight;
static void (*captureHook)(void);
static int captureDown, captureSeen;   /* the capture button as the reports say, and as refresh_gamepads last saw it */
/* The screenshot button (controller_map.h CONTROLLER_SCREENSHOT_*, user 2026-11-11): the physical button whose 3-second hold
   takes the capture button's picture, -1 none; chosen in the key bindings and kept in controller-screenshot.txt. */
static _Atomic int screenshotButton = CONTROLLER_SCREENSHOT_DEFAULT;
static ControllerHold screenshotHold;   /* gamepadLock */
static void gamepad_options_gesture(GCExtendedGamepad *pad);
static void gamepad_gestures_changed(void);

/* The capture button (the window's screenshot, runtime/screenshot.h): macOS's Share button on a controller GameController
   reports, or bit 5 of a Switch controller's 0x30 HID report (a Pro Controller's square button below the D-pad). The
   Switch report arrives on the main run loop, and the GameController value from a value-changed handler; both only set a
   flag, and the refresh that follows calls the hook once per press. A macOS shortcut already bound to the button keeps
   it (System Settings > Game Center): the button then takes no picture. */
void runtime_capture_set_hook(void (*hook)(void)) { captureHook = hook; }
/* The input file's "capture" (runtime/input.c): the capture button pressed once, for automated tests of screenshots. */
void runtime_capture_press(void) { if (captureHook) captureHook(); }
/* A Switch 0x30 report: 1 while its button 5 is down, which arrives every few milliseconds while it is held. */
void runtime_gamepad_switch_capture(void *controller, int pressed) {
    if (!controller) captureDown = 0;
    else if (pressed && !captureDown) { captureDown = 1; refresh_gamepads(); }   /* the refresh calls the hook once */
    else if (!pressed && captureDown) captureDown = 0;
}
#if TARGET_OS_OSX
static _Atomic int gamepadBackground, gamepadFocused = 1;
#endif

/* Called on the main run loop by the already-open Switch HID stream (motion or Betop NS stick fallback). */
void runtime_gamepad_switch_left_config(void *source, const SwitchStickCal *cal, int immediate) {
    GCController *controller = (__bridge GCController *)source;
    if (rawLeftController != controller) rawLeft = (SwitchLeftFallback){0};
    rawLeftController = controller;
    switch_left_configure(&rawLeft, cal, immediate);
}
void runtime_gamepad_switch_right_config(void *source, const SwitchStickCal *cal, int immediate) {
    GCController *controller = (__bridge GCController *)source;
    if (rawRightController != controller) rawRight = (SwitchLeftFallback){0};
    rawRightController = controller;
    switch_left_configure(&rawRight, cal, immediate);
}
void runtime_gamepad_switch_left(void *source, int x, int y) {
    GCController *controller = (__bridge GCController *)source;
    if (rawLeftController != controller) rawLeft = (SwitchLeftFallback){0};
    rawLeftController = controller;
    switch_left_feed(&rawLeft, x, y, runtime_now_ns());
    refresh_gamepads();
}
void runtime_gamepad_switch_right(void *source, int x, int y) {
    GCController *controller = (__bridge GCController *)source;
    if (rawRightController != controller) rawRight = (SwitchLeftFallback){0};
    rawRightController = controller;
    switch_left_feed(&rawRight, x, y, runtime_now_ns());
    refresh_gamepads();
}
/* A controller that goes away takes its HID stick fallback with it (games/odyssey). */
static void gamepad_forget_raw(GCController *controller) {
    if (rawLeftController == controller) { rawLeftController = nil; rawLeft = (SwitchLeftFallback){0}; }
    if (rawRightController == controller) { rawRightController = nil; rawRight = (SwitchLeftFallback){0}; }
}

static int32_t stick_axis(float value) {
    const float deadzone = 0.12f;
    float magnitude = fabsf(value);
    if (magnitude <= deadzone) return 0;
    if (magnitude > 1.0f) magnitude = 1.0f;
    int32_t result = (int32_t)((magnitude - deadzone) / (1.0f - deadzone) * 32767.0f);
    return value < 0 ? -result : result;
}

static int32_t stronger_axis(int32_t a, int32_t b) {
    return abs(b) > abs(a) ? b : a;
}

#include "gamepad_settings.h"   /* setting_read / setting_write, runtime_setting_word / _save / _remove */

/* The player's button layout (controller_map.h): TOTK_CONTROLS names the file (the launcher: <data>/controls.txt, which
   the settings panel's key bindings write). No file: the default layout. A file that does not parse leaves the default
   and names its bad line in the log (games/acnh; the panel writes it again at the next change, and games/totk's launcher
   names the line in an alert before the game starts). The game's own lines (gamepad_game_layout_text) come from the
   same file. */
static void load_controller_map(void) {
    controller_map_default(controllerMap, controllerModes);
    const char *path = getenv("TOTK_CONTROLS");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "[totk-input] controller layout: default\n"); gamepad_game_layout(controllerMap); return; }
    char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); int whole = feof(f); fclose(f);
    if (!whole) { fprintf(stderr, "[totk-input] %s: too large; using the default controller layout\n", path); gamepad_game_layout(controllerMap); return; }
    text[n] = 0;
    const char *error = NULL; int line = controller_map_parse(text, controllerMap, controllerModes, &error);
    if (line) fprintf(stderr, "[totk-input] %s line %d: %s; using the default controller layout\n", path, line, error);
    else {
        uint16_t defaults[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaults, defaultModes);
        int changed = 0; for (int i = 0; i < CONTROLLER_BUTTONS; i++) changed += controllerMap[i] != defaults[i];
        fprintf(stderr, "[totk-input] controller layout: %s (%d buttons changed)\n", path, changed);
    }
    if ((line = gamepad_game_layout_text(text, &error))) fprintf(stderr, "[totk-input] %s line %d: %s; ignored\n", path, line, error);
    gamepad_game_layout(controllerMap);
}

/* macOS normally routes controllers to the frontmost app. Opting in also lets a foreground app receive the same
   controller, so this is a saved, default-off choice rather than an unconditional policy. Main thread only. */
int runtime_gamepad_background_enabled(void) {
#if TARGET_OS_OSX
    return atomic_load(&gamepadBackground);
#else
    return 0;
#endif
}
#if TARGET_OS_OSX
static void gamepad_background_apply(int on) {
    atomic_store(&gamepadBackground, !!on);
    GCController.shouldMonitorBackgroundEvents = !!on;
    if (!on && !atomic_load(&gamepadFocused)) runtime_gamepad_clear();
    else refresh_gamepads();
}
static void load_gamepad_background(void) {
    char path[4096], word[64] = {0}, value[16], extra;
    int on = 0;
    if (setting_read("controller-background.txt", word, sizeof word, path, sizeof path)) {
        if (sscanf(word, "%15s %c", value, &extra) == 1 && (!strcmp(value, "on") || !strcmp(value, "off")))
            on = !strcmp(value, "on");
        else fprintf(stderr, "[totk-input] %s: expected on or off; background controller input stays off\n", path);
    }
    gamepad_background_apply(on);
}
#endif
void runtime_gamepad_background_set(int on) {
#if TARGET_OS_OSX
    gamepad_background_apply(on);
    setting_write("controller-background.txt", on ? "on" : "off", "background controller input");
#else
    (void)on;
#endif
}
void runtime_gamepad_focus(int on) {
#if TARGET_OS_OSX
    atomic_store(&gamepadFocused, !!on);
    uint64_t trace = gamepad_game_trace_row();
    if (trace) fprintf(stderr, GAMEPAD_TRACE_TAG " %llu t=%llu focus=%d background=%d system_background=%d\n",
        (unsigned long long)trace, (unsigned long long)runtime_now_ns(), !!on, runtime_gamepad_background_enabled(), GCController.shouldMonitorBackgroundEvents);
    if (!on && !runtime_gamepad_background_enabled()) runtime_gamepad_clear();
    else refresh_gamepads();
#else
    (void)on;
#endif
}

/* The settings panel's key bindings (main thread, as refresh_gamepads): the layout in force, a new one (saved in the
   TOTK_CONTROLS file with only what differs from the default), or the default (the file removed). */
void runtime_controller_map_get(uint16_t *map, uint8_t *modes) {
    memcpy(map, controllerMap, sizeof controllerMap); memcpy(modes, controllerModes, sizeof controllerModes);
}
static void controller_map_changed(void) {
    gamepad_game_layout(controllerMap);
    gamepad_mapping_reset();
    gamepad_gestures_changed();
    refresh_gamepads();
}
void runtime_controller_map_set(const uint16_t *map, const uint8_t *modes) {
    memcpy(controllerMap, map, sizeof controllerMap); memcpy(controllerModes, modes, sizeof controllerModes);
    controller_map_changed();
    const char *path = getenv("TOTK_CONTROLS"); char text[8192];
    if (path && *path && controller_map_format(controllerMap, controllerModes, text, sizeof text) > 0) file_write(path, text, "controller layout");
    fprintf(stderr, "[totk-input] controller layout: changed in the settings panel\n");
}
void runtime_controller_map_reset(void) {
    controller_map_default(controllerMap, controllerModes);
    controller_map_changed();
    const char *path = getenv("TOTK_CONTROLS");
    if (path && *path) unlink(path);
    fprintf(stderr, "[totk-input] controller layout: default (reset)\n");
}

/* Controller type (gamepad_layout.h GAMEPAD_TYPE_*), in the Mac settings panel, in controller-type.txt. */
static _Atomic int controllerType = GAMEPAD_TYPE_AUTO;

static void load_controller_type(void) {
    char path[4096], word[64] = {0};
    if (!setting_read("controller-type.txt", word, sizeof word, path, sizeof path)) return;
    int type = gamepad_type_parse(word);
    if (type < 0) { fprintf(stderr, "[totk-input] %s: not a controller type (auto, xbox, playstation, switch); using auto\n", path); return; }
    atomic_store(&controllerType, type);
    fprintf(stderr, "[totk-input] controller type: %s (%s)\n", gamepad_type_name(type), path);
}

static void save_controller_type(int type) { setting_write("controller-type.txt", gamepad_type_name(type), "controller type"); }

static int face_by_position(GCController *controller) {
    return gamepad_type_face_by_position(atomic_load(&controllerType), controller.productCategory.UTF8String, controller.vendorName.UTF8String);
}

/* Apple's Options recognizer waits to distinguish a short press from a screenshot long press before forwarding input
   (GCControllerElement.h). Physical "view" is Options/Create on DualSense and maps to Switch Minus by default: that
   wait delays every Purah Pad page although keyboard Minus is immediate. Request direct input while the button drives
   a game action or the settings panel; leave its system gestures available when the player unmaps it. */
static void gamepad_options_gesture(GCExtendedGamepad *pad) {
    GCControllerButtonInput *button = pad.buttonOptions;
    if (!button) return;
    int used = controllerMap[11] != 0 || atomic_load(&settingsButton) == 11;
    GCSystemGestureState wanted = used ? GCSystemGestureStateDisabled : GCSystemGestureStateEnabled;
    if (button.preferredSystemGestureState == wanted) return;
    button.preferredSystemGestureState = wanted;
    fprintf(stderr, "[totk-input] Options/View: %s\n", used ? "requested direct input for the game/panel" : "system gestures available (unmapped)");
}
static void gamepad_gestures_changed(void) {
    for (GCController *controller in GCController.controllers) gamepad_options_gesture(controller.extendedGamepad);
}

/* The physical buttons held, bit i = controller_physical_names[i]; the face buttons by where they sit
   (gamepad_face_positions), so one layout file means the same buttons on every controller. */
static uint64_t gamepad_buttons(GCExtendedGamepad *pad, int by_position) {
    uint64_t buttons = gamepad_face_positions(pad.buttonA.isPressed, pad.buttonB.isPressed, pad.buttonX.isPressed, pad.buttonY.isPressed, by_position);
    if (pad.leftThumbstickButton.isPressed) buttons |= 1ULL << 4;
    if (pad.rightThumbstickButton.isPressed) buttons |= 1ULL << 5;
    if (pad.leftShoulder.isPressed) buttons |= 1ULL << 6;
    if (pad.rightShoulder.isPressed) buttons |= 1ULL << 7;
    /* ZL / ZR: GameController counts a DualSense trigger as pressed from its first step of travel (value > 0, 1/255) and
       released at 0, with no chatter in between and nothing from a finger resting on it (measured over Bluetooth on macOS
       27.2, 2026-10-27), so a threshold of our own could only make the bow draw later. */
    if (pad.leftTrigger.isPressed) buttons |= 1ULL << 8;
    if (pad.rightTrigger.isPressed) buttons |= 1ULL << 9;
    if (pad.buttonMenu.isPressed) buttons |= 1ULL << 10;
    if (pad.buttonOptions.isPressed) buttons |= 1ULL << 11;
    if (pad.dpad.left.isPressed) buttons |= 1ULL << 12;
    if (pad.dpad.up.isPressed) buttons |= 1ULL << 13;
    if (pad.dpad.right.isPressed) buttons |= 1ULL << 14;
    if (pad.dpad.down.isPressed) buttons |= 1ULL << 15;
    if ([pad isKindOfClass:GCDualSenseGamepad.class] && ((GCDualSenseGamepad *)pad).touchpadButton.isPressed) buttons |= 1ULL << 16;
    if ([pad isKindOfClass:GCDualShockGamepad.class] && ((GCDualShockGamepad *)pad).touchpadButton.isPressed) buttons |= 1ULL << 16;
    return buttons;
}

#include "gamepad_hid_pad.h"   /* hidPad: the generic HID pads (macOS) */
#include "gamepad_mouse.h"     /* mouse mode, where the game has it */

/* The settings panel's button (key bindings): -1 the L3 + R3 chord, else the physical button, saved in
   controller-settings.txt by its controls.txt name; -1 removes the file. */
static void load_settings_button(void) {
    char path[4096], word[32] = {0};
    if (!setting_read("controller-settings.txt", word, sizeof word, path, sizeof path)) return;
    word[strcspn(word, " \t\r\n")] = 0;
    for (int i = 0; i <= CONTROLLER_SWITCH_BUTTONS; i++)   /* the controller's buttons and the touchpad, not the mouse's */
        if (!strcasecmp(word, controller_physical_names[i])) {
            atomic_store(&settingsButton, i);
            fprintf(stderr, "[totk-input] settings panel button: %s (%s)\n", controller_physical_names[i], path);
            return;
        }
    fprintf(stderr, "[totk-input] %s: not a controller button; the settings panel stays on L3 + R3\n", path);
}
int runtime_gamepad_settings_button(void) { return atomic_load(&settingsButton); }

/* The screenshot button (key bindings): CONTROLLER_SCREENSHOT_DEFAULT (the touchpad) without a file; the file names another
   by its controls.txt name, or "none". */
static void load_screenshot_button(void) {
    char path[4096], word[32] = {0};
    if (!setting_read("controller-screenshot.txt", word, sizeof word, path, sizeof path)) return;
    word[strcspn(word, " \t\r\n")] = 0;
    if (!strcasecmp(word, "none")) {
        atomic_store(&screenshotButton, -1);
        fprintf(stderr, "[totk-input] screenshot button: none (%s)\n", path);
        return;
    }
    for (int i = 0; i <= CONTROLLER_SWITCH_BUTTONS; i++)   /* the controller's buttons and the touchpad, not the mouse's */
        if (!strcasecmp(word, controller_physical_names[i])) {
            atomic_store(&screenshotButton, i);
            fprintf(stderr, "[totk-input] screenshot button: %s held %d s (%s)\n", controller_physical_names[i], CONTROLLER_SCREENSHOT_HOLD_MS / 1000, path);
            return;
        }
    fprintf(stderr, "[totk-input] %s: not a controller button; the screenshot stays on the %s\n", path,
            controller_physical_names[CONTROLLER_SCREENSHOT_DEFAULT]);
}
int runtime_gamepad_screenshot_button(void) { return atomic_load(&screenshotButton); }
void runtime_gamepad_screenshot_button_set(int physical) {
    if (physical > CONTROLLER_SWITCH_BUTTONS) return;
    if (physical < 0) physical = -1;
    atomic_store(&screenshotButton, physical);
    pthread_mutex_lock(&gamepadLock);
    screenshotHold = (ControllerHold){0};   /* a hold of the old button does not carry over */
    pthread_mutex_unlock(&gamepadLock);
    if (physical == CONTROLLER_SCREENSHOT_DEFAULT) runtime_setting_remove("controller-screenshot.txt");
    else setting_write("controller-screenshot.txt", physical < 0 ? "none" : controller_physical_names[physical], "screenshot button");
    fprintf(stderr, "[totk-input] screenshot button: %s\n", physical < 0 ? "none" : controller_physical_names[physical]);
}
void runtime_gamepad_settings_button_set(int physical) {
    if (physical > CONTROLLER_SWITCH_BUTTONS) return;
    if (physical < 0) physical = -1;
    atomic_store(&settingsButton, physical);
    pthread_mutex_lock(&gamepadLock);
    settingsShortcutDown = settings_chord_held();
    pthread_mutex_unlock(&gamepadLock);
    if (physical < 0) runtime_setting_remove("controller-settings.txt");
    else setting_write("controller-settings.txt", controller_physical_names[physical], "settings panel button");
    fprintf(stderr, "[totk-input] settings panel button: %s\n", physical < 0 ? "L3 + R3" : controller_physical_names[physical]);
    gamepad_gestures_changed();
    refresh_gamepads();
}
#if TARGET_OS_OSX
/* macOS counts the keyboard and the mouse as the user's activity, not a game controller: a player using only the
   controller got the screen saver in the middle of a fight (2026-10-28, in Super Smash Bros. Ultimate; pmset listed no display-sleep assertion from
   the game). While the controller's state keeps changing, the game holds a PreventUserIdleDisplaySleep assertion; it is
   let go once the controller has been still for the Mac's own screen saver time (System Settings, read from
   com.apple.screensaver idleTime of this host; 300 s when none is set), so the screen saver then comes as it would after
   the last key press, and a Mac left alone still sleeps. A stick resting in its dead zone reads 0 and changes nothing. */
enum { AWAKE_DEFAULT_IDLE_S = 300, AWAKE_CHECK_S = 15 };
static _Atomic uint64_t awakeLastInput;   /* ns */
static _Atomic int awakeWanted;           /* set by the first input, cleared when the assertion is let go */
static IOPMAssertionID awakeAssertion;    /* main queue */
static int awakeHeld;                     /* main queue */
static int awakeIdleS;                    /* main queue: the screen saver time read when the assertion was taken */
static int awake_screen_saver_s(void) {
    CFPropertyListRef v = CFPreferencesCopyValue(CFSTR("idleTime"), CFSTR("com.apple.screensaver"), kCFPreferencesCurrentUser, kCFPreferencesCurrentHost);
    int s = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &s);
    if (v) CFRelease(v);
    return s > 0 ? s : AWAKE_DEFAULT_IDLE_S;
}
static void awake_check(void) {
    if (runtime_now_ns() - atomic_load(&awakeLastInput) < (uint64_t)awakeIdleS * 1000000000ull) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)AWAKE_CHECK_S * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ awake_check(); });
        return;
    }
    if (awakeHeld) { IOPMAssertionRelease(awakeAssertion); awakeHeld = 0; }
    atomic_store(&awakeWanted, 0);
    fprintf(stderr, "[totk-input] display may sleep again: no controller input for %d s\n", awakeIdleS);
}
static void awake_take(void) {
    awakeIdleS = awake_screen_saver_s();
    IOReturn r = IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                             CFSTR("MacNativeGames: controller in use"), &awakeAssertion);
    awakeHeld = r == kIOReturnSuccess;
    if (awakeHeld) fprintf(stderr, "[totk-input] display kept awake while the controller is in use (let go after %d s without input)\n", awakeIdleS);
    else fprintf(stderr, "[totk-input] display-sleep assertion failed: 0x%08x\n", r);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)AWAKE_CHECK_S * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ awake_check(); });
}
static void awake_note_input(void) {   /* any thread */
    atomic_store(&awakeLastInput, runtime_now_ns());
    if (!atomic_exchange(&awakeWanted, 1)) dispatch_async(dispatch_get_main_queue(), ^{ awake_take(); });
}
#else
static void awake_note_input(void) {}
#endif

/* The settings panel (key 1, frontend.m): auto, Xbox, PlayStation or Switch; the held buttons are read again at once. */
int runtime_gamepad_type_set(int type) {
    if (type < GAMEPAD_TYPE_AUTO || type >= GAMEPAD_TYPE_COUNT) return atomic_load(&controllerType);
    if (type == atomic_load(&controllerType)) return type;
    atomic_store(&controllerType, type);
    save_controller_type(type);
    fprintf(stderr, "[totk-input] controller type: %s (settings)\n", gamepad_type_name(type));
    dispatch_async(dispatch_get_main_queue(), ^{
        for (GCController *controller in GCController.controllers)
            if (controller.extendedGamepad)
                fprintf(stderr, "[totk-input] %s: face buttons by %s\n", (controller.vendorName ?: @"unknown").UTF8String,
                        face_by_position(controller) ? "position" : "label");
        refresh_gamepads();
    });
    return type;
}

/* For the HUD (main thread): the picked type; *detected = what auto takes the first connected controller for, -1 none. */
int runtime_gamepad_type(int *detected) {
    int found = -1;
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad) { found = gamepad_type_detect(controller.productCategory.UTF8String, controller.vendorName.UTF8String); break; }
#if TARGET_OS_OSX
    if (found < 0 && hidPad.present) found = GAMEPAD_TYPE_SWITCH;   /* the generic HID pads are Switch-style */
#endif
    if (detected) *detected = found;
    return atomic_load(&controllerType);
}

/* A Switch controller's battery from its own HID reports (gamepad_feedback.m): GameController reports a Joy-Con's
   level as 0 while it is charged. */
static _Atomic int hidBatteryPercent = -1, hidBatteryLeftPercent = -1;
void runtime_gamepad_hid_battery(int left, int percent) { atomic_store(left ? &hidBatteryLeftPercent : &hidBatteryPercent, percent); }
/* A paired Joy-Con set's left one, -1 none or unknown; runtime_gamepad_battery_percent is then the right one. */
int runtime_gamepad_battery_left_percent(void) { return atomic_load(&hidBatteryPercent) >= 0 ? atomic_load(&hidBatteryLeftPercent) : -1; }

/* For the HUD (main thread): the first controller's battery as a whole percent, -1 none or unknown. */
int runtime_gamepad_battery_percent(void) {
    int hid = atomic_load(&hidBatteryPercent);
    if (hid >= 0) return hid;
    if ((hid = atomic_load(&hidBatteryLeftPercent)) >= 0) return hid;   /* a left Joy-Con alone */
    for (GCController *controller in GCController.controllers)
        if (controller.battery && controller.battery.batteryState != GCDeviceBatteryStateUnknown)
            return (int)lroundf(controller.battery.batteryLevel * 100.0f);
    return -1;
}

/* Hardware keyboards (Magic Keyboard, Smart Keyboard, Bluetooth) feed the same virtual controller the Mac keyboard
   drives, with the macOS input.c key layout. iOS only: the Mac already gets the keyboard through the window
   (frontend.m keyDown/keyUp), and a second source there would fight it. Strictly additive on iOS: the GCController
   path below is untouched, the keyboard can be switched off with TOTK_KEYBOARD=off, and touch / a controller always
   remain available. */
#if TARGET_OS_IPHONE
static GCKeyboard *keyboardPad;
/* While UIKit's text field is up, the same physical keys type into it; they must not also drive the game. */
static volatile int keyboardPaused;
void runtime_keyboard_capture(int on) {
    if (on && !keyboardPaused) for (unsigned mac = 0; mac < 128; mac++) runtime_input_key(mac, 0);
    keyboardPaused = on;
}

static int mac_key_code(GCKeyCode code) {
    /* GCKeyCode values are extern const objects, not enum constants, so this cannot be a switch. */
    if (code == GCKeyCodeKeyA) return 0;           /* WASD: left stick, as on the Mac */
    if (code == GCKeyCodeKeyS) return 1;
    if (code == GCKeyCodeKeyD) return 2;
    if (code == GCKeyCodeKeyW) return 13;
    if (code == GCKeyCodeLeftArrow) return 123;    /* arrows: right stick */
    if (code == GCKeyCodeRightArrow) return 124;
    if (code == GCKeyCodeDownArrow) return 125;
    if (code == GCKeyCodeUpArrow) return 126;
    if (code == GCKeyCodeKeyF) return 3;           /* F/T/H/G: D-pad */
    if (code == GCKeyCodeKeyT) return 17;
    if (code == GCKeyCodeKeyH) return 4;
    if (code == GCKeyCodeKeyG) return 5;
    if (code == GCKeyCodeKeyJ) return 38;          /* J/K/I/U: face buttons */
    if (code == GCKeyCodeKeyK) return 40;
    if (code == GCKeyCodeKeyI) return 34;
    if (code == GCKeyCodeKeyU) return 32;
    if (code == GCKeyCodeKeyZ) return 6;           /* Z/C: stick clicks (runtime/input.c's bits 4/5) */
    if (code == GCKeyCodeKeyC) return 8;
    if (code == GCKeyCodeKeyQ) return 12;          /* Q/E: L/R */
    if (code == GCKeyCodeKeyE) return 14;
    if (code == GCKeyCodeLeftAlt || code == GCKeyCodeRightAlt) return 58;    /* Option: ZL */
    if (code == GCKeyCodeLeftShift || code == GCKeyCodeRightShift) return 56; /* Shift: ZR */
    if (code == GCKeyCodeReturnOrEnter) return 36;  /* Return: + */
    if (code == GCKeyCodeTab) return 48;            /* Tab / Esc: - */
    if (code == GCKeyCodeEscape) return 53;
    if (code == GCKeyCodeSpacebar) return 49;       /* Space: B, as on the Mac */
    return -1;
}
static void attach_keyboard(GCKeyboard *keyboard) {
    if (!keyboard) return;
    const char *mode = getenv("TOTK_KEYBOARD");
    if (mode && (!strcmp(mode, "off") || !strcmp(mode, "0"))) return;   /* "0": the Settings switch (main.m), as touch_pad.m reads it */
    keyboardPad = keyboard;
    keyboard.keyboardInput.keyChangedHandler = ^(GCKeyboardInput *input, GCControllerButtonInput *key, GCKeyCode code, BOOL pressed) {
        (void)input; (void)key;
        if (keyboardPaused) return;
        int mac = mac_key_code(code);
        if (mac >= 0) runtime_input_key((unsigned)mac, pressed);
    };
    fprintf(stderr, "[totk-input] hardware keyboard connected: %s\n", (keyboard.vendorName ?: @"keyboard").UTF8String);
}
static void detach_keyboard(GCKeyboard *keyboard) {
    if (keyboardPad == keyboard) {
        keyboard.keyboardInput.keyChangedHandler = nil;
        keyboardPad = nil;
    }
    /* Release every key on any keyboard disconnect: a held W/Space must not keep the game walking after the keyboard
       sleeps or is detached, and the notification's object is not guaranteed to be the one attached. */
    for (unsigned mac = 0; mac < 128; mac++) runtime_input_key(mac, 0);
    fprintf(stderr, "[totk-input] hardware keyboard disconnected\n");
}
#endif

/* What every gamepad.m does with a controller GameController offers, besides its own wiring: the Options gesture, the
   log line and the capture button. */
static void gamepad_attach_note(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    gamepad_options_gesture(pad);
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s), face buttons by %s\n",
            (controller.vendorName ?: @"unknown").UTF8String,
            (controller.productCategory ?: @"unknown").UTF8String, face_by_position(controller) ? "position" : "label");
    /* The capture button takes this game's picture (runtime_capture_set_hook). macOS keeps a Share button for its own
       gesture -- a screenshot to the Desktop -- while a system gesture has it; asked off, the button reaches the game.
       A shortcut the player bound to it in System Settings > Game Center keeps it, and then it takes no picture. */
    GCControllerButtonInput *capture = pad.buttons[GCInputButtonShare];
    if (capture) {
        BOOL reserved = capture.isBoundToSystemGesture;
        capture.preferredSystemGestureState = GCSystemGestureStateDisabled;
        fprintf(stderr, "[totk-input] %s: the capture button (%s) saves a screenshot%s\n", (controller.vendorName ?: @"controller").UTF8String,
                capture.localizedName.UTF8String ?: "share", reserved ? "; macOS had a system gesture on it, asked off" : "");
    }
}

/* The GameController part of the start-up (games/oot, 2026-11-07). Its first call into the framework is an XPC handshake
   with gamecontrollerd, which never answers while a process that died with a wedged GPU context is stuck exiting (every
   new game hung here for 30 minutes, the Mac had to be rebooted). So the handshake runs on a background thread, the game
   starts without controllers, and this runs on the main queue once the service has answered; a red line names the wait
   after 5 s. */
static void gamepad_start_gc(void) {
#if TARGET_OS_OSX
    load_gamepad_background();
#endif
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { attach_gamepad(notification.object); }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) {
                       GCController *controller = notification.object;
                       fprintf(stderr, "[totk-input] native gamepad disconnected: %s\n",
                               (controller.vendorName ?: @"unknown").UTF8String);
                       detach_gamepad(controller);
                   }];
    for (GCController *controller in GCController.controllers) attach_gamepad(controller);
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
#if TARGET_OS_IPHONE
    [center addObserverForName:GCKeyboardDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { attach_keyboard(notification.object); }];
    [center addObserverForName:GCKeyboardDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { detach_keyboard(notification.object); }];
    attach_keyboard(GCKeyboard.coalescedKeyboard);
#endif
}

void runtime_gamepad_start(void) {
    if (started) return;
    started = YES;
    load_controller_map();
    load_controller_type();
    load_settings_button();
    load_screenshot_button();
    mouse_start();
    dsu_start();   /* subscribe without taking motion away from native controllers */
    hid_pad_start();
    dispatch_queue_t bg = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    __block atomic_int answered = 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), bg, ^{
        if (!atomic_load(&answered))
            fprintf(stderr, "[totk-input] 【红】GameController 服务 5 秒没有回应（gamecontrollerd 卡住？）：游戏先不带手柄运行，服务回应后自动接上\n");
    });
    dispatch_async(bg, ^{
        (void)GCController.controllers;   // the handshake with gamecontrollerd; blocks only this thread when the service is wedged
        atomic_store(&answered, 1);
        dispatch_async(dispatch_get_main_queue(), ^{ gamepad_start_gc(); });
    });
}

/* The settings panel's controller shortcut (settings_controller_start: on while the window lives; off while a box waits
   for a controller button, which may be the shortcut's). */
void runtime_gamepad_settings_enable(int enabled) {
    pthread_mutex_lock(&gamepadLock);
    settingsShortcutEnabled = !!enabled;
    settingsShortcutPending = 0;
    settingsShortcutDown = settings_chord_held();   /* one held through this waits for a new press */
    pthread_mutex_unlock(&gamepadLock);
}

int runtime_gamepad_settings_requested(void) {
    pthread_mutex_lock(&gamepadLock);
    int requested = settingsShortcutPending; settingsShortcutPending = 0;
    pthread_mutex_unlock(&gamepadLock);
    return requested;
}
int runtime_gamepad_game_input(void) { return !atomic_load(&textCapture); }   /* the bow thread (games/totk feedback_game.h) */
#endif
