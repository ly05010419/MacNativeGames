/* Native macOS controllers feed the same FullKey state as the keyboard. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#else
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
typedef uint32_t IOOptionBits;
enum { kIOHIDOptionsTypeNone = 0, kIOHIDOptionsTypeSeizeDevice = 1 };
typedef enum { kIOHIDReportTypeInput = 0, kIOHIDReportTypeOutput = 1, kIOHIDReportTypeFeature = 2 } IOHIDReportType;
enum {
    kHIDPage_GenericDesktop = 0x01, kHIDPage_Button = 0x09,
    kHIDUsage_GD_Joystick = 0x04, kHIDUsage_GD_GamePad = 0x05,
    kHIDUsage_GD_X = 0x30, kHIDUsage_GD_Y = 0x31, kHIDUsage_GD_Z = 0x32,
    kHIDUsage_GD_Rz = 0x35, kHIDUsage_GD_Hatswitch = 0x39,
};
#define kIOHIDVendorIDKey "VendorID"
#define kIOHIDProductIDKey "ProductID"
#define kIOHIDProductKey "Product"
#define kIOHIDManufacturerKey "Manufacturer"
#define kIOHIDSerialNumberKey "SerialNumber"
#define kIOHIDLocationIDKey "LocationID"
#define kIOHIDDeviceUsagePageKey "DeviceUsagePage"
#define kIOHIDDeviceUsageKey "DeviceUsage"
typedef struct __IOHIDManager *IOHIDManagerRef;
typedef struct __IOHIDDevice *IOHIDDeviceRef;
typedef struct __IOHIDElement *IOHIDElementRef;
typedef struct __IOHIDValue *IOHIDValueRef;
typedef void (*IOHIDDeviceCallback)(void *context, IOReturn result, void *sender, IOHIDDeviceRef device);
typedef void (*IOHIDReportCallback)(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportID, uint8_t *report, CFIndex reportLength);
typedef void (*IOHIDValueCallback)(void *context, IOReturn result, void *sender, IOHIDValueRef value);
extern IOHIDManagerRef IOHIDManagerCreate(CFAllocatorRef allocator, IOOptionBits options);
extern void IOHIDManagerSetDeviceMatching(IOHIDManagerRef manager, CFDictionaryRef matching);
extern void IOHIDManagerSetDeviceMatchingMultiple(IOHIDManagerRef manager, CFArrayRef multiple);
extern void IOHIDManagerRegisterDeviceMatchingCallback(IOHIDManagerRef manager, IOHIDDeviceCallback callback, void *context);
extern void IOHIDManagerRegisterDeviceRemovalCallback(IOHIDManagerRef manager, IOHIDDeviceCallback callback, void *context);
extern void IOHIDManagerScheduleWithRunLoop(IOHIDManagerRef manager, CFRunLoopRef runLoop, CFStringRef runLoopMode);
extern IOReturn IOHIDManagerOpen(IOHIDManagerRef manager, IOOptionBits options);
extern CFSetRef IOHIDManagerCopyDevices(IOHIDManagerRef manager);
extern CFTypeRef IOHIDDeviceGetProperty(IOHIDDeviceRef device, CFStringRef key);
extern io_service_t IOHIDDeviceGetService(IOHIDDeviceRef device);
extern kern_return_t IORegistryEntryGetRegistryEntryID(io_registry_entry_t entry, uint64_t *entryID);
extern IOReturn IOHIDDeviceOpen(IOHIDDeviceRef device, IOOptionBits options);
extern IOReturn IOHIDDeviceClose(IOHIDDeviceRef device, IOOptionBits options);
extern void IOHIDDeviceScheduleWithRunLoop(IOHIDDeviceRef device, CFRunLoopRef runLoop, CFStringRef runLoopMode);
extern void IOHIDDeviceUnscheduleFromRunLoop(IOHIDDeviceRef device, CFRunLoopRef runLoop, CFStringRef runLoopMode);
extern void IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef device, uint8_t *report, CFIndex reportLength, IOHIDReportCallback callback, void *context);
extern void IOHIDDeviceRegisterInputValueCallback(IOHIDDeviceRef device, IOHIDValueCallback callback, void *context);
extern IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef device, IOHIDReportType reportType, CFIndex reportID, const uint8_t *report, CFIndex reportLength);
extern IOHIDElementRef IOHIDValueGetElement(IOHIDValueRef value);
extern CFIndex IOHIDValueGetIntegerValue(IOHIDValueRef value);
extern uint32_t IOHIDElementGetUsagePage(IOHIDElementRef element);
extern uint32_t IOHIDElementGetUsage(IOHIDElementRef element);
extern CFIndex IOHIDElementGetLogicalMin(IOHIDElementRef element);
extern CFIndex IOHIDElementGetLogicalMax(IOHIDElementRef element);
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateSimpleClient(CFAllocatorRef allocator);
extern CFArrayRef IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef client);
extern CFTypeRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef service, CFStringRef key);
extern Boolean IOHIDServiceClientSetProperty(IOHIDServiceClientRef service, CFStringRef key, CFTypeRef property);
#endif
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "runtime.h"
#include "gamepad_layout.h"   // face buttons by position (PlayStation, Xbox) or label (Nintendo)
#include "controller_map.h"   // the player's button layout (controls.txt)
#include "mouse_look.h"       // mouse mode: movement as the right stick and the gyro

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#if TARGET_OS_OSX
#define KEYBOARD_PHYSICAL_HID 1
#endif
#define IOS_KEYBOARD_SLOTS RUNTIME_MAX_GAMEPADS
#define KEYBOARD_CONFIG_CHANGED() runtime_input_gamepads_changed()
#include "keyboard_ios.inc"
#if TARGET_OS_OSX
#include "keyboard_hid_macos.inc"
#endif

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState gamepadState;
static uint64_t pressedEdges;
static RuntimeGamepadState gamepadStates[RUNTIME_MAX_GAMEPADS];
static uint64_t pressedEdgesSlot[RUNTIME_MAX_GAMEPADS];
static unsigned gamepadConnectedCount;
static GCController *gamepadSlots[RUNTIME_MAX_GAMEPADS];
static BOOL started;
static _Atomic int inputSettingsSuspended;
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
#include "controller_state.h"
static ControllerModeState slotModeState[RUNTIME_MAX_GAMEPADS];
static uint16_t slotMap[RUNTIME_MAX_GAMEPADS][CONTROLLER_BUTTONS];
static uint8_t slotModes[RUNTIME_MAX_GAMEPADS][CONTROLLER_BUTTONS];
static int slotTypes[RUNTIME_MAX_GAMEPADS]; /* main event queue only */
static int slotsInitialized;
static uintptr_t slotOwner[RUNTIME_MAX_GAMEPADS];
static void refresh_gamepads(void);
static void touch_clear_locked(void);

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

/* Controller type (gamepad_layout.h GAMEPAD_TYPE_*), key 6 on the Mac, in controller-type.txt. */
static _Atomic int controllerType = GAMEPAD_TYPE_AUTO;

static void load_controller_type(void) {
    char path[4096], word[64] = {0};
    if (!setting_read("controller-type.txt", word, sizeof word, path, sizeof path)) return;
    int type = gamepad_type_parse(word);
    if (type < 0) { fprintf(stderr, "[totk-input] %s: not a controller type (auto, xbox, playstation, switch, thirdparty-pro); using auto\n", path); return; }
    atomic_store(&controllerType, type);
    fprintf(stderr, "[totk-input] controller type: %s (%s)\n", gamepad_type_name(type), path);
}


