/* Native macOS controllers feed the same FullKey state as the keyboard. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#endif
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "runtime.h"
#include "gamepad_layout.h"   // face buttons by position (PlayStation, Xbox) or label (Nintendo)
#include "controller_map.h"   // the player's button layout (controls.txt)
#include "switch_left_fallback.h"
#include "mouse_look.h"       // mouse mode: movement as the right stick and the gyro

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState gamepadState;
static uint64_t pressedEdges;
static RuntimeGamepadState textPad;
static uint64_t textEdges;
static _Atomic int textCapture;       /* gamepadLock: 1 UI owns input, 2 wait for release, 0 game */
static BOOL started;
static _Atomic int settingsShortcutEnabled;
static int settingsShortcutDown, settingsShortcutPending;
/* The chord opens the settings panel only once it has been held for a second (user 2026-10-27): L2 + R2 + L3 is also
   ZL + ZR + L3 in the game, pressed together for a moment while sneaking with the bow. Held still, the controller sends
   no events, so the panel's 60 Hz timer (runtime_gamepad_settings_requested) decides when the second is up. */
#define SETTINGS_HOLD_NS 1000000000ULL
static uint64_t settingsChordSince;
static int settingsChordFired;
#define SETTINGS_CHORD ((1ull << 8) | (1ull << 9) | (1ull << 4))
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
static uint64_t physicalPrev;
static uint64_t tapUntil[CONTROLLER_BUTTONS];   /* ns, for tap */
static int toggleOn[CONTROLLER_BUTTONS];        /* for toggle */
static int toggleMoved[CONTROLLER_BUTTONS];     /* the stick moved since the toggle went on */
static __weak GCController *rawLeftController, *rawRightController;
static SwitchLeftFallback rawLeft, rawRight;
static uint64_t gamepadRawLeftAt, gamepadRawRightAt;
static int32_t gamepadNativeLx, gamepadNativeLy, gamepadNativeRx, gamepadNativeRy;   /* the sticks as GameController reads them */
static void refresh_gamepads(void);

/* Called on the main run loop by the already-open Switch HID motion stream. */
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

/* Settings the keys change (6: controller type, 7: mouse mode), one word each in a file next to controls.txt
   (TOTK_CONTROLS; a run without it, such as a test, neither reads nor writes them), so they hold for the next launch. */
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

static void setting_write(const char *name, const char *word, const char *what) {
    char path[4096], temp[4200];
    if (!setting_path(name, path, sizeof path)) return;
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE *f = fopen(temp, "w");
    int ok = f && fprintf(f, "%s\n", word) > 0;
    if (f && fclose(f)) ok = 0;
    if (ok && rename(temp, path)) ok = 0;
    if (!ok) { fprintf(stderr, "[totk-input] could not save the %s to %s; it holds until the game closes\n", what, path); unlink(temp); }
}

/* The same one-word files for the rest of the runtime: 1 with the file's text in word as written (the caller drops the
   blanks around the word), 0 when there is no such file or no settings folder (a run without TOTK_CONTROLS). */
int runtime_setting_word(const char *name, char *word, size_t size) {
    char path[4096];
    return setting_read(name, word, size, path, sizeof path);
}
/* ... and saves one, as controller-type.txt is saved (the settings panel's controller rows). */
void runtime_setting_save(const char *name, const char *word, const char *what) { setting_write(name, word, what); }

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

#if TARGET_OS_OSX
/* ---- Generic HID pads macOS will not hand to GameController ----

   GameController only adopts a fixed set of controller families, so a generic USB pad (a Switch-style clone, a DirectInput
   pad from a budget brand) is invisible to it: GCController.controllers stays empty and none of the code below ever runs.
   The HID stack still sees such a pad, and the kernel marks it, per device, with GameControllerSupportedHIDDevice = No.

   This backend opens exactly those devices - present and false, never absent - so a controller GameController already
   handles is not read twice, and feeds the same physical button bits and stick values as the GameController path. From
   there the player's layout file (controller_map.h) applies unchanged, so face buttons, modes and remapping work the same.

   Usages come from the pad's own descriptor, so nothing here is specific to one model: button usages 1-15, the hat switch,
   and the X/Y/Z/Rz axes. A pad whose buttons sit elsewhere simply leaves those bits unused. */
