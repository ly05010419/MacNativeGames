/* Native macOS controllers feed the same FullKey state as the keyboard. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include "runtime.h"
#include "gamepad_layout.h"   // face buttons by position (PlayStation, Xbox) or label (Nintendo)
#include "controller_map.h"   // the player's button layout (controls.txt)

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState gamepadState;
static uint64_t pressedEdges;
/* The settings panel (key 1, runtime/settings_panel.h) and the text entry bar read the controller here instead of the
   game: textPad and textEdges are the physical buttons (controller_map.h order) and the sticks. */
static RuntimeGamepadState textPad;
static uint64_t textEdges;
static _Atomic int textCapture;       /* gamepadLock: 1 the panel owns input, 2 wait for release, 0 the game */
static BOOL started;
static _Atomic int settingsShortcutEnabled;
static int settingsShortcutDown, settingsShortcutPending;
/* The controller's way to the settings panel (as games/totk, reported in play, PR #1305): L3 and R3 pressed together by default, or
   one button the player picks in the key bindings (the touchpad, say), kept in controller-settings.txt. It opens or
   closes the panel as soon as it is all down; from there the game gets no controller input until everything is
   released. A picked button drives nothing in the game. */
#define SETTINGS_CHORD ((1ull << 4) | (1ull << 5))
static _Atomic int settingsButton = -1;   /* physical button (controller_map.h), -1 the chord */
static uint64_t settings_trigger(void) { int b = atomic_load(&settingsButton); return b >= 0 ? 1ull << b : SETTINGS_CHORD; }
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
static uint64_t physicalPrev;
static uint64_t tapUntil[CONTROLLER_BUTTONS];   /* ns, for tap */
static int toggleOn[CONTROLLER_BUTTONS];        /* for toggle */
static int toggleMoved[CONTROLLER_BUTTONS];     /* the stick moved since the toggle went on */
static void refresh_gamepads(void);

/* The player's button layout (controller_map.h): TOTK_CONTROLS names the file (the launcher: <data>/controls.txt, which
   the settings panel's key bindings write). No file: the default layout. A file that does not parse leaves the default
   and names its bad line in the log (the panel writes it again at the next change). */
static void load_controller_map(void) {
    controller_map_default(controllerMap, controllerModes);
    const char *path = getenv("TOTK_CONTROLS");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "[totk-input] controller layout: default\n"); return; }
    char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); int whole = feof(f); fclose(f);
    if (!whole) { fprintf(stderr, "[totk-input] %s: too large; using the default controller layout\n", path); return; }
    text[n] = 0;
    const char *error = NULL; int line = controller_map_parse(text, controllerMap, controllerModes, &error);
    if (line) { fprintf(stderr, "[totk-input] %s line %d: %s; using the default controller layout\n", path, line, error); return; }
    uint16_t defaults[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaults, defaultModes);
    int changed = 0; for (int i = 0; i < CONTROLLER_BUTTONS; i++) changed += controllerMap[i] != defaults[i];
    fprintf(stderr, "[totk-input] controller layout: %s (%d buttons changed)\n", path, changed);
}

/* Settings of the panel, one word each in a file next to controls.txt (TOTK_CONTROLS; a run without it, such as a test,
   neither reads nor writes them), so they hold for the next launch. */
static int setting_path(const char *name, char *out, size_t size) {
    const char *controls = getenv("TOTK_CONTROLS");
    if (!controls || !*controls) return 0;
    const char *slash = strrchr(controls, '/');
    int dir = slash ? (int)(slash - controls) : 0;
    int n = slash ? snprintf(out, size, "%.*s/%s", dir, controls, name) : snprintf(out, size, "%s", name);
    return n > 0 && (size_t)n < size;
}

