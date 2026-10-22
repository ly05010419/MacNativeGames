/* Native macOS controllers feed the same FullKey state as the keyboard. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "gamepad_layout.h"   // face buttons by position (PlayStation, Xbox) or label (Nintendo)
#include "controller_map.h"   // the player's button layout (controls.txt)

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState gamepadState;
static uint64_t pressedEdges;
static BOOL started;
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
static uint64_t physicalPrev;
static uint64_t tapUntil[CONTROLLER_BUTTONS];   /* ns, for tap */
static int toggleOn[CONTROLLER_BUTTONS];        /* for toggle */
static int toggleMoved[CONTROLLER_BUTTONS];     /* the stick moved since the toggle went on */
static void refresh_gamepads(void);

/* The player's button layout (controller_map.h): TOTK_CONTROLS names the file (the launcher: <data>/controls.txt and
   checks it first). No file: the default layout. */
static void load_controller_map(void) {
    controller_map_default(controllerMap, controllerModes);
    const char *path = getenv("TOTK_CONTROLS");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "[totk-input] controller layout: default\n"); return; }
    char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); int whole = feof(f); fclose(f);
    if (!whole) runtime_fail("controller layout file too large");
    text[n] = 0;
    const char *error = NULL; int line = controller_map_parse(text, controllerMap, controllerModes, &error);
    if (line) { fprintf(stderr, "[totk-input] %s line %d: %s\n", path, line, error); runtime_fail("controller layout file"); }
    uint16_t defaults[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaults, defaultModes);
    int changed = 0; for (int i = 0; i < CONTROLLER_BUTTONS; i++) changed += controllerMap[i] != defaults[i];
    fprintf(stderr, "[totk-input] controller layout: %s (%d buttons changed)\n", path, changed);
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
    return gamepad_face_by_position(controller.productCategory.UTF8String, controller.vendorName.UTF8String);
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
    uint64_t now = runtime_now_ns(), edges = physical & ~physicalPrev, synthetic = 0, suppressed = 0;
    int moving = next.lx || next.ly;
    physicalPrev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(controllerModes[i]);
        if (kind == CONTROLLER_TAP && (edges & bit)) {
            tapUntil[i] = now + 400000000ULL;
            /* Release the pulse even if the controller sends no further event. */
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(400 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
        }
        if (kind == CONTROLLER_TOGGLE) {
            if (edges & bit) { toggleOn[i] = !toggleOn[i]; toggleMoved[i] = moving; }
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
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    gamepadState = next;
    pthread_mutex_unlock(&gamepadLock);
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

/* ---- Trackpad right stick (macOS only; off by default, enable it in trackpad.txt) ----
   One finger on the built-in trackpad drives the right stick (the camera). Raw touches come from the private
   MultitouchSupport framework, loaded at run time, so the build needs no new link flags.
   Touch-relative mode (default): the stick origin drifts toward the finger, so the stick follows finger SPEED.
   A faster swipe turns the camera more, and a finger that stops (or rests) returns to neutral.
   Settings live in ~/Library/Application Support/MacNativeGames/TOTK/trackpad.txt (created on first launch, every
   option explained inside). Edits apply on the next touch. TOTK_TRACKPAD_CONFIG names another file, and the
   environment variables TOTK_TRACKPAD, TOTK_TP_MODE, TOTK_TP_SENS, TOTK_TP_DEADZONE, TOTK_TP_CURVE, TOTK_TP_SMOOTH,
   TOTK_TP_RECENTER_MS and TOTK_TP_HIDECURSOR override the file. */
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <dlfcn.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef __OBJC__
#import <AppKit/AppKit.h>
#endif

typedef struct { float x, y; } TPPoint;
typedef struct { TPPoint pos, vel; } TPVector;
typedef struct {   /* MultitouchSupport touch record (96 bytes) */
    int32_t frame; double timestamp; int32_t identifier, state, foo3, foo4;
    TPVector normalized; float size; int32_t zero1; float angle, major, minor;
    TPVector mm; int32_t zero2[2]; float unk2;
} TPTouch;
typedef void *TPDeviceRef;
typedef int (*TPCallback)(TPDeviceRef, TPTouch *, int, double, int);
typedef CFArrayRef (*TPCreateList)(void);
typedef void (*TPRegister)(TPDeviceRef, TPCallback);
typedef int (*TPStart)(TPDeviceRef, int);
typedef bool (*TPIsBuiltIn)(TPDeviceRef);

#define TP_TOUCHING 4
#define TP_ASPECT 1.5f          /* trackpad width / height, so a circle on the pad is a circle on the stick */
#define TP_DEFAULT_ENABLED 0    /* opt-in: the settings file or TOTK_TRACKPAD=1 turns it on */

static int tpEnabled, tpRelative = 1, tpTracking, tpHideCursor = 1;
static volatile int tpFocused = 1, tpCursorWanted;
static int32_t tpId, tpX, tpY;            /* tpX, tpY: output, guarded by gamepadLock */
static float tpOx, tpOy, tpSx, tpSy;      /* origin, smoothed output */
static double tpLast;                     /* timestamp of the previous touch frame */
static float tpPx, tpPy;                  /* finger position in the previous frame */
static float tpDead = 0.10f, tpRadius = 0.08f, tpCurve = 1.5f, tpSmooth = 0.35f, tpRecenter = 0.12f;

/* ---- settings file: "name = value" lines, # starts a comment ---- */
#define TP_CFG_MAX 16
static char tpCfgKey[TP_CFG_MAX][24], tpCfgVal[TP_CFG_MAX][48];
static int tpCfgCount;
static char tpCfgPath[1024];
static time_t tpCfgMtime;

static const char tpCfgTemplate[] =
    "# Trackpad as the right stick (camera) for the Mac build of Tears of the Kingdom.\n"
    "# Format: name = value. Lines starting with # are comments. Delete a line to use its default.\n"
    "# Save the file, then touch the trackpad: changes apply on the next touch.\n"
    "# Only \"enabled\" needs a relaunch. Environment variables (TOTK_TP_...) override this file.\n"
    "\n"
    "# on or off. Off by default. Set it to on to drive the right stick with one finger\n"
    "# on the built-in trackpad. Arrow keys and controllers keep working.\n"
    "enabled = off\n"
    "\n"
    "# relative: where your finger lands is the start point, and the stick follows finger speed.\n"
    "# center: the middle of the pad is neutral and the stick follows finger position.\n"
    "mode = relative\n"
    "\n"
    "# More camera for the same swipe. Range 0.2 to 5.\n"
    "sensitivity = 1.0\n"
    "\n"
    "# Ignore tiny movements. Range 0 to 0.9.\n"
    "deadzone = 0.10\n"
    "\n"
    "# Higher: slow swipes are gentler and fast swipes are stronger. Range 0.5 to 4.\n"
    "curve = 1.5\n"
    "\n"
    "# Smoothing. 0 is none; higher is smoother but laggier. Range 0 to 0.9.\n"
    "smoothing = 0.35\n"
    "\n"
    "# How fast the stick returns to neutral when your finger stops, in milliseconds (relative mode).\n"
    "# Lower stops faster but needs bigger swipes. 0 keeps a fixed start point, like a plain joystick.\n"
    "# Range 0 to 1000.\n"
    "recenter_ms = 120\n"
    "\n"
    "# on or off. Hide and freeze the mouse pointer while a finger drives the stick.\n"
    "hide_cursor = on\n"
    "";

static void tp_cfg_path(void) {
    const char *over = getenv("TOTK_TRACKPAD_CONFIG"), *home = getenv("HOME");
    if (over && *over) snprintf(tpCfgPath, sizeof tpCfgPath, "%s", over);
    else if (home && *home) snprintf(tpCfgPath, sizeof tpCfgPath, "%s/Library/Application Support/MacNativeGames/TOTK/trackpad.txt", home);
}

static void tp_cfg_write_template(void) {   /* only when the file does not exist; never overwrites */
    FILE *f = fopen(tpCfgPath, "w");
    if (!f) return;
    fputs(tpCfgTemplate, f);
    fclose(f);
}

static void tp_trim_end(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static void tp_cfg_load(void) {
    tpCfgCount = 0;
    FILE *f = tpCfgPath[0] ? fopen(tpCfgPath, "r") : NULL;
    if (!f) return;
    char line[160];
    while (tpCfgCount < TP_CFG_MAX && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        char *note = strstr(val, " #");
        if (note) *note = 0;
        tp_trim_end(p); tp_trim_end(val);
        snprintf(tpCfgKey[tpCfgCount], sizeof tpCfgKey[0], "%.23s", p);
        snprintf(tpCfgVal[tpCfgCount], sizeof tpCfgVal[0], "%.47s", val);
        tpCfgCount++;
    }
    fclose(f);
}

static const char *tp_str(const char *envName, const char *key) {   /* environment first, then the file */
    const char *e = getenv(envName);
    if (e && *e) return e;
    for (int i = 0; i < tpCfgCount; i++) if (strcasecmp(tpCfgKey[i], key) == 0) return tpCfgVal[i];
    return NULL;
}

static float tp_num(const char *envName, const char *key, float def, float lo, float hi) {
    const char *s = tp_str(envName, key);
    if (!s || !*s) return def;
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s) return def;   /* not a number: keep the default */
    return v < lo ? lo : v > hi ? hi : v;
}

static int tp_flag(const char *envName, const char *key, int def) {
    const char *s = tp_str(envName, key);
    if (!s || !*s) return def;
    if (!strcasecmp(s, "on") || !strcasecmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "true")) return 1;
    if (!strcasecmp(s, "off") || !strcasecmp(s, "0") || !strcasecmp(s, "no") || !strcasecmp(s, "false")) return 0;
    return def;
}

static void tp_apply(void) {
    const char *mode = tp_str("TOTK_TP_MODE", "mode");
    tpRelative = !(mode && strcasecmp(mode, "center") == 0);
    tpHideCursor = tp_flag("TOTK_TP_HIDECURSOR", "hide_cursor", 1);
    tpDead = tp_num("TOTK_TP_DEADZONE", "deadzone", 0.10f, 0.0f, 0.9f);
    tpRecenter = tp_num("TOTK_TP_RECENTER_MS", "recenter_ms", 120.0f, 0.0f, 1000.0f) / 1000.0f;
    float sens = tp_num("TOTK_TP_SENS", "sensitivity", 1.0f, 0.2f, 5.0f);
    tpRadius = ((tpRelative && tpRecenter > 0.0f) ? 0.08f : 0.15f) / sens;
    tpCurve = tp_num("TOTK_TP_CURVE", "curve", 1.5f, 0.5f, 4.0f);
    tpSmooth = tp_num("TOTK_TP_SMOOTH", "smoothing", 0.35f, 0.0f, 0.9f);
}

static void tp_cfg_poll(void) {   /* a finger just landed: reload the file if it changed */
    struct stat st;
    if (!tpCfgPath[0] || stat(tpCfgPath, &st) != 0 || st.st_mtime == tpCfgMtime) return;
    tpCfgMtime = st.st_mtime;
    tp_cfg_load();
    tp_apply();
    fprintf(stderr, "[totk-input] trackpad settings reloaded: sensitivity radius %.3f, deadzone %.2f, curve %.2f, smooth %.2f, recenter %.0f ms\n",
            tpRadius, tpDead, tpCurve, tpSmooth, tpRecenter * 1000.0f);
}

#ifdef __OBJC__
/* While a finger drives the stick the pointer is hidden and frozen, so it cannot wander off the window, wake the
   Dock or hit a hot corner. Everything is undone the moment the finger lifts or the window loses focus. */
static int tpCursorHidden;   /* main thread only */
static void tp_cursor_apply(int hide) {
    if (hide && !tpCursorHidden) {
        CGDisplayHideCursor(kCGDirectMainDisplay);
        CGAssociateMouseAndMouseCursorPosition(false);
        tpCursorHidden = 1;
    } else if (!hide && tpCursorHidden) {
        CGAssociateMouseAndMouseCursorPosition(true);
        CGDisplayShowCursor(kCGDirectMainDisplay);
        tpCursorHidden = 0;
    }
}
static void tp_set_cursor(int want) {
    if (want && !tpHideCursor) return;   /* showing the pointer again is always allowed */
    if (want == tpCursorWanted) return;
    tpCursorWanted = want;
    dispatch_async(dispatch_get_main_queue(), ^{ tp_cursor_apply(want); });
}
#else
static void tp_set_cursor(int want) { (void)want; }
#endif

/* Neutral at once: finger lifted, focus lost, emergency stop. */
static void tp_release(void) {
    pthread_mutex_lock(&gamepadLock);
    tpX = 0; tpY = 0; tpTracking = 0; tpSx = 0; tpSy = 0;
    pthread_mutex_unlock(&gamepadLock);
    tp_set_cursor(0);
}

static int tp_callback(TPDeviceRef device, TPTouch *touches, int count, double timestamp, int frame) {
    (void)device; (void)frame;
    TPTouch *mine = NULL;
    if (tpFocused) {
        for (int i = 0; i < count; i++) {
            if (touches[i].state != TP_TOUCHING) continue;
            if (tpTracking) { if (touches[i].identifier == tpId) { mine = &touches[i]; break; } }
            else { mine = &touches[i]; break; }
        }
    }
    if (!mine) { tp_release(); return 0; }   /* no finger, or the tracked finger left: neutral */
    float px = mine->normalized.pos.x, py = mine->normalized.pos.y;
    if (!tpTracking) {
        tp_cfg_poll();
        tpTracking = 1; tpId = mine->identifier;
        tpOx = tpRelative ? px : 0.5f; tpOy = tpRelative ? py : 0.5f;   /* touch-relative or centre-start */
        tpSx = 0; tpSy = 0; tpLast = timestamp; tpPx = px; tpPy = py;
        tp_set_cursor(1);
    }
    double dt = timestamp - tpLast; tpLast = timestamp;
    if (dt <= 0.0 || dt > 0.25) dt = 0.01;
    if (dt < 0.002) dt = 0.002;
    if (dt > 0.05) dt = 0.05;
    if (tpRelative && tpRecenter > 0.0f) {   /* the origin chases the finger: output follows speed, rest = neutral */
        float speed = hypotf((px - tpPx) * TP_ASPECT, py - tpPy) / (float)dt;   /* pad widths per second */
        float tau = speed < 0.12f ? tpRecenter * 0.25f : tpRecenter;             /* a resting finger stops the stick fast */
        float a = 1.0f - expf(-(float)dt / tau);
        tpOx += (px - tpOx) * a; tpOy += (py - tpOy) * a;
    }
    tpPx = px; tpPy = py;
    float dx = (px - tpOx) * TP_ASPECT / tpRadius, dy = (py - tpOy) / tpRadius, mag = hypotf(dx, dy);
    float ox = 0, oy = 0;
    if (mag > tpDead) {
        float m = mag > 1.0f ? 1.0f : mag;
        m = (m - tpDead) / (1.0f - tpDead);
        m = powf(m, tpCurve);
        ox = dx / mag * m; oy = dy / mag * m;   /* y is up on the pad, and up on the stick */
    }
    tpSx = tpSx * tpSmooth + ox * (1.0f - tpSmooth);
    tpSy = tpSy * tpSmooth + oy * (1.0f - tpSmooth);
    pthread_mutex_lock(&gamepadLock);
    tpX = (int32_t)(tpSx * 32767.0f); tpY = (int32_t)(tpSy * 32767.0f);
    pthread_mutex_unlock(&gamepadLock);
    return 0;
}

static void trackpad_start(void) {
    struct stat st;
    tp_cfg_path();
    if (tpCfgPath[0] && stat(tpCfgPath, &st) != 0 && errno == ENOENT) tp_cfg_write_template();
    tp_cfg_load();
    if (tpCfgPath[0] && stat(tpCfgPath, &st) == 0) tpCfgMtime = st.st_mtime;
    if (!tp_flag("TOTK_TRACKPAD", "enabled", TP_DEFAULT_ENABLED)) {
        fprintf(stderr, "[totk-input] trackpad stick: off (set enabled = on in %s to use it)\n",
                tpCfgPath[0] ? tpCfgPath : "the settings file");
        return;
    }
    tp_apply();
    void *lib = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
    TPCreateList createList = lib ? (TPCreateList)dlsym(lib, "MTDeviceCreateList") : NULL;
    TPRegister registerCallback = lib ? (TPRegister)dlsym(lib, "MTRegisterContactFrameCallback") : NULL;
    TPStart start = lib ? (TPStart)dlsym(lib, "MTDeviceStart") : NULL;
    TPIsBuiltIn isBuiltIn = lib ? (TPIsBuiltIn)dlsym(lib, "MTDeviceIsBuiltIn") : NULL;
    if (!createList || !registerCallback || !start) {
        fprintf(stderr, "[totk-input] trackpad stick: MultitouchSupport not available\n");
        return;
    }
    CFArrayRef list = createList();   /* kept alive on purpose: the devices must outlive the callbacks */
    int used = 0;
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        TPDeviceRef device = (TPDeviceRef)CFArrayGetValueAtIndex(list, i);
        if (isBuiltIn && !isBuiltIn(device)) continue;   /* built-in trackpad only */
        registerCallback(device, tp_callback);
        start(device, 0);
        used++;
    }
    if (!used) { fprintf(stderr, "[totk-input] trackpad stick: no built-in trackpad found\n"); return; }
    tpEnabled = 1;
#ifdef __OBJC__
    dispatch_async(dispatch_get_main_queue(), ^{ if (NSApp) tpFocused = NSApp.isActive ? 1 : 0; });
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; tpFocused = 1; }];
    [center addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; tpFocused = 0; tp_release(); }];