typedef struct { int present; uint64_t buttons; int32_t lx, ly, rx, ry; } HIDPadState;
static HIDPadState hidPad;
static IOHIDManagerRef hidManager;
static IOHIDDeviceRef hidDevice;
static BOOL hidRefreshPending;
static unsigned char hidButtons[16];          /* usage 1..15, 1 = pressed */
static int hidHat = 8;                        /* 0 N, 1 NE, 2 E ... 7 NW; 8 centred */
static float hidX, hidY, hidZ, hidRz;         /* -1..1, 0 centred */

/* Axes and the hat are scaled from the element's own logical range (after PR #190): pads report 8-bit sticks (0..255),
   16-bit ones (0..65535 or -32768..32767) or a hat numbered from 1. A range the descriptor leaves empty counts as 0..255. */
static float hid_axis(IOHIDElementRef element, int v) {
    CFIndex lo = IOHIDElementGetLogicalMin(element), hi = IOHIDElementGetLogicalMax(element);
    if (hi <= lo) { lo = 0; hi = 255; }
    float f = (float)(v - lo) / (float)(hi - lo) * 2.0f - 1.0f;
    return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}
static int hid_hat(IOHIDElementRef element, int v) {   /* 8 positions (or 4, every other one); outside the range: centred */
    CFIndex lo = IOHIDElementGetLogicalMin(element), hi = IOHIDElementGetLogicalMax(element), n = hi - lo + 1;
    if (v < lo || v > hi || (n != 8 && n != 4)) return 8;
    return (int)(v - lo) * (n == 4 ? 2 : 1);
}

/* The same physical bits gamepad_buttons() produces, so the layout file cannot tell the two sources apart. */
static void hid_pad_compute(void) {
    /* The usages are Switch-style: B at the bottom (1), A on the right (2), Y on the left (4), X at the top (5). Those are
       positions, so they go in as GameController's buttonA (bottom), buttonB (right), buttonX (left), buttonY (top) and
       are always taken by position, whatever the maker's name says. */
    uint64_t buttons = gamepad_face_positions(hidButtons[1], hidButtons[2], hidButtons[4], hidButtons[5], 1);
    if (hidButtons[14]) buttons |= 1ULL << 4;    /* l3 */
    if (hidButtons[15]) buttons |= 1ULL << 5;    /* r3 */
    if (hidButtons[7]) buttons |= 1ULL << 6;     /* l1 */
    if (hidButtons[8]) buttons |= 1ULL << 7;     /* r1 */
    if (hidButtons[9]) buttons |= 1ULL << 8;     /* l2 = ZL */
    if (hidButtons[10]) buttons |= 1ULL << 9;    /* r2 = ZR */
    if (hidButtons[12]) buttons |= 1ULL << 10;   /* menu = plus (start) */
    if (hidButtons[11]) buttons |= 1ULL << 11;   /* view = minus (select) */
    switch (hidHat) {
    case 0: buttons |= 1ULL << 13; break;
    case 1: buttons |= (1ULL << 13) | (1ULL << 14); break;
    case 2: buttons |= 1ULL << 14; break;
    case 3: buttons |= (1ULL << 14) | (1ULL << 15); break;
    case 4: buttons |= 1ULL << 15; break;
    case 5: buttons |= (1ULL << 15) | (1ULL << 12); break;
    case 6: buttons |= 1ULL << 12; break;
    case 7: buttons |= (1ULL << 12) | (1ULL << 13); break;
    default: break;
    }
    hidPad.present = 1;
    hidPad.buttons = buttons;
    hidPad.lx = stick_axis(hidX);
    hidPad.ly = stick_axis(-hidY);    /* HID Y grows downwards, the game's grows upwards */
    hidPad.rx = stick_axis(hidZ);
    hidPad.ry = stick_axis(-hidRz);
}