#include "input_source.inc"
#define DEVICE_SLOT_ACCEPTS_PAD(s) runtime_input_source_allows((s), INPUT_DEVICE_PAD)
#if TARGET_OS_OSX
static void pro_hid_set(int on);
#define SLOT_BACKEND_CHANGED(slot, previous, next) do {     if (!(slot) && (previous) != (next) && ((previous) == GAMEPAD_TYPE_THIRDPARTY_PRO || (next) == GAMEPAD_TYPE_THIRDPARTY_PRO)) pro_hid_set((next) == GAMEPAD_TYPE_THIRDPARTY_PRO); } while (0)
#endif
#include "controller_slots.inc"
#include "stick_bindings.inc"
#include "controller_devices.inc"

static int face_by_position(GCController *controller) {
    int type = GAMEPAD_TYPE_AUTO;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        if (gamepadSlots[s] == controller) { type = slotTypes[s]; break; }
    return gamepad_type_face_by_position(type, controller.productCategory.UTF8String, controller.vendorName.UTF8String);
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
    /* Some Switch-mode third-party pads expose the d-pad as direction values without setting isPressed. Use both forms. */
    if (pad.dpad.left.isPressed || pad.dpad.left.value < -0.5) buttons |= 1ULL << 12;
    if (pad.dpad.up.isPressed || pad.dpad.up.value > 0.5) buttons |= 1ULL << 13;
    if (pad.dpad.right.isPressed || pad.dpad.right.value > 0.5) buttons |= 1ULL << 14;
    if (pad.dpad.down.isPressed || pad.dpad.down.value < -0.5) buttons |= 1ULL << 15;
    if ([pad isKindOfClass:GCDualSenseGamepad.class] && ((GCDualSenseGamepad *)pad).touchpadButton.isPressed) buttons |= 1ULL << 16;
    if ([pad isKindOfClass:GCDualShockGamepad.class] && ((GCDualShockGamepad *)pad).touchpadButton.isPressed) buttons |= 1ULL << 16;
    return buttons;
}

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
static HIDPadState hidPad[RUNTIME_MAX_GAMEPADS];
static IOHIDManagerRef hidManager;
static pthread_mutex_t hidDeviceLock = PTHREAD_MUTEX_INITIALIZER;
static IOHIDDeviceRef hidDevice[RUNTIME_MAX_GAMEPADS];
static int hidPlayerDevice[RUNTIME_MAX_GAMEPADS] = {[0 ... RUNTIME_MAX_GAMEPADS - 1] = -1}; /* hidDeviceLock */
static int hidClaimFd[RUNTIME_MAX_GAMEPADS] = {[0 ... RUNTIME_MAX_GAMEPADS - 1] = -1};
static int gcClaimFd[RUNTIME_MAX_GAMEPADS] = {[0 ... RUNTIME_MAX_GAMEPADS - 1] = -1};
static BOOL hidRefreshPending;

/* Cross-process controller claim lock: when multiple MacNativeGames apps run at once, each physical controller is
   claimed via an flock() file in /tmp/macnativegames-pads/ so another running game will skip it and take the next
   available controller. */
static NSString *normalize_pad_key(NSString *raw) {
    if (!raw.length) return nil;
    NSString *s = [raw.lowercaseString stringByReplacingOccurrencesOfString:@":" withString:@"-"];
    NSRegularExpression *re = [NSRegularExpression regularExpressionWithPattern:@"([0-9a-f]{2}-[0-9a-f]{2}-[0-9a-f]{2}-[0-9a-f]{2}-[0-9a-f]{2}-[0-9a-f]{2})" options:0 error:nil];
    NSTextCheckingResult *m = [re firstMatchInString:s options:0 range:NSMakeRange(0, s.length)];
    if (m) return [s substringWithRange:[m rangeAtIndex:1]];
    NSMutableString *clean = [NSMutableString string];
    for (NSUInteger i = 0; i < s.length; i++) {
        unichar c = [s characterAtIndex:i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') [clean appendFormat:@"%C", c];
    }
    return clean.length ? clean : nil;
}
static NSString *gc_pad_key(GCController *c) {
    if ([c respondsToSelector:NSSelectorFromString(@"identifier")]) {
        id v = [c valueForKey:@"identifier"];
        NSString *k = normalize_pad_key([v description]);
        if (k) return k;
    }
    if ([c respondsToSelector:NSSelectorFromString(@"deviceHash")]) {
        id v = [c valueForKey:@"deviceHash"];
        NSString *k = normalize_pad_key([v description]);
        if (k) return k;
    }
    return normalize_pad_key(c.vendorName ?: @"pad");
}
static NSString *hid_pad_key(IOHIDDeviceRef dev) {
    NSString *serial = (__bridge NSString *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDSerialNumberKey));
    NSString *k = normalize_pad_key(serial);
    if (k) return k;
    NSNumber *loc = (__bridge NSNumber *)IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDLocationIDKey));
    if (loc) return [NSString stringWithFormat:@"loc-%@", loc];
    io_service_t srv = IOHIDDeviceGetService(dev);
    uint64_t regID = 0;
    if (srv && IORegistryEntryGetRegistryEntryID(srv, &regID) == KERN_SUCCESS && regID)
        return [NSString stringWithFormat:@"reg-%llx", regID];
    return @"hid-pad";
}
static int pad_claim_acquire(NSString *key) {
#if !TARGET_OS_OSX
    (void)key;
    return 0;
#else
    if (!key.length) return -1;
    mkdir("/tmp/macnativegames-pads", 0777);
    char path[512];
    snprintf(path, sizeof path, "/tmp/macnativegames-pads/%s.lock", key.UTF8String);
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
#endif
}
static void pad_claim_release(int *fd) {
#if !TARGET_OS_OSX
    *fd = -1;
#else
    if (*fd >= 0) {
        flock(*fd, LOCK_UN);
        close(*fd);
        *fd = -1;
    }
#endif
}
static unsigned char hidButtons[RUNTIME_MAX_GAMEPADS][16];   /* usage 1..15, 1 = pressed */
static int hidHat[RUNTIME_MAX_GAMEPADS];                     /* 0 N, 1 NE, 2 E ... 7 NW; 8 centred */
static float hidX[RUNTIME_MAX_GAMEPADS], hidY[RUNTIME_MAX_GAMEPADS], hidZ[RUNTIME_MAX_GAMEPADS], hidRz[RUNTIME_MAX_GAMEPADS]; /* -1..1, 0 centred */

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

/* Third-party Pro (Betop Zeus, 057e:2009) is read entirely from its own report 0x30; GameController is not used for
   it. Standard Switch Pro report 0x30 layout:
   byte 3: bit3 A, bit2 B, bit1 X, bit0 Y, bit6 R, bit7 ZR;
   byte 4: bit0 minus, bit1 plus, bit2 R3, bit3 L3;
   byte 5: bit0 d-pad down, bit1 d-pad up, bit2 d-pad right, bit3 d-pad left, bit6 L, bit7 ZL;
   bytes 6..11: two 12-bit sticks (2048 centred); bytes 13..48: three 6-axis IMU frames. */