static int setting_read(const char *name, char *word, size_t size, char *path, size_t path_size) {   /* 1: read */
    if (!setting_path(name, path, path_size)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;   /* never changed: the default */
    size_t n = fread(word, 1, size - 1, f);
    fclose(f);
    word[n] = 0;
    return 1;
}

static void file_write(const char *path, const char *text, const char *what) {   /* whole or not at all */
    char temp[4200];
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    int ok = f && fprintf(f, "%s", text) >= 0;
    if (f && fclose(f)) ok = 0;
    if (ok && rename(temp, path)) ok = 0;
    if (!ok) { fprintf(stderr, "[totk-input] could not save the %s to %s; it holds until the game closes\n", what, path); unlink(temp); }
}

static void setting_write(const char *name, const char *word, const char *what) {
    char path[4096], text[16384];
    if (!setting_path(name, path, sizeof path)) return;
    size_t n = strlen(word);
    snprintf(text, sizeof text, "%s%s", word, n && word[n - 1] == '\n' ? "" : "\n");
    file_write(path, text, what);
}

/* The same files for the rest of the runtime: 1 with the file's text in word as written (the caller drops the blanks
   around the word), 0 when there is no such file or no settings folder (a run without TOTK_CONTROLS). */
int runtime_setting_word(const char *name, char *word, size_t size) {
    char path[4096];
    return setting_read(name, word, size, path, sizeof path);
}
void runtime_setting_save(const char *name, const char *word, const char *what) { setting_write(name, word, what); }
/* ... or removes it: the default again (the settings panel's resets). */
void runtime_setting_remove(const char *name) {
    char path[4096];
    if (setting_path(name, path, sizeof path)) unlink(path);
}

/* The settings panel's key bindings (main thread, as refresh_gamepads): the layout in force, a new one (saved in the
   TOTK_CONTROLS file with only what differs from the default), or the default (the file removed). */
void runtime_controller_map_get(uint16_t *map, uint8_t *modes) {
    memcpy(map, controllerMap, sizeof controllerMap); memcpy(modes, controllerModes, sizeof controllerModes);
}
static void controller_map_changed(void) {
    physicalPrev = 0;
    memset(tapUntil, 0, sizeof tapUntil); memset(toggleOn, 0, sizeof toggleOn); memset(toggleMoved, 0, sizeof toggleMoved);
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

/* Controller type (gamepad_layout.h GAMEPAD_TYPE_*), in the settings panel, in controller-type.txt. */
static _Atomic int controllerType = GAMEPAD_TYPE_AUTO;

static void load_controller_type(void) {
    char path[4096], word[64] = {0};
    if (!setting_read("controller-type.txt", word, sizeof word, path, sizeof path)) return;
    int type = gamepad_type_parse(word);
    if (type < 0) { fprintf(stderr, "[totk-input] %s: not a controller type (auto, xbox, playstation, switch); using auto\n", path); return; }
    atomic_store(&controllerType, type);
    fprintf(stderr, "[totk-input] controller type: %s (%s)\n", gamepad_type_name(type), path);
}

/* The settings panel's button (key bindings): -1 the L3 + R3 chord, else the physical button, saved in
   controller-settings.txt by its controls.txt name; -1 removes the file. */
static void load_settings_button(void) {
    char path[4096], word[32] = {0};
    if (!setting_read("controller-settings.txt", word, sizeof word, path, sizeof path)) return;
    word[strcspn(word, " \t\r\n")] = 0;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++)
        if (!strcasecmp(word, controller_physical_names[i])) {
            atomic_store(&settingsButton, i);
            fprintf(stderr, "[totk-input] settings panel button: %s (%s)\n", controller_physical_names[i], path);
            return;
        }
    fprintf(stderr, "[totk-input] %s: not a controller button; the settings panel stays on L3 + R3\n", path);
}
int runtime_gamepad_settings_button(void) { return atomic_load(&settingsButton); }
void runtime_gamepad_settings_button_set(int physical) {
    if (physical >= CONTROLLER_BUTTONS) return;
    if (physical < 0) physical = -1;
    atomic_store(&settingsButton, physical);
    pthread_mutex_lock(&gamepadLock);
    settingsShortcutDown = (textPad.buttons & settings_trigger()) == settings_trigger();
    pthread_mutex_unlock(&gamepadLock);
    if (physical < 0) runtime_setting_remove("controller-settings.txt");
    else setting_write("controller-settings.txt", controller_physical_names[physical], "settings panel button");
    fprintf(stderr, "[totk-input] settings panel button: %s\n", physical < 0 ? "L3 + R3" : controller_physical_names[physical]);
    refresh_gamepads();
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

static int face_by_position(GCController *controller) {
    return gamepad_type_face_by_position(atomic_load(&controllerType), controller.productCategory.UTF8String, controller.vendorName.UTF8String);
}

/* The physical buttons held, bit i = controller_physical_names[i]; the face buttons by where they sit
   (gamepad_face_positions), so one layout file means the same buttons on every controller. */
static uint64_t gamepad_buttons(GCExtendedGamepad *pad, int by_position) {
    uint64_t buttons = gamepad_face_positions(pad.buttonA.isPressed, pad.buttonB.isPressed, pad.buttonX.isPressed, pad.buttonY.isPressed, by_position);
    if (pad.leftThumbstickButton.isPressed) buttons |= 1ULL << 4;
    if (pad.rightThumbstickButton.isPressed) buttons |= 1ULL << 5;
    if (pad.leftShoulder.isPressed) buttons |= 1ULL << 6;
    if (pad.rightShoulder.isPressed) buttons |= 1ULL << 7;
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

static void refresh_gamepads(void) {
    RuntimeGamepadState next = {0};
    uint64_t physical = 0;
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        next.connected = 1;
        physical |= gamepad_buttons(pad, face_by_position(controller));
        next.lx = stronger_axis(next.lx, stick_axis(pad.leftThumbstick.xAxis.value));
        next.ly = stronger_axis(next.ly, stick_axis(pad.leftThumbstick.yAxis.value));
        next.rx = stronger_axis(next.rx, stick_axis(pad.rightThumbstick.xAxis.value));
        next.ry = stronger_axis(next.ry, stick_axis(pad.rightThumbstick.yAxis.value));
    }
    /* Modes work on the physical buttons, not on the mapped ones: two physical buttons may share a Switch button
       (X and L3 both running), and a mapped edge would be lost while the other one is held. */
    uint64_t now = runtime_now_ns(), edges = physical & ~physicalPrev, synthetic = 0, suppressed = 0, trigger = settings_trigger();
    if (atomic_load(&settingsButton) >= 0) suppressed |= trigger;   /* a picked button is the panel's only */
    int shortcut = settingsShortcutEnabled && (physical & trigger) == trigger;
    int moving = next.lx || next.ly;
    physicalPrev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(controllerModes[i]);
        if (!textCapture && !shortcut && kind == CONTROLLER_TAP && (edges & bit)) {
            tapUntil[i] = now + 400000000ULL;
            /* Release the pulse even if the controller sends no further event. */
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(400 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
        }
        if (kind == CONTROLLER_TOGGLE) {
            if (!textCapture && !shortcut && (edges & bit)) { toggleOn[i] = !toggleOn[i]; toggleMoved[i] = moving; }
            if (toggleOn[i] && moving) toggleMoved[i] = 1;
            /* A toggle ends when the stick returns to center after moving, or the controller goes away; clicking at rest
               waits for the first move, and attacking while running does not stop it. */
            if (toggleOn[i] && ((toggleMoved[i] && !moving) || !next.connected)) toggleOn[i] = 0;
        }
        if (kind == CONTROLLER_AIM && (physical & bit)) {
            /* Aim: the Switch button is held while drawing, released while the fire button is down (the shot), and its
               fire button does nothing else meanwhile. */
            uint64_t fire = 1ULL << controller_mode_fire(controllerModes[i]);
            suppressed |= fire;
            if (!(physical & fire)) synthetic |= controllerMap[i];
        }
        int active = kind == CONTROLLER_TAP ? now < tapUntil[i] : kind == CONTROLLER_TOGGLE ? toggleOn[i] : 0;
        if (active) synthetic |= controllerMap[i];
    }
    uint64_t held = physical & ~suppressed;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) if (controllerModes[i] != CONTROLLER_HOLD) held &= ~(1ULL << i);
    next.buttons |= controller_map_apply(controllerMap, held) | synthetic;
    pthread_mutex_lock(&gamepadLock);
    if (shortcut && !settingsShortcutDown) {
        settingsShortcutPending = 1;
        if (!textCapture) textCapture = 2;   /* from here the game no longer gets the controller, until it is released */
    }
    settingsShortcutDown = shortcut;
    uint64_t all = (1ull << CONTROLLER_BUTTONS) - 1;
    textEdges |= (physical & all) & ~textPad.buttons;
    textPad = next; textPad.buttons = physical & all;
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    gamepadState = next;
    pthread_mutex_unlock(&gamepadLock);
}

/* The settings panel: auto, Xbox, PlayStation or Switch; the held buttons are read again at once. */
int runtime_gamepad_type_set(int type) {
    if (type < GAMEPAD_TYPE_AUTO || type >= GAMEPAD_TYPE_COUNT) return atomic_load(&controllerType);
    if (type == atomic_load(&controllerType)) return type;
    atomic_store(&controllerType, type);
    setting_write("controller-type.txt", gamepad_type_name(type), "controller type");
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

/* For the panel and the overlay (main thread): the picked type; *detected = what auto takes the first connected
   controller for, -1 none. */
int runtime_gamepad_type(int *detected) {
    int found = -1;
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad) { found = gamepad_type_detect(controller.productCategory.UTF8String, controller.vendorName.UTF8String); break; }
    if (detected) *detected = found;
    return atomic_load(&controllerType);
}

/* For the overlay (main thread): the first controller's battery as a whole percent, -1 none or unknown. */
int runtime_gamepad_battery_percent(void) {
    for (GCController *controller in GCController.controllers)
        if (controller.battery && controller.battery.batteryState != GCDeviceBatteryStateUnknown)
            return (int)lroundf(controller.battery.batteryLevel * 100.0f);
    return -1;
}

static void attach_gamepad(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad) return;
    pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
        (void)changed; (void)element;
        refresh_gamepads();
    };
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s), face buttons by %s\n",
            (controller.vendorName ?: @"unknown").UTF8String,
            (controller.productCategory ?: @"unknown").UTF8String, face_by_position(controller) ? "position" : "label");
    runtime_feedback_attach(controller);
    refresh_gamepads();
}

