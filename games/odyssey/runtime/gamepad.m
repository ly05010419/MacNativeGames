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
#include <strings.h>
#include <unistd.h>
#include "runtime.h"
#include "gamepad_layout.h"   // face buttons by position (PlayStation, Xbox) or label (Nintendo)
#include "controller_map.h"   // the player's button layout (controls.txt)
#include "switch_left_fallback.h"
#include "mouse_look.h"       // mouse mode: movement as the right stick and the gyro

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
/* Two physical seats remain stable through hotplug. The shared sample also includes any extra pads,
   for single-player and for the settings panel's physical (unmapped) controls. */
enum { EXTRA_PADS = 2 };
typedef struct {
    RuntimeGamepadState native;
    int32_t lx, ly, rx, ry;                 /* Switch HID fallback, while its report is fresh */
    uint64_t leftAt, rightAt;
} PadSample;
typedef struct {
    uint64_t physicalPrev, buttons, pressedEdges;
    uint64_t tapUntil[CONTROLLER_BUTTONS];
    int toggleOn[CONTROLLER_BUTTONS], toggleMoved[CONTROLLER_BUTTONS];
} PadMapping;
static PadSample samples[3], shared;        /* gamepadLock: two seats, extra pads, and their combined physical state */
static PadMapping mappings[2];              /* gamepadLock */
static int twoPlayers;                      /* only the guest's controller support applet selects 2P */
static NSMutableArray<GCController *> *controllers;    /* main queue, in connection order */
static GCController *seats[2];
static NSArray<GCController *> *feedbackControllers;   /* main queue */
static int hidSeat = -1;
static uint64_t textEdges;
static _Atomic int textCapture;       /* gamepadLock: 1 UI owns input, 2 wait for release, 0 game */
static BOOL started;
static _Atomic int settingsShortcutEnabled;
static int settingsShortcutDown, settingsShortcutPending;
/* The controller's way to the settings panel (games/totk, user 2026-10-29): L3 and R3 pressed together by default, or one button the
   player picks in the key bindings (the touchpad, say), kept in controller-settings.txt. It opens or closes the panel as
   soon as it is all down; from there the game gets no controller input until everything is released. A picked button
   drives nothing in the game. (Until 2026-10-29: L2 + R2 + L3 held for a second, games/totk, user 2026-10-27.) */
#define SETTINGS_CHORD ((1ull << 4) | (1ull << 5))
static _Atomic int settingsButton = -1;   /* physical button (controller_map.h), -1 the chord */
static uint64_t settings_trigger(void) { int b = atomic_load(&settingsButton); return b >= 0 ? 1ull << b : SETTINGS_CHORD; }
static int settings_chord_held(void) {    /* gamepadLock held */
    uint64_t trigger = settings_trigger();
    if (!twoPlayers) return (shared.native.buttons & trigger) == trigger;
    return (samples[0].native.buttons & trigger) == trigger || (samples[1].native.buttons & trigger) == trigger;
}
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
static __weak GCController *rawLeftController, *rawRightController;
static SwitchLeftFallback rawLeft, rawRight;
static void refresh_gamepads(void);
static void update_feedback_controllers(void);

static void forget_seat(int seat) {      /* physical disconnect: discard the pad's pulses, keep other input sources */
    if (seat < 0) return;
    pthread_mutex_lock(&gamepadLock);
    if (twoPlayers) mappings[seat] = (PadMapping){0};
    pthread_mutex_unlock(&gamepadLock);
}

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
static float stickReach[8] = {1, 1, 1, 1, 1, 1, 1, 1};   /* controller_reach_parse: a worn stick's travel per direction */
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
    line = controller_reach_parse(text, stickReach, &error);
    if (line) { fprintf(stderr, "[totk-input] %s line %d: %s\n", path, line, error); runtime_fail("controller layout file"); }
    for (int i = 0; i < 8; i++) if (stickReach[i] < 1.0f)
        fprintf(stderr, "[totk-input] %s %s reaches %.2f: scaled to full input\n", i < 4 ? "left stick" : "right stick", (const char *[]){"up", "down", "left", "right"}[i % 4], stickReach[i]);
    uint16_t defaults[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaults, defaultModes);
    int changed = 0; for (int i = 0; i < CONTROLLER_BUTTONS; i++) changed += controllerMap[i] != defaults[i];
    fprintf(stderr, "[totk-input] controller layout: %s (%d buttons changed)\n", path, changed);
}