static void hid_value(void *context, IOReturn result, void *sender, IOHIDValueRef value) {
    (void)context; (void)result; (void)sender;
    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t page = IOHIDElementGetUsagePage(element), usage = IOHIDElementGetUsage(element);
    int v = (int)IOHIDValueGetIntegerValue(value);
    if (page == kHIDPage_Button && usage >= 1 && usage <= 15) {
        hidButtons[usage] = (unsigned char)(v != 0);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Hatswitch) {
        hidHat = hid_hat(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_X) {
        hidX = hid_axis(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Y) {
        hidY = hid_axis(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Z) {
        hidZ = hid_axis(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Rz) {
        hidRz = hid_axis(element, v);
    } else {
        return;
    }
    hid_pad_compute();
    /* One report delivers a value per element: coalesce them into one refresh on the next run loop turn. */
    if (!hidRefreshPending) {
        hidRefreshPending = YES;
        dispatch_async(dispatch_get_main_queue(), ^{ hidRefreshPending = NO; refresh_gamepads(); });
    }
}

static void hid_pad_reset(void) {
    memset(hidButtons, 0, sizeof hidButtons);
    hidHat = 8;
    hidX = hidY = hidZ = hidRz = 0;
    hidPad = (HIDPadState){0};
}

static void hid_pad_detach(void) {
    if (!hidDevice) return;
    IOHIDDeviceUnscheduleFromRunLoop(hidDevice, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeNone);
    CFRelease(hidDevice);
    hidDevice = NULL;
    hid_pad_reset();
    fprintf(stderr, "[totk-input] HID gamepad disconnected\n");
    refresh_gamepads();
}

static void hid_pad_attach(IOHIDDeviceRef device) {
    if (hidDevice) return;
    /* Absent means GameController may well own it; only a definite No says the pad is unreachable otherwise. The flag also
       reads No on pads GameController does adopt (a Bluetooth Switch Pro Controller), so Nintendo, Sony and Microsoft
       devices are left to it. */
    CFTypeRef vendorId = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    int vid = 0;
    if (vendorId && CFGetTypeID(vendorId) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)vendorId, kCFNumberIntType, &vid);
    if (vid == 0x057e || vid == 0x054c || vid == 0x045e) return;
    CFTypeRef supported = IOHIDDeviceGetProperty(device, CFSTR("GameControllerSupportedHIDDevice"));
    if (!supported || CFGetTypeID(supported) != CFBooleanGetTypeID() || CFBooleanGetValue((CFBooleanRef)supported)) return;
    if (IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) return;

    hidDevice = (IOHIDDeviceRef)CFRetain(device);
    const char *name = NULL, *manufacturer = NULL;
    CFTypeRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    if (product && CFGetTypeID(product) == CFStringGetTypeID()) {
        name = [(__bridge NSString *)product UTF8String];
    }
    CFTypeRef vendor = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDManufacturerKey));
    if (vendor && CFGetTypeID(vendor) == CFStringGetTypeID()) {
        manufacturer = [(__bridge NSString *)vendor UTF8String];
    }
    hid_pad_reset();
    IOHIDDeviceRegisterInputValueCallback(device, hid_value, NULL);
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    hidPad.present = 1;
    fprintf(stderr, "[totk-input] HID gamepad connected: %s (%s)\n", name ? name : "unknown", manufacturer ? manufacturer : "unknown");
    refresh_gamepads();
}

static void hid_matched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)sender;
    if (result == kIOReturnSuccess) hid_pad_attach(device);
}