void runtime_gamepad_start(void) {
    if (started) return;
    started = YES;
    load_controller_map();
    load_controller_type();
    load_settings_button();
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { attach_gamepad(notification.object); }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) {
                       GCController *controller = notification.object;
                       fprintf(stderr, "[totk-input] native gamepad disconnected: %s\n",
                               (controller.vendorName ?: @"unknown").UTF8String);
                       runtime_feedback_detach(controller);
                       refresh_gamepads();
                   }];
    for (GCController *controller in GCController.controllers) attach_gamepad(controller);
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
    physicalPrev = 0;
    memset(tapUntil, 0, sizeof tapUntil);
    memset(toggleOn, 0, sizeof toggleOn);
    memset(toggleMoved, 0, sizeof toggleMoved);
    pthread_mutex_lock(&gamepadLock);
    gamepadState = (RuntimeGamepadState){0};
    pressedEdges = 0; textEdges = 0; textPad = (RuntimeGamepadState){0};
    settingsShortcutDown = settingsShortcutPending = 0;
    pthread_mutex_unlock(&gamepadLock);
}

/* On-screen touch controls (iPhone, ios/app/Sources/touch_controls.m) feed a second state merged with the physical
   controllers: buttons are OR-ed (a tap shorter than one game frame is latched like a controller press), each stick
   axis takes the stronger value. */