static int32_t stick_axis(float value, float reach_negative, float reach_positive) {
    const float deadzone = 0.12f;
    value /= value < 0 ? reach_negative : reach_positive;   /* a worn direction's full travel is full input */
    float magnitude = fabsf(value);
    if (magnitude <= deadzone) return 0;
    if (magnitude > 1.0f) magnitude = 1.0f;
    int32_t result = (int32_t)((magnitude - deadzone) / (1.0f - deadzone) * 32767.0f);
    return value < 0 ? -result : result;
}

static int32_t stronger_axis(int32_t a, int32_t b) {
    return abs(b) > abs(a) ? b : a;
}

/* Settings the panel changes (controller type and mouse mode), one word each in a file next to controls.txt
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

/* The same one-word files for the rest of the runtime: 1 with the file's text in word as written (the caller drops the
   blanks around the word), 0 when there is no such file or no settings folder (a run without TOTK_CONTROLS). */
int runtime_setting_word(const char *name, char *word, size_t size) {
    char path[4096];
    return setting_read(name, word, size, path, sizeof path);
}
/* ... and saves one, as controller-type.txt is saved (the settings panel's controller rows). */
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
void runtime_controller_map_set(const uint16_t *map, const uint8_t *modes) {
    pthread_mutex_lock(&gamepadLock);
    memcpy(controllerMap, map, sizeof controllerMap); memcpy(controllerModes, modes, sizeof controllerModes);
    memset(mappings, 0, sizeof mappings);
    pthread_mutex_unlock(&gamepadLock);
    refresh_gamepads();
    const char *path = getenv("TOTK_CONTROLS"); char text[8192];
    if (path && *path && controller_map_format(controllerMap, controllerModes, text, sizeof text) > 0) file_write(path, text, "controller layout");
    fprintf(stderr, "[totk-input] controller layout: changed in the settings panel\n");
}
void runtime_controller_map_reset(void) {
    pthread_mutex_lock(&gamepadLock);
    controller_map_default(controllerMap, controllerModes);
    memset(mappings, 0, sizeof mappings);
    pthread_mutex_unlock(&gamepadLock);
    refresh_gamepads();
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
    hidPad.lx = stick_axis(hidX, stickReach[2], stickReach[3]);
    hidPad.ly = stick_axis(-hidY, stickReach[1], stickReach[0]);    /* HID Y grows downwards, the game's grows upwards */
    hidPad.rx = stick_axis(hidZ, stickReach[6], stickReach[7]);
    hidPad.ry = stick_axis(-hidRz, stickReach[5], stickReach[4]);
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
    forget_seat(hidSeat);
    hid_pad_reset();
    fprintf(stderr, "[totk-input] HID gamepad disconnected\n");
    refresh_gamepads();
    update_feedback_controllers();
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
   the mouse's buttons and movement here. Buttons go to the keyboard and mouse layout (runtime/input.c, keyboard_map.h:
   left attack, right draw the bow with left to shoot, middle lock on), which the settings panel's key bindings change;
   they are also physical buttons 17-21 of the controller layout, for mouse lines in controls.txt. Movement goes to
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
        MouseLookSettings settings = mouse_look_settings(NULL, getenv("TOTK_MOUSE_INVERT_Y"),
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
    refresh_gamepads();
}

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
    runtime_input_mouse(button, down);
    refresh_gamepads();
}

void runtime_mouse_release(void) {   /* main thread: the window let the pointer go (focus lost, text entry, mouse mode off) */
    pthread_mutex_lock(&gamepadLock);
    mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0;
    pthread_mutex_unlock(&gamepadLock);
    if (mouseButtons) {
        for (int i = 0; i < CONTROLLER_MOUSE_BUTTONS; i++) if (mouseButtons >> i & 1) runtime_input_mouse(i, 0);
        mouseButtons = 0; refresh_gamepads();
    }
}

static void merge_pad(RuntimeGamepadState *out, const RuntimeGamepadState *pad) {
    out->connected |= pad->connected;
    out->buttons |= pad->buttons;
    out->lx = stronger_axis(out->lx, pad->lx); out->ly = stronger_axis(out->ly, pad->ly);
    out->rx = stronger_axis(out->rx, pad->rx); out->ry = stronger_axis(out->ry, pad->ry);
}

static void merge_sample(PadSample *out, const PadSample *pad) {
    merge_pad(&out->native, &pad->native);
    if (pad->leftAt) { out->leftAt = pad->leftAt; out->lx = pad->lx; out->ly = pad->ly; }
    if (pad->rightAt) { out->rightAt = pad->rightAt; out->rx = pad->rx; out->ry = pad->ry; }
}

static PadSample combined_sample(const PadSample pads[3]) {
    PadSample combined = pads[0];
    merge_sample(&combined, &pads[1]);
    merge_sample(&combined, &pads[EXTRA_PADS]);
    return combined;
}