static int32_t pro_axis(int v) { float f = (v - 2048) / 2048.0f; if (f > 1) f = 1; else if (f < -1) f = -1; return stick_axis(f); }
static uint8_t proReport[RUNTIME_MAX_GAMEPADS][64];
static uint8_t proPacket[RUNTIME_MAX_GAMEPADS];
static void pro_subcommand(int slot, IOHIDDeviceRef device, uint8_t cmd, uint8_t arg) {
    uint8_t out[49] = {0x01, proPacket[slot]++ & 15, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, cmd, arg};
    IOReturn r = IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, 0x01, out, sizeof out);
    fprintf(stderr, "[totk-input] third-party Pro slot %d subcommand 0x%02x 0x%02x: %s\n",
            slot, cmd, arg, r == kIOReturnSuccess ? "ok" : "failed");
}
void runtime_gamepad_hid_rumble_send(int slot, const uint8_t *out, size_t len) {
    if (slot < 0 || slot >= RUNTIME_MAX_GAMEPADS || !out || len < 2) return;
    pthread_mutex_lock(&hidDeviceLock);
    int device = hidPlayerDevice[slot];
    IOHIDDeviceRef dev = device >= 0 && hidDevice[device] ? (IOHIDDeviceRef)CFRetain(hidDevice[device]) : NULL;
    pthread_mutex_unlock(&hidDeviceLock);
    if (!dev) return;
    IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, out[0], out, (CFIndex)len);
    CFRelease(dev);
}
static void pro_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *p, CFIndex len) {
    (void)sender; (void)type;
    int slot = (int)(intptr_t)ctx;
    if (slot < 0 || slot >= RUNTIME_MAX_GAMEPADS) return;
    if (result != kIOReturnSuccess || id != 0x30 || len < 12) return;
    uint64_t buttons = gamepad_face_positions(p[3] & 4, p[3] & 8, p[3] & 1, p[3] & 2, 1);   /* south=B(bit2), east=A(bit3), west=Y(bit0), north=X(bit1) */
    if (p[4] & 8) buttons |= 1ULL << 4;     /* l3 */
    if (p[4] & 4) buttons |= 1ULL << 5;     /* r3 */
    if (p[5] & 64) buttons |= 1ULL << 6;    /* l */
    if (p[3] & 64) buttons |= 1ULL << 7;    /* r */
    if (p[5] & 128) buttons |= 1ULL << 8;   /* zl */
    if (p[3] & 128) buttons |= 1ULL << 9;   /* zr */
    if (p[4] & 2) buttons |= 1ULL << 10;    /* plus */
    if (p[4] & 1) buttons |= 1ULL << 11;    /* minus */
    if (p[5] & 8) buttons |= 1ULL << 12;    /* d-pad left */
    if (p[5] & 2) buttons |= 1ULL << 13;    /* d-pad up */
    if (p[5] & 4) buttons |= 1ULL << 14;    /* d-pad right */
    if (p[5] & 1) buttons |= 1ULL << 15;    /* d-pad down */
    int lx = p[6] | (p[7] & 0x0f) << 8, ly = (p[7] >> 4) | p[8] << 4;
    int rx = p[9] | (p[10] & 0x0f) << 8, ry = (p[10] >> 4) | p[11] << 4;
    hidPad[slot].present = 1;
    hidPad[slot].buttons = buttons;
    hidPad[slot].lx = pro_axis(lx); hidPad[slot].ly = pro_axis(ly);
    hidPad[slot].rx = pro_axis(rx); hidPad[slot].ry = pro_axis(ry);
    pthread_mutex_lock(&hidDeviceLock);
    int motionOwner = -1;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        if (hidPlayerDevice[s] == slot) motionOwner = s;
    pthread_mutex_unlock(&hidDeviceLock);
    if (motionOwner >= 0) runtime_gamepad_hid_motion((unsigned)motionOwner, p, (int)len);
    refresh_gamepads();
}

static void hid_pad_compute(int slot) {   /* generic pads macOS does not adopt: buttons and sticks from their own descriptor */
    uint64_t buttons = gamepad_face_positions(hidButtons[slot][1], hidButtons[slot][2], hidButtons[slot][4], hidButtons[slot][5], 1);
    if (hidButtons[slot][14]) buttons |= 1ULL << 4; if (hidButtons[slot][15]) buttons |= 1ULL << 5;
    if (hidButtons[slot][7]) buttons |= 1ULL << 6; if (hidButtons[slot][8]) buttons |= 1ULL << 7;
    if (hidButtons[slot][9]) buttons |= 1ULL << 8; if (hidButtons[slot][10]) buttons |= 1ULL << 9;
    if (hidButtons[slot][12]) buttons |= 1ULL << 10; if (hidButtons[slot][11]) buttons |= 1ULL << 11;
    switch (hidHat[slot]) {
    case 0: buttons |= 1ULL << 13; break; case 2: buttons |= 1ULL << 14; break;
    case 4: buttons |= 1ULL << 15; break; case 6: buttons |= 1ULL << 12; break;
    case 1: buttons |= (1ULL << 13) | (1ULL << 14); break; case 3: buttons |= (1ULL << 14) | (1ULL << 15); break;
    case 5: buttons |= (1ULL << 15) | (1ULL << 12); break; case 7: buttons |= (1ULL << 12) | (1ULL << 13); break;
    default: break; }
    hidPad[slot].present = 1; hidPad[slot].buttons = buttons;
    hidPad[slot].lx = stick_axis(hidX[slot]); hidPad[slot].ly = stick_axis(-hidY[slot]);
    hidPad[slot].rx = stick_axis(hidZ[slot]); hidPad[slot].ry = stick_axis(-hidRz[slot]);
}