static RuntimeGamepadState touchState;
void runtime_touch_pad_set(const RuntimeGamepadState *s) {
    pthread_mutex_lock(&gamepadLock);
    pressedEdges |= s->buttons & ~touchState.buttons;
    touchState = *s;
    pthread_mutex_unlock(&gamepadLock);
}

/* The settings panel's controller shortcut (settings_controller_start: on while the window lives; off while a box waits
   for a controller button, which may be the shortcut's). */
void runtime_gamepad_settings_enable(int enabled) {
    pthread_mutex_lock(&gamepadLock);
    settingsShortcutEnabled = !!enabled;
    settingsShortcutPending = 0;
    settingsShortcutDown = (textPad.buttons & settings_trigger()) == settings_trigger();   /* one held through this waits for a new press */
    pthread_mutex_unlock(&gamepadLock);
}
int runtime_gamepad_settings_requested(void) {
    pthread_mutex_lock(&gamepadLock);
    int requested = settingsShortcutPending; settingsShortcutPending = 0;
    pthread_mutex_unlock(&gamepadLock);
    return requested;
}
/* 1: the panel or the text entry bar owns the controller (the game reads a released one); 0: back to the game once
   everything is released. */
void runtime_gamepad_text_capture(int on) {
    pthread_mutex_lock(&gamepadLock);
    textCapture = on ? 1 : 2;
    textEdges = pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
    /* UI actions must not start gameplay tap/toggle bindings. */
    memset(tapUntil, 0, sizeof tapUntil); memset(toggleOn, 0, sizeof toggleOn); memset(toggleMoved, 0, sizeof toggleMoved);
    refresh_gamepads();
}
/* The physical buttons (controller_map.h order) held or pressed since the last read, and the sticks, for the panel. */
void runtime_gamepad_text_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = textPad; out->buttons |= textEdges; textEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = gamepadState;
    out->buttons |= touchState.buttons | pressedEdges;
    out->lx = stronger_axis(out->lx, touchState.lx); out->ly = stronger_axis(out->ly, touchState.ly);
    out->rx = stronger_axis(out->rx, touchState.rx); out->ry = stronger_axis(out->ry, touchState.ry);
    if (touchState.connected) out->connected = 1;
    pressedEdges = 0;
    if (textCapture) {
        if (textCapture == 2 && !textPad.buttons && !textPad.lx && !textPad.ly && !textPad.rx && !textPad.ry) textCapture = 0;
        *out = (RuntimeGamepadState){.connected = out->connected};
    }
    pthread_mutex_unlock(&gamepadLock);
}