static PadSample sample_controller(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    float lx = pad.leftThumbstick.xAxis.value, ly = pad.leftThumbstick.yAxis.value;
    float rx = pad.rightThumbstick.xAxis.value, ry = pad.rightThumbstick.yAxis.value;
    PadSample s = {.native = {.connected = 1, .buttons = gamepad_buttons(pad, face_by_position(controller)),
        .lx = stick_axis(lx, stickReach[2], stickReach[3]), .ly = stick_axis(ly, stickReach[1], stickReach[0]),
        .rx = stick_axis(rx, stickReach[6], stickReach[7]), .ry = stick_axis(ry, stickReach[5], stickReach[4])}};
    int wasActive = rawLeft.active;
    if (controller == rawLeftController && switch_left_get(&rawLeft, lx, ly, runtime_now_ns(), &lx, &ly)) {
        s.leftAt = rawLeft.at; s.lx = stick_axis(lx, stickReach[2], stickReach[3]); s.ly = stick_axis(ly, stickReach[1], stickReach[0]);
        if (!wasActive) fprintf(stderr, "[totk-input] Switch left stick: native axes stuck at zero; using live HID input\n");
    }
    wasActive = rawRight.active;
    if (controller == rawRightController && switch_left_get(&rawRight, rx, ry, runtime_now_ns(), &rx, &ry)) {
        s.rightAt = rawRight.at; s.rx = stick_axis(rx, stickReach[6], stickReach[7]); s.ry = stick_axis(ry, stickReach[5], stickReach[4]);
        if (!wasActive) fprintf(stderr, "[totk-input] Switch right stick: native axes stuck at zero; using live HID input\n");
    }
    return s;
}

static RuntimeGamepadState pad_state(const PadSample *s) {
    RuntimeGamepadState out = s->native;
    uint64_t now = runtime_now_ns();
    if (s->leftAt && now - s->leftAt <= 250000000ULL) {
        out.lx = stronger_axis(out.lx, s->lx); out.ly = stronger_axis(out.ly, s->ly);
    }
    if (s->rightAt && now - s->rightAt <= 250000000ULL) {
        out.rx = stronger_axis(out.rx, s->rx); out.ry = stronger_axis(out.ry, s->ry);
    }
    return out;
}

/* gamepadLock held. In single-player the physical inputs are combined before applying the layout,
   just as for one controller. In two-player mode each seat has its own edges, taps and toggles. */
static void map_buttons(PadMapping *map, const PadSample *source) {
    RuntimeGamepadState next = pad_state(source);
    uint64_t physical = next.buttons;
    /* Modes work on the physical buttons, not on the mapped ones: two physical buttons may share a Switch button
       (X and L3 both running), and a mapped edge would be lost while the other one is held. */
    uint64_t now = runtime_now_ns(), edges = physical & ~map->physicalPrev, synthetic = 0, suppressed = 0, trigger = settings_trigger();
    if (atomic_load(&settingsButton) >= 0) suppressed |= trigger;   /* a picked button is the panel's only */
    int moving = next.lx || next.ly;
    map->physicalPrev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(controllerModes[i]);
        if (!textCapture && !(settingsShortcutEnabled && (physical & trigger) == trigger) && kind == CONTROLLER_TAP && (edges & bit)) {
            map->tapUntil[i] = now + 400000000ULL;
            /* Release the pulse even if the controller sends no further event. */
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(400 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
        }
        if (kind == CONTROLLER_TOGGLE) {
            if (!textCapture && !(settingsShortcutEnabled && (physical & trigger) == trigger) && (edges & bit)) { map->toggleOn[i] = !map->toggleOn[i]; map->toggleMoved[i] = moving; }
            if (map->toggleOn[i] && moving) map->toggleMoved[i] = 1;
            /* A toggle ends when the stick returns to center after moving, or the controller goes away; clicking at rest
               waits for the first move, and attacking while running does not stop it. */
            if (map->toggleOn[i] && ((map->toggleMoved[i] && !moving) || !next.connected)) map->toggleOn[i] = 0;
        }
        if (kind == CONTROLLER_AIM && (physical & bit)) {
            /* Aim: the Switch button is held while drawing, released while the fire button is down (the shot), and its
               fire button does nothing else meanwhile. */
            uint64_t fire = 1ULL << controller_mode_fire(controllerModes[i]);
            suppressed |= fire;
            if (!(physical & fire)) synthetic |= controllerMap[i];
        }
        int active = kind == CONTROLLER_TAP ? now < map->tapUntil[i] : kind == CONTROLLER_TOGGLE ? map->toggleOn[i] : 0;
        if (active) synthetic |= controllerMap[i];
    }
    uint64_t held = physical & ~suppressed;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) if (controllerModes[i] != CONTROLLER_HOLD) held &= ~(1ULL << i);
    uint64_t buttons = controller_map_apply(controllerMap, held) | synthetic;
    map->pressedEdges |= buttons & ~map->buttons;
    map->buttons = buttons;
}