static void hid_value(void *context, IOReturn result, void *sender, IOHIDValueRef value) {
    (void)result; (void)sender;
    int slot = (int)(intptr_t)context;
    if (slot < 0 || slot >= RUNTIME_MAX_GAMEPADS) return;
    IOHIDElementRef element = IOHIDValueGetElement(value);
    uint32_t page = IOHIDElementGetUsagePage(element), usage = IOHIDElementGetUsage(element);
    int v = (int)IOHIDValueGetIntegerValue(value);
    if (page == kHIDPage_Button && usage >= 1 && usage <= 15) {
        hidButtons[slot][usage] = (unsigned char)(v != 0);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Hatswitch) {
        hidHat[slot] = hid_hat(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_X) {
        hidX[slot] = hid_axis(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Y) {
        hidY[slot] = hid_axis(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Z) {
        hidZ[slot] = hid_axis(element, v);
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Rz) {
        hidRz[slot] = hid_axis(element, v);
    } else {
        return;
    }
    hid_pad_compute(slot);
    /* One report delivers a value per element: coalesce them into one refresh on the next run loop turn. */
    if (!hidRefreshPending) {
        hidRefreshPending = YES;
        dispatch_async(dispatch_get_main_queue(), ^{ hidRefreshPending = NO; refresh_gamepads(); });
    }
}

static void hid_pad_reset_slot(int slot) {
    for (unsigned s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        if (hidPlayerDevice[s] == slot) runtime_gamepad_hid_motion_reset(s);
    memset(hidButtons[slot], 0, sizeof hidButtons[slot]);
    hidHat[slot] = 8;
    hidX[slot] = hidY[slot] = hidZ[slot] = hidRz[slot] = 0;
    hidPad[slot] = (HIDPadState){0};
}

static void hid_pad_reset(void) {
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) hid_pad_reset_slot(s);
}

static void hid_pad_detach_slot(int slot) {
    if (slot < 0 || slot >= RUNTIME_MAX_GAMEPADS || !hidDevice[slot]) return;
    pthread_mutex_lock(&hidDeviceLock);
    IOHIDDeviceRef dev = hidDevice[slot];
    hidDevice[slot] = NULL;
    pthread_mutex_unlock(&hidDeviceLock);
    IOHIDDeviceUnscheduleFromRunLoop(dev, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
    CFRelease(dev);
    pad_claim_release(&hidClaimFd[slot]);
    hid_pad_reset_slot(slot);
    fprintf(stderr, "[totk-input] HID gamepad slot %d disconnected\n", slot);
    refresh_gamepads();
}

/* Raw Pro discovery must not seize controllers for disabled player inputs.
   Otherwise a one-player process can hold every pad, even though routing drops
   the unused ones. Keep existing owners stable and let the periodic rescan in
   other processes acquire released devices. Main run loop only. */
static int pro_hid_capacity(void) {
    int capacity = 0;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        if (device_slot_auto(s)) ++capacity;
    return capacity;
}
static int pro_hid_has_room(void) {
    int owned = 0;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        if (hidDevice[s]) ++owned;
    return owned < pro_hid_capacity();
}
static void pro_hid_release_unused(void) {
    int keep = pro_hid_capacity();
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        if (!hidDevice[s]) continue;
        if (keep > 0) --keep;
        else hid_pad_detach_slot(s);
    }
}

static void hid_pad_attach(IOHIDDeviceRef device) {
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        if (hidDevice[s] && CFEqual(hidDevice[s], device)) return;
    }
    int slot = -1;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        if (!hidDevice[s]) { slot = s; break; }
    }
    if (slot < 0) return;
    /* Absent means GameController may well own it; only a definite No says the pad is unreachable otherwise. The flag also
       reads No on pads GameController does adopt (a Bluetooth Switch Pro Controller), so Nintendo, Sony and Microsoft
       devices are left to it. */
    CFTypeRef vendorId = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    int vid = 0, pid = 0;
    if (vendorId && CFGetTypeID(vendorId) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)vendorId, kCFNumberIntType, &vid);
    CFTypeRef productId = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    if (productId && CFGetTypeID(productId) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)productId, kCFNumberIntType, &pid);
    const char *prodName = NULL;
    CFTypeRef prodProp = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    if (prodProp && CFGetTypeID(prodProp) == CFStringGetTypeID()) prodName = [(__bridge NSString *)prodProp UTF8String];
    if (vid == 0x054c || vid == 0x045e) return;
    int isThirdPartyMode = (atomic_load(&controllerType) == GAMEPAD_TYPE_THIRDPARTY_PRO);
    int thirdparty = isThirdPartyMode && (vid == 0x057e || (prodName && (strstr(prodName, "Pro Controller") || strstr(prodName, "Switch"))));
    if (vid == 0x057e && !thirdparty) return;
    /* A third-party Pro is adopted by GameController, which would normally make us skip it; in that mode we read it
       ourselves, so the supported flag is ignored. */
    if (!thirdparty && !isThirdPartyMode) {
        CFTypeRef supported = IOHIDDeviceGetProperty(device, CFSTR("GameControllerSupportedHIDDevice"));
        if (!supported || CFGetTypeID(supported) != CFBooleanGetTypeID() || CFBooleanGetValue((CFBooleanRef)supported)) return;
    }
    if (isThirdPartyMode && !pro_hid_has_room()) return;
    /* Release any GCController claim we held for this same physical controller so we can claim it as HID, or skip if another game holds it. */
    NSString *key = hid_pad_key(device);
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        if (gamepadSlots[s] && [gc_pad_key(gamepadSlots[s]) isEqualToString:key]) {
            pad_claim_release(&gcClaimFd[s]);
            slot = s;
            break;
        }
    }
    int claimFd = pad_claim_acquire(key);
    if (claimFd < 0) {
        fprintf(stderr, "[totk-input] HID gamepad (%s) is already in use by another game; skipping\n", key.UTF8String);
        return;
    }
    IOReturn opened = kIOReturnError;
    if (thirdparty) {
        opened = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
        fprintf(stderr, "[totk-input] third-party Pro slot %d seize: %s\n", slot, opened == kIOReturnSuccess ? "ok" : "failed");
        if (opened != kIOReturnSuccess) opened = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    } else {
        opened = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    }
    if (opened != kIOReturnSuccess) {
        pad_claim_release(&claimFd);
        return;
    }

    hidClaimFd[slot] = claimFd;
    pthread_mutex_lock(&hidDeviceLock);
    hidDevice[slot] = (IOHIDDeviceRef)CFRetain(device);
    pthread_mutex_unlock(&hidDeviceLock);
    const char *name = NULL, *manufacturer = NULL;
    CFTypeRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    if (product && CFGetTypeID(product) == CFStringGetTypeID()) {
        name = [(__bridge NSString *)product UTF8String];
    }
    CFTypeRef vendor = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDManufacturerKey));
    if (vendor && CFGetTypeID(vendor) == CFStringGetTypeID()) {
        manufacturer = [(__bridge NSString *)vendor UTF8String];
    }
    hid_pad_reset_slot(slot);
    if (thirdparty) {
        IOHIDDeviceRegisterInputReportCallback(device, proReport[slot], sizeof proReport[slot], pro_report, (void *)(intptr_t)slot);
        IOHIDDeviceRegisterInputValueCallback(device, hid_value, (void *)(intptr_t)slot);
        /* Same init as Ryujinx/SDL HIDAPI: full 0x30 reports, then IMU + player LEDs + vibration enable. */
        pro_subcommand(slot, device, 0x03, 0x30);
        pro_subcommand(slot, device, 0x30, (uint8_t)(1u << (slot & 3)));
        pro_subcommand(slot, device, 0x48, 0x01);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (hidDevice[slot]) pro_subcommand(slot, hidDevice[slot], 0x40, 0x01);
        });
    } else {
        IOHIDDeviceRegisterInputValueCallback(device, hid_value, (void *)(intptr_t)slot);
    }
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    hidPad[slot].present = 1;
    fprintf(stderr, "[totk-input] HID gamepad slot %d connected: %s (%s)\n", slot, name ? name : "unknown", manufacturer ? manufacturer : "unknown");
    refresh_gamepads();
}

static void hid_matched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)sender;
    if (result == kIOReturnSuccess) hid_pad_attach(device);
}

static void hid_removed(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        if (hidDevice[s] && CFEqual(hidDevice[s], device)) hid_pad_detach_slot(s);
    }
}

/* Switching to or from thirdparty-pro (key 6) attaches or detaches the 057e:2009 HID devices on the fly. */
static void pro_hid_set(int on) {
    if (!hidManager) return;
    if (!on) {
        for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
            if (hidDevice[s]) hid_pad_detach_slot(s);
        }
        return;
    }
    pro_hid_release_unused();
    NSSet *devices = (__bridge_transfer NSSet *)IOHIDManagerCopyDevices(hidManager);
    for (id obj in devices) hid_pad_attach((__bridge IOHIDDeviceRef)obj);
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