static void hid_removed(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    if (hidDevice && CFEqual(hidDevice, device)) hid_pad_detach();
}

static void hid_pad_start(void) {
    if (hidManager) return;
    hid_pad_reset();
    hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!hidManager) return;
    NSArray *matches = @[
        @{ @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_GamePad) },
        @{ @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_Joystick) },
    ];
    IOHIDManagerSetDeviceMatchingMultiple(hidManager, (__bridge CFArrayRef)matches);
    IOHIDManagerRegisterDeviceMatchingCallback(hidManager, hid_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(hidManager, hid_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(hidManager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(hidManager, kIOHIDOptionsTypeNone);
}

#else
typedef struct { int present; uint64_t buttons; int32_t lx, ly, rx, ry; } HIDPadState;
static HIDPadState hidPad;
static void hid_pad_start(void) {}
#endif

/* ---- Mouse mode (Mac, the settings panel; in mouse.txt) ----
   The window (runtime/frontend.m) holds the pointer while mouse mode is on and the window has the keyboard, and sends
   the mouse's buttons and movement here. Buttons are physical buttons 17-21 of the controller layout (controller_map.h:
   left attack, right draw the bow with left to shoot, middle target), so controls.txt can change them. Movement goes to
   runtime/mouse_look.h, ticked every 10 ms by a timer while mouse mode is on: the right stick (the stronger of it and a
   controller's) and, unless TOTK_MOUSE_GYRO=0 or the game has no six-axis device (TOTK_GYRO=0), the gyro, which the
   mouse then has to itself (runtime_motion_mouse). Settings: TOTK_MOUSE_SENSITIVITY 1-100 (50; else the settings
   panel's, in mouse-sensitivity.txt), TOTK_MOUSE_INVERT_Y=1,
   TOTK_MOUSE_COUNTERWEIGHT, TOTK_MOUSE_DECAY, TOTK_MOUSE_MIN_DECAY 0-100 (20, 18, 6). */
static _Atomic int mouseMode;
static unsigned mouseButtons;                  /* main thread; bit i = mouse button i (0 left, 1 right, 2 middle, 3, 4) */
static MouseLook mouseLook;                    /* gamepadLock */
static MouseLookSettings mouseSettings;        /* gamepadLock */
static int32_t mouseRX, mouseRY;               /* gamepadLock; the stick shown by the last tick */
static int mouseGyro;                          /* the mouse feeds the gyro in this mouse mode */
static dispatch_source_t mouseTimer;
static int mouseTicksByHand;                   /* tests/mouse/probe.m drives mouse_tick itself */
static void (*mouseHook)(void);                /* the window: mouse mode changed */

static void mouse_tick(void) {
    double gyro[3], acc[3]; int32_t rx, ry;
    pthread_mutex_lock(&gamepadLock);
#ifndef GAME_BOTW
    /* L held (keys or controller): the ability wheel is open and the mouse points at an ability (mouse_look.h) */
    mouse_look_point(&mouseLook, (int)(((runtime_keyboard_buttons() | gamepadState.buttons) >> 6) & 1));
#endif
    mouse_look_tick(&mouseLook, &mouseSettings, &rx, &ry, gyro, acc);
    mouseRX = rx; mouseRY = ry;
    int feed = mouseGyro;
    pthread_mutex_unlock(&gamepadLock);
    if (feed) runtime_motion_feed_mouse(acc, gyro, runtime_now_ns());
}

/* The settings panel's sensitivity row (key 1, runtime/settings_panel.h): TOTK_MOUSE_SENSITIVITY decides when set (the
   row greyed out), else mouse-sensitivity.txt, else 50. A pick is saved and applies at once, also in the middle of a turn. */
static _Atomic int mouseSensitivity;           /* 0: mouse-sensitivity.txt not read yet */
int runtime_mouse_sensitivity_locked(void) { return getenv("TOTK_MOUSE_SENSITIVITY") != NULL; }
int runtime_mouse_sensitivity(void) {
    if (runtime_mouse_sensitivity_locked()) return mouse_look_setting(getenv("TOTK_MOUSE_SENSITIVITY"), 1, 100, 50);
    int s = atomic_load(&mouseSensitivity);
    if (s) return s;
    char path[4096], word[16] = {0};
    s = 50;
    if (setting_read("mouse-sensitivity.txt", word, sizeof word, path, sizeof path)) {
        size_t n = strlen(word);
        while (n && (word[n - 1] == '\n' || word[n - 1] == '\r' || word[n - 1] == ' ' || word[n - 1] == '\t')) word[--n] = 0;
        s = mouse_look_setting(word, 1, 100, 0);
        if (!s) { fprintf(stderr, "[totk-input] %s: not a number from 1 to 100; sensitivity 50\n", path); s = 50; }
    }
    int none = 0;
    atomic_compare_exchange_strong(&mouseSensitivity, &none, s);
    return atomic_load(&mouseSensitivity);
}
void runtime_mouse_sensitivity_set(int sensitivity) {
    if (runtime_mouse_sensitivity_locked() || sensitivity < 1 || sensitivity > 100) return;
    atomic_store(&mouseSensitivity, sensitivity);
    char word[8];
    snprintf(word, sizeof word, "%d", sensitivity);
    setting_write("mouse-sensitivity.txt", word, "mouse sensitivity");
    pthread_mutex_lock(&gamepadLock);
    mouseSettings.sensitivity = sensitivity;
    pthread_mutex_unlock(&gamepadLock);
    fprintf(stderr, "[totk-input] mouse sensitivity %d (settings)\n", sensitivity);
}

static void mouse_apply(int on) {   /* start or stop the ticks and the gyro takeover */
    if (on) {
        const char *g = getenv("TOTK_MOUSE_GYRO");
        MouseLookSettings settings = mouse_look_settings(getenv("TOTK_MOUSE_SENSITIVITY"), getenv("TOTK_MOUSE_INVERT_Y"),
                                                         getenv("TOTK_MOUSE_COUNTERWEIGHT"), getenv("TOTK_MOUSE_DECAY"), getenv("TOTK_MOUSE_MIN_DECAY"));
        settings.sensitivity = runtime_mouse_sensitivity();
        int gyro = runtime_gyro_enabled() && !(g && !strcmp(g, "0"));
        pthread_mutex_lock(&gamepadLock);
        mouseSettings = settings; mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0; mouseGyro = gyro;
        pthread_mutex_unlock(&gamepadLock);
        runtime_motion_mouse(gyro);
        fprintf(stderr, "[totk-input] mouse mode on: sensitivity %d%s, counterweight %d, decay %d, minimum decay %d, gyro %s\n",
                settings.sensitivity, settings.invert_y ? " (Y inverted)" : "", settings.counterweight, settings.decay, settings.min_decay,
                gyro ? "from the mouse" : "not from the mouse");
        if (!mouseTicksByHand && !mouseTimer) {
            mouseTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_queue_create("totk.mouse", DISPATCH_QUEUE_SERIAL));
            dispatch_source_set_timer(mouseTimer, dispatch_time(DISPATCH_TIME_NOW, MOUSE_LOOK_TICK_NS), MOUSE_LOOK_TICK_NS, 1000000);
            dispatch_source_set_event_handler(mouseTimer, ^{ mouse_tick(); });
            dispatch_resume(mouseTimer);
        }
    } else {
        if (mouseTimer) { dispatch_source_cancel(mouseTimer); mouseTimer = nil; }
        pthread_mutex_lock(&gamepadLock);
        mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0; mouseGyro = 0;
        pthread_mutex_unlock(&gamepadLock);
        runtime_motion_mouse(0);
        fprintf(stderr, "[totk-input] mouse mode off\n");
    }
    if (mouseHook) dispatch_async(dispatch_get_main_queue(), ^{ mouseHook(); });
}

static void load_mouse_mode(void) {
    char path[4096], word[16] = {0};
    if (!setting_read("mouse.txt", word, sizeof word, path, sizeof path)) return;
    int on = !strcmp(word, "on\n") || !strcmp(word, "on");
    if (!on && strcmp(word, "off\n") && strcmp(word, "off")) fprintf(stderr, "[totk-input] %s: not on or off; mouse mode off\n", path);
    if (on != atomic_load(&mouseMode)) { atomic_store(&mouseMode, on); mouse_apply(on); }
}

int runtime_mouse_enabled(void) { return atomic_load(&mouseMode); }
void runtime_mouse_set_hook(void (*hook)(void)) { mouseHook = hook; }

/* The settings panel (key 1, frontend.m). */
int runtime_mouse_set(int on) {
    on = !!on;
    if (on == atomic_load(&mouseMode)) return on;
    atomic_store(&mouseMode, on);
    setting_write("mouse.txt", on ? "on" : "off", "mouse mode");
    if (!on) dispatch_async(dispatch_get_main_queue(), ^{ runtime_mouse_release(); });
    mouse_apply(on);
    return on;
}

void runtime_mouse_move(double dx, double dy) {   /* the window, while it holds the pointer */
    if (!atomic_load(&mouseMode)) return;
    pthread_mutex_lock(&gamepadLock);
    mouse_look_move(&mouseLook, dx, dy, &mouseSettings);
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_mouse_button(int button, int down) {   /* main thread */
    if (button < 0 || button >= CONTROLLER_MOUSE_BUTTONS || (down && !atomic_load(&mouseMode))) return;
    unsigned bit = 1u << button, next = down ? mouseButtons | bit : mouseButtons & ~bit;
    if (next == mouseButtons) return;
    mouseButtons = next;
    refresh_gamepads();
}

void runtime_mouse_release(void) {   /* main thread: the window let the pointer go (focus lost, text entry, mouse mode off) */
    pthread_mutex_lock(&gamepadLock);
    mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0;
    pthread_mutex_unlock(&gamepadLock);
    if (mouseButtons) { mouseButtons = 0; refresh_gamepads(); }
}

static void refresh_gamepads(void) {
    RuntimeGamepadState next = {0};
    uint64_t physical = (uint64_t)mouseButtons << CONTROLLER_MOUSE_FIRST;
    int32_t nativeLx = 0, nativeLy = 0, nativeRx = 0, nativeRy = 0;
    uint64_t rawAt = 0, rawRightAt = 0;
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        next.connected = 1;
        physical |= gamepad_buttons(pad, face_by_position(controller));
        float lx = pad.leftThumbstick.xAxis.value, ly = pad.leftThumbstick.yAxis.value;
        nativeLx = stronger_axis(nativeLx, stick_axis(lx));
        nativeLy = stronger_axis(nativeLy, stick_axis(ly));
        int wasActive = rawLeft.active;
        float rawX, rawY;
        if (controller == rawLeftController && switch_left_get(&rawLeft, lx, ly, runtime_now_ns(), &rawX, &rawY)) {
            lx = rawX; ly = rawY;
            rawAt = rawLeft.at;
            if (!wasActive) fprintf(stderr, "[totk-input] Switch left stick: native axes stuck at zero; using live HID input\n");
        }
        next.lx = stronger_axis(next.lx, stick_axis(lx));
        next.ly = stronger_axis(next.ly, stick_axis(ly));
        float rx = pad.rightThumbstick.xAxis.value, ry = pad.rightThumbstick.yAxis.value;
        nativeRx = stronger_axis(nativeRx, stick_axis(rx));
        nativeRy = stronger_axis(nativeRy, stick_axis(ry));
        int rightWasActive = rawRight.active;
        float rawRX, rawRY;
        if (controller == rawRightController && switch_left_get(&rawRight, rx, ry, runtime_now_ns(), &rawRX, &rawRY)) {
            rx = rawRX; ry = rawRY;
            rawRightAt = rawRight.at;
            if (!rightWasActive) fprintf(stderr, "[totk-input] Switch right stick: native axes stuck at zero; using live HID input\n");
        }
        next.rx = stronger_axis(next.rx, stick_axis(rx));
        next.ry = stronger_axis(next.ry, stick_axis(ry));
    }
    if (hidPad.present) {
        next.connected = 1;
        physical |= hidPad.buttons;
        next.lx = stronger_axis(next.lx, hidPad.lx);
        next.ly = stronger_axis(next.ly, hidPad.ly);
        next.rx = stronger_axis(next.rx, hidPad.rx);
        next.ry = stronger_axis(next.ry, hidPad.ry);
    }
    /* Modes work on the physical buttons, not on the mapped ones: two physical buttons may share a Switch button
       (X and L3 both running), and a mapped edge would be lost while the other one is held. */
    uint64_t now = runtime_now_ns(), edges = physical & ~physicalPrev, synthetic = 0, suppressed = 0;
    int moving = next.lx || next.ly;
    physicalPrev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(controllerModes[i]);
        if (!textCapture && !(settingsShortcutEnabled && (physical & SETTINGS_CHORD) == SETTINGS_CHORD) && kind == CONTROLLER_TAP && (edges & bit)) {
            tapUntil[i] = now + 400000000ULL;
            /* Release the pulse even if the controller sends no further event. */
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(400 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
        }
        if (kind == CONTROLLER_TOGGLE) {
            if (!textCapture && !(settingsShortcutEnabled && (physical & SETTINGS_CHORD) == SETTINGS_CHORD) && (edges & bit)) { toggleOn[i] = !toggleOn[i]; toggleMoved[i] = moving; }
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
    int chord = settingsShortcutEnabled && (physical & SETTINGS_CHORD) == SETTINGS_CHORD;
    if (chord && !settingsShortcutDown) { settingsChordSince = now; settingsChordFired = 0; }   /* the game keeps it until the second is up */
    settingsShortcutDown = chord;
    textEdges |= (physical & 0x1ffffu) & ~textPad.buttons;
    textPad = next; textPad.buttons = physical & 0x1ffffu;
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    gamepadState = next;
    gamepadRawLeftAt = rawAt; gamepadRawRightAt = rawRightAt;
    gamepadNativeLx = nativeLx; gamepadNativeLy = nativeLy; gamepadNativeRx = nativeRx; gamepadNativeRy = nativeRy;
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

/* For the HUD (main thread): the first controller's battery as a whole percent, -1 none or unknown. */
int runtime_gamepad_battery_percent(void) {
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

void runtime_gamepad_start(void) {
    if (started) return;
    started = YES;
    load_controller_map();
    load_controller_type();
    load_mouse_mode();
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
    hid_pad_start();
#if TARGET_OS_IPHONE
    [center addObserverForName:GCKeyboardDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { attach_keyboard(notification.object); }];
    [center addObserverForName:GCKeyboardDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { detach_keyboard(notification.object); }];
    attach_keyboard(GCKeyboard.coalescedKeyboard);
#endif
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
    physicalPrev = 0;
    memset(tapUntil, 0, sizeof tapUntil);
    memset(toggleOn, 0, sizeof toggleOn);
    memset(toggleMoved, 0, sizeof toggleMoved);
    pthread_mutex_lock(&gamepadLock);
    gamepadState = (RuntimeGamepadState){0};
    gamepadRawLeftAt = 0; gamepadRawRightAt = 0;
    gamepadNativeLx = gamepadNativeLy = 0;
    pressedEdges = 0; textEdges = 0; textPad = (RuntimeGamepadState){0};
    settingsShortcutDown = settingsShortcutPending = 0;
    mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0;
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

void runtime_gamepad_settings_enable(int enabled) {
    pthread_mutex_lock(&gamepadLock);
    settingsShortcutEnabled = !!enabled;
    settingsShortcutPending = 0;
    settingsShortcutDown = (textPad.buttons & SETTINGS_CHORD) == SETTINGS_CHORD;
    settingsChordSince = runtime_now_ns(); settingsChordFired = 0;   /* a chord held through this counts from now */
    pthread_mutex_unlock(&gamepadLock);
}
int runtime_gamepad_settings_requested(void) {
    pthread_mutex_lock(&gamepadLock);
    int requested = settingsShortcutPending; settingsShortcutPending = 0;
    uint64_t now = runtime_now_ns();
    if (settingsShortcutEnabled && settingsShortcutDown && !settingsChordFired && now >= settingsChordSince && now - settingsChordSince >= SETTINGS_HOLD_NS) {
        settingsChordFired = requested = 1;
        if (!textCapture) textCapture = 2;   /* from here the game no longer gets the chord, until it is released */
    }
    pthread_mutex_unlock(&gamepadLock);
    return requested;
}
void runtime_gamepad_text_capture(int on) {
    pthread_mutex_lock(&gamepadLock);
    textCapture = on ? 1 : 2;
    textEdges = pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
    /* UI actions must not start persistent gameplay tap/toggle bindings. */
    memset(tapUntil, 0, sizeof tapUntil); memset(toggleOn, 0, sizeof toggleOn);
    memset(toggleMoved, 0, sizeof toggleMoved);
    refresh_gamepads();
}
int runtime_gamepad_game_input(void) { return !atomic_load(&textCapture); }   /* the bow thread (gamepad_feedback.m) */
/* Keep the Switch HID fallback's expiry identical for the UI and the game. gamepadLock held. */
static RuntimeGamepadState text_state_locked(void) {
    RuntimeGamepadState state = textPad;
    if (gamepadRawLeftAt && runtime_now_ns() - gamepadRawLeftAt > 250000000ULL) {
        state.lx = gamepadNativeLx; state.ly = gamepadNativeLy;
    }
    if (gamepadRawRightAt && runtime_now_ns() - gamepadRawRightAt > 250000000ULL) {   /* the right stick's HID fallback too (after PR #844) */
        state.rx = gamepadNativeRx; state.ry = gamepadNativeRy;
    }
    return state;
}
void runtime_gamepad_text_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = text_state_locked(); out->buttons |= textEdges; textEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}
void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = gamepadState;
    if (gamepadRawLeftAt && runtime_now_ns() - gamepadRawLeftAt > 250000000ULL) {
        out->lx = gamepadNativeLx; out->ly = gamepadNativeLy;
    }
    if (gamepadRawRightAt && runtime_now_ns() - gamepadRawRightAt > 250000000ULL) {   /* the HID reports stopped: GameController's right stick */
        out->rx = gamepadNativeRx; out->ry = gamepadNativeRy;
    }
    out->buttons |= touchState.buttons | pressedEdges;
    out->lx = stronger_axis(out->lx, touchState.lx); out->ly = stronger_axis(out->ly, touchState.ly);
    out->rx = stronger_axis(out->rx, touchState.rx); out->ry = stronger_axis(out->ry, touchState.ry);
    out->rx = stronger_axis(out->rx, mouseRX); out->ry = stronger_axis(out->ry, mouseRY);
    if (touchState.connected) out->connected = 1;
    pressedEdges = 0;
    if (textCapture) {
        RuntimeGamepadState held = text_state_locked();
        if (textCapture == 2 && !held.buttons && !held.lx && !held.ly && !held.rx && !held.ry)
            textCapture = 0;
        *out = (RuntimeGamepadState){.connected = out->connected};
    }
    pthread_mutex_unlock(&gamepadLock);
}