static int free_seat(void) {
    for (int i = 0; i < 2; i++) if (!seats[i] && hidSeat != i) return i;
    return -1;
}

static void refresh_gamepads(void) {
    PadSample next[3] = {0};
    if (!hidPad.present) hidSeat = -1;
    for (GCController *controller in controllers) {
        int seat = controller == seats[0] ? 0 : controller == seats[1] ? 1 : free_seat();
        PadSample pad = sample_controller(controller);
        if (seat >= 0) {
            if (!seats[seat] && getenv("TOTK_INPUT_LOG"))
                fprintf(stderr, "[odyssey-input] seat %d: %s\n", seat + 1, (controller.vendorName ?: @"controller").UTF8String);
            seats[seat] = controller;
            next[seat] = pad;
        } else merge_sample(&next[EXTRA_PADS], &pad);
    }
    if (hidPad.present) {
        if (hidSeat < 0) hidSeat = free_seat();
        RuntimeGamepadState pad = {.connected = 1, .buttons = hidPad.buttons, .lx = hidPad.lx, .ly = hidPad.ly, .rx = hidPad.rx, .ry = hidPad.ry};
        merge_pad(&next[hidSeat >= 0 ? hidSeat : EXTRA_PADS].native, &pad);
    }
    uint64_t mouse = (uint64_t)mouseButtons << CONTROLLER_MOUSE_FIRST;
    next[0].native.buttons |= mouse;
    PadSample combined = combined_sample(next);
    pthread_mutex_lock(&gamepadLock);
    textEdges |= (combined.native.buttons & 0x1ffffu) & ~shared.native.buttons;
    shared = combined;
    memcpy(samples, next, sizeof samples);
    int chord = settingsShortcutEnabled && settings_chord_held();
    if (chord && !settingsShortcutDown) {
        settingsShortcutPending = 1;
        if (!textCapture) textCapture = 2;   /* from here the game no longer gets the controller, until it is released */
    }
    settingsShortcutDown = chord;
    map_buttons(&mappings[0], twoPlayers ? &samples[0] : &shared);
    if (twoPlayers) map_buttons(&mappings[1], &samples[1]);
    pthread_mutex_unlock(&gamepadLock);
}

/* Single-player offers controllers in connection order: a later pad can supply a sensor an
   earlier one lacks. In 2P only Mario's controller may drive the shared feedback backend. */
static void update_feedback_controllers(void) {
    pthread_mutex_lock(&gamepadLock);
    int split = twoPlayers;
    pthread_mutex_unlock(&gamepadLock);
    NSArray<GCController *> *targets = controllers;
    if (split) targets = seats[0] ? @[seats[0]] : @[];
    if ([targets isEqualToArray:feedbackControllers]) return;
    BOOL rebind = NO;
    for (GCController *pad in feedbackControllers) if (![targets containsObject:pad]) {
        runtime_feedback_detach(pad);
        rebind = YES;
    }
    for (GCController *pad in targets) {
        if (![feedbackControllers containsObject:pad]) rebind = YES;
        if (rebind) runtime_feedback_attach(pad);   /* restore later devices in their original order */
    }
    feedbackControllers = [targets copy];
}

static void attach_gamepad(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad || [controllers containsObject:controller]) return;
    if (!controllers) controllers = [NSMutableArray array];
    [controllers addObject:controller];
    pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
        (void)changed; (void)element;
        refresh_gamepads();
    };
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s), face buttons by %s\n",
            (controller.vendorName ?: @"unknown").UTF8String,
            (controller.productCategory ?: @"unknown").UTF8String, face_by_position(controller) ? "position" : "label");
    refresh_gamepads();
    update_feedback_controllers();
}