/* ---- Mouse mode (Mac, key 7; in mouse.txt) ----
   The window (runtime/frontend.m) holds the pointer while mouse mode is on and the window has the keyboard, and sends
   the mouse's buttons and movement here. Buttons are physical buttons 17-21 of the controller layout (controller_map.h:
   left attack, right draw the bow with left to shoot, middle target), so controls.txt can change them. Movement goes to
   runtime/mouse_look.h, ticked every 10 ms by a timer while mouse mode is on: the right stick (the stronger of it and a
   controller's) and, unless TOTK_MOUSE_GYRO=0 or the game has no six-axis device (TOTK_GYRO=0), the gyro, which the
   mouse then has to itself (runtime_motion_mouse). Settings: TOTK_MOUSE_SENSITIVITY 1-100 (50), TOTK_MOUSE_INVERT_Y=1,
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
    int feed = mouseGyro && runtime_input_source_allows(0,INPUT_DEVICE_MOUSE) && !atomic_load(&inputSettingsSuspended);
    pthread_mutex_unlock(&gamepadLock);
    if (feed) runtime_motion_feed_mouse(acc, gyro, runtime_now_ns());
}

static void mouse_apply(int on) {   /* start or stop the ticks and the gyro takeover */
    if (on) {
        const char *g = getenv("TOTK_MOUSE_GYRO");
        MouseLookSettings settings = mouse_look_settings(getenv("TOTK_MOUSE_SENSITIVITY"), getenv("TOTK_MOUSE_INVERT_Y"),
                                                         getenv("TOTK_MOUSE_COUNTERWEIGHT"), getenv("TOTK_MOUSE_DECAY"), getenv("TOTK_MOUSE_MIN_DECAY"));
        int gyro = (atomic_load(&gyroSources[0])==0||atomic_load(&gyroSources[0])==2) && runtime_gyro_enabled() && runtime_input_source_allows(0,INPUT_DEVICE_MOUSE) && !(g && !strcmp(g, "0"));
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

void runtime_mouse_refresh_source(void) { mouse_apply(atomic_load(&mouseMode)); }

static void load_mouse_mode(void) {
    char path[4096], word[16] = {0};
    if (!setting_read("mouse.txt", word, sizeof word, path, sizeof path)) return;
    int on = !strcmp(word, "on\n") || !strcmp(word, "on");
    if (!on && strcmp(word, "off\n") && strcmp(word, "off")) fprintf(stderr, "[totk-input] %s: not on or off; mouse mode off\n", path);
    if (on != atomic_load(&mouseMode)) { atomic_store(&mouseMode, on); mouse_apply(on); }
}

int runtime_mouse_enabled(void) { return atomic_load(&mouseMode); }
void runtime_mouse_set_hook(void (*hook)(void)) { mouseHook = hook; }

/* Key 7 (runtime/input.c). */
int runtime_mouse_toggle(void) {
    int on = !atomic_load(&mouseMode);
    atomic_store(&mouseMode, on);
    setting_write("mouse.txt", on ? "on" : "off", "mouse mode");
    if (!on) dispatch_async(dispatch_get_main_queue(), ^{ runtime_mouse_release(); });
    mouse_apply(on);
    return on;
}

void runtime_mouse_move(double dx, double dy) {   /* the window, while it holds the pointer */
    if (!atomic_load(&mouseMode) || atomic_load(&inputSettingsSuspended) || !runtime_input_source_allows(0,INPUT_DEVICE_MOUSE)) return;
    pthread_mutex_lock(&gamepadLock);
    mouse_look_move(&mouseLook, dx, dy, &mouseSettings);
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_mouse_button(int button, int down) {   /* main thread */
    if (atomic_load(&inputSettingsSuspended) || !runtime_input_source_allows(0,INPUT_DEVICE_MOUSE)) return;
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

static void refresh_gamepads(void);

#if !TARGET_OS_OSX
@interface NSObject (RuntimeGCHIDServiceInfo)
- (NSArray *)hidServices;
- (NSArray *)components;
- (void)setProperty:(id)value forKey:(NSString *)key;
- (id)propertyForKey:(NSString *)key;
- (IOHIDServiceClientRef)service;
@end

/* Opt-in device probe: never assumes that desktop HID access is available on iOS.
   GCHIDInformation is a component on newer OS versions, not a hidServices entry.
   The HID mode experiment is explicit because report 0x3f does not carry IMU data. */
static void ios_controller_probe(GCController *controller) {
    if (!getenv("TOTK_IOS_INPUT_PROBE")) return;
    NSArray *services = [controller respondsToSelector:@selector(hidServices)] ? ([controller hidServices] ?: @[]) : @[];
    NSArray *components = [controller respondsToSelector:@selector(components)] ? ([controller components] ?: @[]) : @[];
    fprintf(stderr, "[input-probe] controller=%s services=%lu components=%lu\n",
            controller.productCategory.UTF8String, (unsigned long)services.count, (unsigned long)components.count);
    GCPhysicalInputProfile *profile = controller.physicalInputProfile;
    fprintf(stderr, "[input-probe] remapped=%d left-alias=%s axes=%lu dpads=%lu\n",
            profile.hasRemappedElements,
            [profile mappedElementAliasForPhysicalInputName:GCInputLeftThumbstick].UTF8String ?: "none",
            (unsigned long)profile.allAxes.count, (unsigned long)profile.allDpads.count);
    /* Poll all public axes, not just callbacks: distinguish an unmoving left stick from
       a missed callback, an alternate axis, or an app-side deadzone. Opt-in only. */
    __weak GCController *weakController = controller;
    NSMutableDictionary<NSString *, NSArray<NSNumber *> *> *ranges = [NSMutableDictionary new];
    __block unsigned pollCount = 0;
    [NSTimer scheduledTimerWithTimeInterval:0.02 repeats:YES block:^(NSTimer *timer) {
        GCController *c = weakController;
        if (!c || ![GCController.controllers containsObject:c]) { [timer invalidate]; return; }
        GCPhysicalInputProfile *p = c.physicalInputProfile;
        for (NSString *key in p.axes) {
            float v = p.axes[key].value;
            NSArray<NSNumber *> *r = ranges[key];
            ranges[key] = @[@(r ? fminf(r[0].floatValue, v) : v), @(r ? fmaxf(r[1].floatValue, v) : v)];
        }
        if (c.motion.hasRotationRate && c.motion.sensorsRequireManualActivation) c.motion.sensorsActive = YES;
        if (++pollCount % 50) return;
        GCRotationRate rate = c.motion.rotationRate;
        fprintf(stderr, "[input-probe] motion=%d rotation=%d rate=(%.5f,%.5f,%.5f)\n",
                c.motion != nil, c.motion.hasRotationRate, rate.x, rate.y, rate.z);
        GCExtendedGamepad *pad = c.extendedGamepad;
        fprintf(stderr, "[input-probe] poll=%u left=(%.4f,%.4f) right=(%.4f,%.4f) dpad=(%.1f,%.1f) A=%d L3=%d event-time=%.3f\n",
                pollCount, pad.leftThumbstick.xAxis.value, pad.leftThumbstick.yAxis.value,
                pad.rightThumbstick.xAxis.value, pad.rightThumbstick.yAxis.value,
                pad.dpad.xAxis.value, pad.dpad.yAxis.value, pad.buttonA.isPressed,
                pad.leftThumbstickButton.isPressed, p.lastEventTimestamp);
        for (NSString *key in [p.axes.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            NSArray<NSNumber *> *r = ranges[key];
            fprintf(stderr, "[input-probe] axis=%s value=%.4f range=[%.4f,%.4f]\n",
                    key.UTF8String, p.axes[key].value, r[0].floatValue, r[1].floatValue);
        }
    }];
    __block float minX = 0, maxX = 0, minY = 0, maxY = 0;
    __block unsigned samples = 0;
    controller.extendedGamepad.leftThumbstick.valueChangedHandler = ^(GCControllerDirectionPad *stick, float x, float y) {
        (void)stick; samples++;
        minX = fminf(minX, x); maxX = fmaxf(maxX, x); minY = fminf(minY, y); maxY = fmaxf(maxY, y);
        if (samples == 1 || samples % 60 == 0)
            fprintf(stderr, "[input-probe] left-stick events=%u x=[%.3f,%.3f] y=[%.3f,%.3f]\n", samples, minX, maxX, minY, maxY);
    };
    for (id item in [services arrayByAddingObjectsFromArray:components]) {
        fprintf(stderr, "[input-probe] component=%s property-read=%d property-write=%d\n",
                NSStringFromClass([item class]).UTF8String,
                [item respondsToSelector:@selector(propertyForKey:)],
                [item respondsToSelector:@selector(setProperty:forKey:)]);
        if ([item respondsToSelector:@selector(propertyForKey:)]) {
            id mode = [item propertyForKey:@"JoyConInputMode"];
            fprintf(stderr, "[input-probe] input-mode=%s\n", [mode description].UTF8String ?: "unavailable");
        }
        const char *mode = getenv("TOTK_IOS_PRO_INPUT_MODE");
        if (mode && (!strcmp(mode, "HID") || !strcmp(mode, "STANDARD-FULL")) &&
            [controller.productCategory isEqualToString:@"Switch Pro Controller"] &&
            [NSStringFromClass([item class]) isEqualToString:@"GCHIDInformation"] &&
            [item respondsToSelector:@selector(setProperty:forKey:)]) {
            [(NSObject *)item setProperty:@(mode) forKey:@"JoyConInputMode"];
            fprintf(stderr, "[input-probe] requested %s (not confirmation of a mode change)\n", mode);
        }
    }
}

static int ios_joycon_raw_sticks(GCController *controller, int32_t *outLx, int32_t *outLy, int32_t *outRx, int32_t *outRy) {
    if (!controller) return 0;
    NSDictionary *debugDict = nil;
    if ([controller respondsToSelector:@selector(hidServices)]) {
        NSArray *services = [controller hidServices];
        for (id info in services) {
            if ([info respondsToSelector:@selector(propertyForKey:)]) {
                id val = [info propertyForKey:@"ServicePluginDebug"];
                if ([val isKindOfClass:[NSDictionary class]]) { debugDict = val; break; }
            }
            if ([info respondsToSelector:@selector(service)]) {
                IOHIDServiceClientRef svc = [info service];
                if (svc) {
                    CFTypeRef cfVal = IOHIDServiceClientCopyProperty(svc, CFSTR("ServicePluginDebug"));
                    if (cfVal) {
                        if ([(__bridge id)cfVal isKindOfClass:[NSDictionary class]])
                            debugDict = (__bridge_transfer NSDictionary *)cfVal;
                        else
                            CFRelease(cfVal);
                        if (debugDict) break;
                    }
                }
            }
        }
    }
    if (!debugDict) {
        static IOHIDEventSystemClientRef sysClient = NULL;
        static dispatch_once_t onceToken;
        dispatch_once(&onceToken, ^{
            sysClient = IOHIDEventSystemClientCreateSimpleClient(kCFAllocatorDefault);
        });
        if (sysClient) {
            CFArrayRef services = IOHIDEventSystemClientCopyServices(sysClient);
            if (services) {
                CFIndex count = CFArrayGetCount(services);
                for (CFIndex i = 0; i < count; i++) {
                    IOHIDServiceClientRef svc = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
                    CFTypeRef cfVal = IOHIDServiceClientCopyProperty(svc, CFSTR("ServicePluginDebug"));
                    if (cfVal) {
                        if ([(__bridge id)cfVal isKindOfClass:[NSDictionary class]]) {
                            NSDictionary *d = (__bridge_transfer NSDictionary *)cfVal;
                            if (d[@"JoyConStatistics"]) { debugDict = d; break; }
                        } else {
                            CFRelease(cfVal);
                        }
                    }
                }
                CFRelease(services);
            }
        }
    }
    if (!debugDict) return 0;
    static int loggedDebug = 0;
    if (!loggedDebug) {
        loggedDebug = 1;
        fprintf(stderr, "[totk-input] iOS JoyCon ServicePluginDebug: %s\n", debugDict.description.UTF8String ?: "");
    }
    NSDictionary *stats = debugDict[@"JoyConStatistics"];
    if (![stats isKindOfClass:[NSDictionary class]]) return 0;
    int got = 0;
    NSDictionary *left = stats[@"LeftStick"];
    if ([left isKindOfClass:[NSDictionary class]]) {
        NSNumber *lxNum = left[@"x.last"];
        NSNumber *lyNum = left[@"y.last"];
        if (lxNum && lyNum) {
            int lx = lxNum.intValue;
            int ly = lyNum.intValue;
            if ((lx > 0 && lx < 4096) || (ly > 0 && ly < 4096)) {
                int32_t ax = pro_axis(lx);
                int32_t ay = pro_axis(ly);
                /* Apply a ~10% inner deadzone (3277 / 32767) for uncalibrated 12-bit sticks */
                if (ax > -3277 && ax < 3277) ax = 0;
                if (ay > -3277 && ay < 3277) ay = 0;
                if (outLx) *outLx = ax;
                if (outLy) *outLy = ay;
                got = 1;
            }
        }
    }
    NSDictionary *right = stats[@"RightStick"];
    if ([right isKindOfClass:[NSDictionary class]]) {
        NSNumber *rxNum = right[@"x.last"];
        NSNumber *ryNum = right[@"y.last"];
        if (rxNum && ryNum) {
            int rx = rxNum.intValue;
            int ry = ryNum.intValue;
            if ((rx > 0 && rx < 4096) || (ry > 0 && ry < 4096)) {
                int32_t ax = pro_axis(rx);
                int32_t ay = pro_axis(ry);
                if (ax > -3277 && ax < 3277) ax = 0;
                if (ay > -3277 && ay < 3277) ay = 0;
                if (outRx) *outRx = ax;
                if (outRy) *outRy = ay;
                got = 1;
            }
        }
    }
    return got;
}
#endif

static void refresh_gamepads(void) {
    stick_bindings_load();
    RuntimeGamepadState next = {0};
    RuntimeGamepadState nextSlots[RUNTIME_MAX_GAMEPADS] = {{0}};
    uintptr_t nextOwner[RUNTIME_MAX_GAMEPADS] = {0};
    int nextHidDevice[RUNTIME_MAX_GAMEPADS] = {[0 ... RUNTIME_MAX_GAMEPADS - 1] = -1};
    uint64_t slotPhysical[RUNTIME_MAX_GAMEPADS] = {runtime_input_source_allows(0,INPUT_DEVICE_MOUSE) ? (uint64_t)mouseButtons << CONTROLLER_MOUSE_FIRST : 0};
#if TARGET_OS_OSX
    int thirdparty = atomic_load(&controllerType) == GAMEPAD_TYPE_THIRDPARTY_PRO;
#else
    int thirdparty = 0;
#endif
    NSMutableArray *available = [NSMutableArray array];
    if (!thirdparty) for (GCController *controller in device_list()) {
        BOOL owned = NO, rawOwned = NO;
        for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) owned |= gamepadSlots[s] == controller;
        NSString *key = gc_pad_key(controller);
        for (int h = 0; h < RUNTIME_MAX_GAMEPADS; ++h)
            if (hidDevice[h] && [hid_pad_key(hidDevice[h]) isEqual:key]) rawOwned = YES;
        if (rawOwned) continue;
        if (!owned) {
            int probe = pad_claim_acquire(key);
            if (probe < 0) continue;
            pad_claim_release(&probe);
        }
        [available addObject:controller];
    }
    NSArray *desired = device_resolve(available);
    BOOL assignmentChanged = NO;
    /* Detach every old owner before attaching any new one (including swaps).
       Feedback detaches/attaches are queued in this order on the rumble queue. */
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        GCController *wanted = desired[s] == NSNull.null ? nil : desired[s];
        if (gamepadSlots[s] && gamepadSlots[s] != wanted) {
            runtime_feedback_quiet();
            runtime_feedback_detach(gamepadSlots[s]);
            gamepadSlots[s].playerIndex = GCControllerPlayerIndexUnset;
            gamepadSlots[s] = nil;
            slot_clear(s); pad_claim_release(&gcClaimFd[s]); assignmentChanged = YES;
        }
    }
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        GCController *controller = desired[s] == NSNull.null ? nil : desired[s];
        if (!controller || gamepadSlots[s] == controller) continue;
        NSString *key = gc_pad_key(controller);
        int alreadyHid = 0;
        for (int h = 0; h < RUNTIME_MAX_GAMEPADS; ++h)
            if (hidDevice[h] && [hid_pad_key(hidDevice[h]) isEqual:key]) { alreadyHid = 1; break; }
        if (alreadyHid) continue;
        int claimFd = pad_claim_acquire(key);
        if (claimFd < 0) continue;
        gcClaimFd[s] = claimFd; slot_clear(s); gamepadSlots[s] = controller;
        controller.playerIndex = (GCControllerPlayerIndex)s;
        if (@available(macOS 11.0, *)) {
            static const float rgb[4][3] = {{0,0.4f,1},{1,0.15f,0.15f},{0.1f,0.8f,0.2f},{1,0.8f,0.1f}};
            if (controller.light) controller.light.color = [[GCColor alloc] initWithRed:rgb[s][0] green:rgb[s][1] blue:rgb[s][2]];
        }
        runtime_feedback_set_slot(controller, s);
        if (s == 0) runtime_feedback_attach(controller); /* motion backend is player 1 only */
        assignmentChanged = YES;
    }
    unsigned count = 0;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        GCController *controller = gamepadSlots[s];
        if (!controller) continue;
        runtime_feedback_set_slot(controller, s);
        if (thirdparty && hidPad[s].present) continue;
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        uint64_t pbits = gamepad_buttons(pad, face_by_position(controller));
        int32_t lx = stick_axis(pad.leftThumbstick.xAxis.value);
        int32_t ly = stick_axis(pad.leftThumbstick.yAxis.value);
        int32_t rx = stick_axis(pad.rightThumbstick.xAxis.value);
        int32_t ry = stick_axis(pad.rightThumbstick.yAxis.value);
#if !TARGET_OS_OSX
        int32_t jlx = 0, jly = 0, jrx = 0, jry = 0;
        if (ios_joycon_raw_sticks(controller, &jlx, &jly, &jrx, &jry)) {
            int tp = slotTypes[s] == GAMEPAD_TYPE_THIRDPARTY_PRO;
            if (tp || (lx == 0 && ly == 0)) {
                lx = stronger_axis(lx, jlx);
                ly = stronger_axis(ly, jly);
            }
            if (tp || (rx == 0 && ry == 0)) {
                rx = stronger_axis(rx, jrx);
                ry = stronger_axis(ry, jry);
            }
        }
#endif
        nextSlots[s].connected = 1;
        slotPhysical[s] |= pbits;
        nextOwner[s] = (uintptr_t)(__bridge void *)controller;
        nextSlots[s].lx = lx; nextSlots[s].ly = ly;
        nextSlots[s].rx = rx; nextSlots[s].ry = ry;
        if ((unsigned)(s + 1) > count) count = (unsigned)(s + 1);
        if (s == 0) {
            next.connected = 1;

            next.lx = stronger_axis(next.lx, lx);
            next.ly = stronger_axis(next.ly, ly);
            next.rx = stronger_axis(next.rx, rx);
            next.ry = stronger_axis(next.ry, ry);
        }
    }
    for (int h = 0; h < RUNTIME_MAX_GAMEPADS; h++) {
        if (!hidPad[h].present) continue;
        int s = -1;
        if (device_slot_auto(h) && !nextSlots[h].connected) {
            s = h;
        } else {
            for (int k = 0; k < RUNTIME_MAX_GAMEPADS; k++) {
                if (device_slot_auto(k) && !nextSlots[k].connected) { s = k; break; }
            }
        }
        if (s < 0) continue;
        nextSlots[s].connected = 1;
        slotPhysical[s] |= hidPad[h].buttons;
        nextOwner[s] = (uintptr_t)hidDevice[h];
        nextHidDevice[s] = h;
        nextSlots[s].lx = stronger_axis(nextSlots[s].lx, hidPad[h].lx);
        nextSlots[s].ly = stronger_axis(nextSlots[s].ly, hidPad[h].ly);
        nextSlots[s].rx = stronger_axis(nextSlots[s].rx, hidPad[h].rx);
        nextSlots[s].ry = stronger_axis(nextSlots[s].ry, hidPad[h].ry);
        if ((unsigned)(s + 1) > count) count = (unsigned)(s + 1);
        if (s == 0) {
            next.connected = 1;

            next.lx = stronger_axis(next.lx, hidPad[h].lx);
            next.ly = stronger_axis(next.ly, hidPad[h].ly);
            next.rx = stronger_axis(next.rx, hidPad[h].rx);
            next.ry = stronger_axis(next.ry, hidPad[h].ry);
        }
    }
    /* Rumble addresses logical players, not the HID discovery-array index.
       Stop the old raw device before publishing a changed route. */
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        pthread_mutex_lock(&hidDeviceLock);
        int old = hidPlayerDevice[s];
        IOHIDDeviceRef previous = thirdparty && old >= 0 && old != nextHidDevice[s] && hidDevice[old]
            ? (IOHIDDeviceRef)CFRetain(hidDevice[old]) : NULL;
        hidPlayerDevice[s] = nextHidDevice[s];
        pthread_mutex_unlock(&hidDeviceLock);
        if (previous) {
            const uint8_t quiet[10] = {0x10, 0, 0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40};
            IOHIDDeviceSetReport(previous, kIOHIDReportTypeOutput, 0x10, quiet, sizeof quiet);
            CFRelease(previous);
        }
    }
    if (atomic_load(&inputSettingsSuspended)) {
        memset(slotPhysical, 0, sizeof slotPhysical);
        memset(slotModeState, 0, sizeof slotModeState);
        for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
            nextSlots[s].lx = nextSlots[s].ly = nextSlots[s].rx = nextSlots[s].ry = 0;
    }
    uint64_t now = runtime_now_ns();
    int schedulePulse = 0;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; ++s) {
        if (nextOwner[s] != slotOwner[s]) { slot_clear(s); slotOwner[s] = nextOwner[s]; assignmentChanged = YES; }
        int pulse = 0;
        nextSlots[s].buttons = controller_state_apply(&slotModeState[s], slotMap[s], slotModes[s],
            slotPhysical[s], nextSlots[s].lx || nextSlots[s].ly, nextSlots[s].connected, now, &pulse);
        stick_bindings_apply(s, &nextSlots[s]);
        schedulePulse |= pulse;
    }
    if (schedulePulse)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(400 * NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), ^{ refresh_gamepads(); });
    next.buttons = nextSlots[0].buttons;
    pthread_mutex_lock(&gamepadLock);
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    gamepadState = next;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        pressedEdgesSlot[s] |= nextSlots[s].buttons & ~gamepadStates[s].buttons;
        gamepadStates[s] = nextSlots[s];
    }
    gamepadConnectedCount = count;
    pthread_mutex_unlock(&gamepadLock);
    static unsigned lastCount;
    if (count != lastCount || assignmentChanged) { lastCount = count; runtime_input_gamepads_changed(); }
}