#endif
    fprintf(stderr, "[totk-input] trackpad stick: on (%s, deadzone %.2f, radius %.3f, curve %.2f, smooth %.2f, recenter %.0f ms, cursor %s; settings %s)\n",
            tpRelative ? "touch-relative" : "centre-start", tpDead, tpRadius, tpCurve, tpSmooth, tpRecenter * 1000.0f,
            tpHideCursor ? "hidden while driving" : "left alone", tpCfgPath[0] ? tpCfgPath : "none");
}
#else
static void tp_release(void) {}
static void trackpad_start(void) {}
#endif

void runtime_gamepad_start(void) {
    if (started) return;
    started = YES;
    load_controller_map();
    trackpad_start();
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
    tp_release();
    physicalPrev = 0;
    memset(tapUntil, 0, sizeof tapUntil);
    memset(toggleOn, 0, sizeof toggleOn);
    memset(toggleMoved, 0, sizeof toggleMoved);
    pthread_mutex_lock(&gamepadLock);
    gamepadState = (RuntimeGamepadState){0};
    pressedEdges = 0;
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

void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = gamepadState;
    out->buttons |= touchState.buttons | pressedEdges;
    out->lx = stronger_axis(out->lx, touchState.lx); out->ly = stronger_axis(out->ly, touchState.ly);
    out->rx = stronger_axis(out->rx, touchState.rx); out->ry = stronger_axis(out->ry, touchState.ry);
    if (touchState.connected) out->connected = 1;
#if TARGET_OS_OSX
    if (tpEnabled) {   /* trackpad stick (TOTK_TRACKPAD=1) */
        out->rx = stronger_axis(out->rx, tpX); out->ry = stronger_axis(out->ry, tpY);
        out->connected = 1;
    }
#endif
    pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}