static void detach_gamepad(GCController *controller) {
    controller.extendedGamepad.valueChangedHandler = nil;
    [controllers removeObjectIdenticalTo:controller];
    for (int i = 0; i < 2; i++) if (seats[i] == controller) {
        forget_seat(i);
        seats[i] = nil;
    }
    if (rawLeftController == controller) { rawLeftController = nil; rawLeft = (SwitchLeftFallback){0}; }
    if (rawRightController == controller) { rawRightController = nil; rawRight = (SwitchLeftFallback){0}; }
    refresh_gamepads();
    update_feedback_controllers();
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
    load_settings_button();
    load_mouse_mode();
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
    hid_pad_start();
#if TARGET_OS_IPHONE
    [center addObserverForName:GCKeyboardDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { attach_keyboard(notification.object); }];
    [center addObserverForName:GCKeyboardDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                   usingBlock:^(NSNotification *notification) { detach_keyboard(notification.object); }];
    attach_keyboard(GCKeyboard.coalescedKeyboard);
#endif
}

int runtime_gamepad_set_players(int count) {
    pthread_mutex_lock(&gamepadLock);
    int split = count == 2 && samples[0].native.connected && samples[1].native.connected;
    if (split != twoPlayers) {
        twoPlayers = split;
        if (!split) shared = combined_sample(samples);   /* omit any input the guest cleared while split */
        memset(mappings, 0, sizeof mappings);
        for (int i = 0; i < 1 + split; i++) {
            const PadSample *src = split ? &samples[i] : &shared;
            mappings[i].physicalPrev = src->native.buttons;   /* held across the change is not a new tap/toggle */
            map_buttons(&mappings[i], src);
        }
        dispatch_async(dispatch_get_main_queue(), ^{ update_feedback_controllers(); });
    }
    pthread_mutex_unlock(&gamepadLock);
    return 1 + split;
}

int runtime_gamepad_second_connected(void) {
    pthread_mutex_lock(&gamepadLock);
    int connected = twoPlayers && samples[1].native.connected;
    pthread_mutex_unlock(&gamepadLock);
    return connected;
}

static RuntimeGamepadState touchState;
void runtime_gamepad_clear_player(int player) {     /* -1: focus loss or shared 1P input */
    if (player != 1) runtime_feedback_quiet();
    pthread_mutex_lock(&gamepadLock);
    for (int i = 0; i < 2; i++) if (player < 0 || player == i) {
        mappings[i] = (PadMapping){0};
        samples[i] = (PadSample){.native.connected = samples[i].native.connected};
    }
    /* A single guest Npad release does not cancel physical input pending in the host settings panel. */
    if (player < 0) {
        samples[EXTRA_PADS] = (PadSample){.native.connected = samples[EXTRA_PADS].native.connected};
        shared = (PadSample){.native.connected = shared.native.connected};
        textEdges = 0;
        settingsShortcutDown = settingsShortcutPending = 0;
    }
    if (player != 1) {
        touchState = (RuntimeGamepadState){0};
        mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0;
    }
    pthread_mutex_unlock(&gamepadLock);
}
void runtime_gamepad_clear(void) { runtime_gamepad_clear_player(-1); }

void runtime_touch_pad_set(const RuntimeGamepadState *s) {
    pthread_mutex_lock(&gamepadLock);
    mappings[0].pressedEdges |= s->buttons & ~touchState.buttons;
    touchState = *s;
    pthread_mutex_unlock(&gamepadLock);
}

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
void runtime_gamepad_text_capture(int on) {
    pthread_mutex_lock(&gamepadLock);
    textCapture = on ? 1 : 2;
    textEdges = 0;
    memset(mappings, 0, sizeof mappings);
    pthread_mutex_unlock(&gamepadLock);
    refresh_gamepads();
}
/* Keep the Switch HID fallback's expiry identical for the UI and the game. gamepadLock held. */
static RuntimeGamepadState text_state_locked(void) {
    RuntimeGamepadState state = pad_state(&shared);
    state.buttons &= 0x1ffffu;
    return state;
}
void runtime_gamepad_text_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = text_state_locked(); out->buttons |= textEdges; textEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}
void runtime_gamepad_read_player(int player, RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = pad_state(twoPlayers ? &samples[player] : &shared);
    out->buttons = mappings[player].buttons | mappings[player].pressedEdges;
    mappings[player].pressedEdges = 0;
    if (player == 0) {
        merge_pad(out, &touchState);
        out->rx = stronger_axis(out->rx, mouseRX); out->ry = stronger_axis(out->ry, mouseRY);
    }
    if (textCapture) {
        RuntimeGamepadState held = text_state_locked();
        if (textCapture == 2 && !held.buttons && !held.lx && !held.ly && !held.rx && !held.ry) {
            textCapture = 0;
            mappings[0].pressedEdges = mappings[1].pressedEdges = 0;
        }
        *out = (RuntimeGamepadState){.connected = out->connected};
    }
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_gamepad_read(RuntimeGamepadState *out) { runtime_gamepad_read_player(0, out); }