static void attach_gamepad(GCController *controller) {
    controller.handlerQueue = dispatch_get_main_queue();
#if !TARGET_OS_OSX
    fprintf(stderr, "[input-motion] %s (%s): motion=%d rotation=%d gravity=%d manual=%d active=%d\n",
            controller.vendorName.UTF8String ?: "unknown", controller.productCategory.UTF8String ?: "unknown",
            controller.motion != nil, controller.motion.hasRotationRate, controller.motion.hasGravityAndUserAcceleration,
            controller.motion.sensorsRequireManualActivation, controller.motion.sensorsActive);
    ios_controller_probe(controller);
#endif
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad) return;
    pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
        (void)changed; (void)element;
        refresh_gamepads();
    };
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s), face buttons by %s\n",
            (controller.vendorName ?: @"unknown").UTF8String,
            (controller.productCategory ?: @"unknown").UTF8String, face_by_position(controller) ? "position" : "label");
    refresh_gamepads();
}

/* Key 6 (runtime/input.c): the next type (auto -> Xbox -> PlayStation -> Switch); the held buttons are read again at once. */
int runtime_gamepad_type_set(int type) {
    if (type < GAMEPAD_TYPE_AUTO || type >= GAMEPAD_TYPE_COUNT) type = GAMEPAD_TYPE_AUTO;
    slot_load();
#if TARGET_OS_OSX
    /* P1 stores the explicit global raw-HID compatibility switch. */
    int saved = (type == GAMEPAD_TYPE_THIRDPARTY_PRO || slotTypes[0] == GAMEPAD_TYPE_THIRDPARTY_PRO)
        ? slot_save(0, slotMap[0], slotModes[0], type) : runtime_gamepad_slot_type_set(0, type);
#else
    int saved = runtime_gamepad_slot_type_set(0, type);
#endif
    if (!saved) return slotTypes[0];
    fprintf(stderr, "[totk-input] controller type: %s\n", gamepad_type_name(type));
    dispatch_async(dispatch_get_main_queue(), ^{
        for (GCController *controller in GCController.controllers)
            if (controller.extendedGamepad)
                fprintf(stderr, "[totk-input] %s: face buttons by %s\n", (controller.vendorName ?: @"unknown").UTF8String,
                        face_by_position(controller) ? "position" : "label");
        pro_hid_set(type == GAMEPAD_TYPE_THIRDPARTY_PRO);
        refresh_gamepads();
    });
    return type;
}

int runtime_gamepad_type_cycle(void) {
    return runtime_gamepad_type_set(gamepad_type_next(atomic_load(&controllerType)));
}

/* For the HUD (main thread): the picked type; *detected = what auto takes the first connected controller for, -1 none. */
int runtime_gamepad_type(int *detected) {
    int found = -1;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        GCController *controller = gamepadSlots[s];
        if (controller && controller.extendedGamepad) {
            found = gamepad_type_detect(controller.productCategory.UTF8String, controller.vendorName.UTF8String);
            break;
        }
    }
    if (found < 0 && hidPad[0].present) found = GAMEPAD_TYPE_SWITCH;   /* the generic HID pads are Switch-style */
    if (detected) *detected = found;
    return atomic_load(&controllerType);
}

void runtime_gamepad_start(void) {
    if (started) return;
    started = YES;
    /* macOS 11.3 stopped delivering controller events to an app that is not frontmost unless asked. The title screen
       ignores a press until the window is clicked without this. */
    if (@available(macOS 11.3, *)) GCController.shouldMonitorBackgroundEvents = YES;
    load_controller_map();
    load_controller_type();
    slot_load();
    input_source_load();
    stick_bindings_load();
    atomic_store(&controllerType, slotTypes[0]);
    load_mouse_mode();
    kb_start();
    /* Old keyboard bindings predate source selection. Honor them exclusively
       only when no valid source configuration exists; explicit choices win. */
    if (!inputSourcesConfigured && runtime_keyboard_slot() >= 0)
        atomic_store(&inputSource[runtime_keyboard_slot()], INPUT_SOURCE_KEYBOARD);
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
    if (atomic_load(&controllerType) == GAMEPAD_TYPE_THIRDPARTY_PRO) pro_hid_set(1);
    dispatch_source_t pollTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
#if TARGET_OS_OSX
    dispatch_source_set_timer(pollTimer, dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), 1500 * NSEC_PER_MSEC, 200 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(pollTimer, ^{
        if (atomic_load(&controllerType) == GAMEPAD_TYPE_THIRDPARTY_PRO) pro_hid_set(1);
        refresh_gamepads();
    });
#else
    dispatch_source_set_timer(pollTimer, dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC), 16 * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(pollTimer, ^{
        if (gamepadSlots[0] || GCController.controllers.count > 0) refresh_gamepads();
    });
#endif
    dispatch_resume(pollTimer);
    fprintf(stderr, "[totk-input] controller type at start: %s\n", gamepad_type_name(atomic_load(&controllerType)));
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
    runtime_keyboard_clear();
    memset(slotModeState, 0, sizeof slotModeState);
    pthread_mutex_lock(&gamepadLock);
    gamepadState = (RuntimeGamepadState){0};
    touch_clear_locked();
    pressedEdges = 0;
    for (int s = 0; s < RUNTIME_MAX_GAMEPADS; s++) {
        gamepadStates[s].buttons = 0;
        gamepadStates[s].lx = gamepadStates[s].ly = gamepadStates[s].rx = gamepadStates[s].ry = 0;
        pressedEdgesSlot[s] = 0;
    }
    mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0;
    pthread_mutex_unlock(&gamepadLock);
}

/* On-screen touch controls (iPhone, ios/app/Sources/touch_controls.m) feed a second state merged with the physical
   controllers: buttons are OR-ed (a tap shorter than one game frame is latched like a controller press), each stick
   axis takes the stronger value. */
static RuntimeGamepadState touchState;
static uint64_t touchEdges;
static int touchAvailable; /* UI presence is not a held button; clearing input must preserve it. */
static void touch_clear_locked(void) { touchState = (RuntimeGamepadState){0}; touchEdges = 0; }
void runtime_touch_pad_set(const RuntimeGamepadState *s) {
    pthread_mutex_lock(&gamepadLock);
    touchAvailable = !!s->connected;
    if (runtime_input_source_allows(0,INPUT_DEVICE_TOUCH) && !atomic_load(&inputSettingsSuspended)) {
        touchEdges |= s->buttons & ~touchState.buttons; touchState = *s;
    } else { touchEdges = 0; touchState = (RuntimeGamepadState){0}; }
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_gamepad_read_index(unsigned index, RuntimeGamepadState *out) {
    if (index >= RUNTIME_MAX_GAMEPADS) { *out = (RuntimeGamepadState){0}; return; }
    pthread_mutex_lock(&gamepadLock);
    *out = gamepadStates[index];
    out->buttons |= pressedEdgesSlot[index];
    pressedEdgesSlot[index] = 0;
    if (index == 0) {
        out->buttons |= pressedEdges;
        if (runtime_input_source_allows(0,INPUT_DEVICE_TOUCH)) {
            out->buttons |= touchState.buttons | touchEdges;
            out->lx = stronger_axis(out->lx, touchState.lx); out->ly = stronger_axis(out->ly, touchState.ly);
            out->rx = stronger_axis(out->rx, touchState.rx); out->ry = stronger_axis(out->ry, touchState.ry);
            if (touchAvailable) out->connected = 1;
        }
        touchEdges = 0;
        if (runtime_input_source_allows(0,INPUT_DEVICE_MOUSE)) {
            int target=atomic_load(&mouseStickTarget);
            if(target&1){out->lx=stronger_axis(out->lx,mouseRX);out->ly=stronger_axis(out->ly,mouseRY);}
            if(target&2){out->rx=stronger_axis(out->rx,mouseRX);out->ry=stronger_axis(out->ry,mouseRY);}
        }
        pressedEdges = 0;
    }
    pthread_mutex_unlock(&gamepadLock);
    if (runtime_input_source_allows((int)index,INPUT_DEVICE_KEYBOARD)) kb_merge((int)index, out);
    if (atomic_load(&inputSettingsSuspended)) *out = (RuntimeGamepadState){.connected=out->connected};
}

int runtime_gamepad_connected(unsigned index) {
    if (index >= RUNTIME_MAX_GAMEPADS) return 0;
    pthread_mutex_lock(&gamepadLock);
    int connected = (runtime_input_source_allows((int)index,INPUT_DEVICE_PAD) && gamepadStates[index].connected) ||
        (!index && runtime_input_source_allows(0,INPUT_DEVICE_TOUCH) && touchAvailable);
    pthread_mutex_unlock(&gamepadLock);
    pthread_mutex_lock(&kbLock);
    if (runtime_input_source_allows((int)index,INPUT_DEVICE_KEYBOARD)) {
#if TARGET_OS_OSX
    if (physicalKeyboardEnabled) connected |= physicalKeyboardConnected[index];
    else
#endif
    connected |= kbConnected && kbSlot == (int)index;
#if TARGET_OS_OSX
    if (!index && !physicalKeyboardEnabled && kbSlot < 0) connected = 1; /* legacy system keyboard */
#endif
    }
    pthread_mutex_unlock(&kbLock);
    return connected;
}

unsigned runtime_gamepad_count(void) {
    unsigned count = 0;
    for (unsigned s = 0; s < RUNTIME_MAX_GAMEPADS; ++s)
        count += runtime_gamepad_connected(s) != 0;
    return count;
}

void runtime_gamepad_read(RuntimeGamepadState *out) {
    runtime_gamepad_read_index(0, out);
}

/* Settings suppress guest input, not connection topology. Main queue only. */
void runtime_input_settings_suspend(int suspended) {
    atomic_store(&inputSettingsSuspended, !!suspended);
    runtime_keyboard_suspend(suspended);
    runtime_mouse_release();
    runtime_input_clear();
    pthread_mutex_lock(&gamepadLock);
    touchState = (RuntimeGamepadState){0};
    pthread_mutex_unlock(&gamepadLock);
}

int runtime_input_settings_active(void) { return atomic_load(&inputSettingsSuspended); }
