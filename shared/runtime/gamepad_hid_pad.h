/* Generic HID pads that macOS does not hand to GameController, read straight from IOKit (shared/README.md). Included by
   runtime/gamepad_core.h, which supplies stick_axis, refresh_gamepads and the game's gamepad_game_stick. iOS: none. */
#ifndef GAMEPAD_HID_PAD_H
#define GAMEPAD_HID_PAD_H

#if TARGET_OS_OSX
/* ---- Generic HID pads macOS will not hand to GameController ----

   GameController only adopts a fixed set of controller families, so a generic USB pad (a Switch-style clone, a DirectInput
   pad from a budget brand) is invisible to it: GCController.controllers stays empty and none of the code below ever runs.
   The HID stack still sees such a pad, and the kernel marks it, per device, with GameControllerSupportedHIDDevice = No.

   This backend opens exactly those devices - present and false, never absent - so a controller GameController already
   handles is not read twice, and feeds the same physical button bits and stick values as the GameController path. From
   there the player's layout file (controller_map.h) applies unchanged, so face buttons, modes and remapping work the same.

   Usages come from the pad's own descriptor: button usages 1-15 (in the order gamepad_hid_order picks from the pad's
   name), the hat switch, and the X/Y axes with Z/Rz for the right stick, or Rx/Ry when the pad has those and no Z/Rz (a
   Switch Pro Controller's simple report). A pad whose buttons sit elsewhere simply leaves those bits unused. */
typedef struct { int present; uint64_t buttons; int32_t lx, ly, rx, ry; } HIDPadState;
static HIDPadState hidPad;
static IOHIDManagerRef hidManager;
static IOHIDDeviceRef hidDevice;
static BOOL hidRefreshPending;
static unsigned char hidButtons[16];          /* usage 1..15, 1 = pressed */
static int hidHat = 8;                        /* 0 N, 1 NE, 2 E ... 7 NW; 8 centred */
static float hidX, hidY, hidZ, hidRz;         /* -1..1, 0 centred */
static int hidOrder;                          /* gamepad_hid_order of the attached pad */
#ifdef RUNTIME_STICK_CALIBRATION
static unsigned hidSampleAxes; /* all four descriptor axes must be primed; attach zeros are not samples */
#endif
static int hidRightRxRy;                      /* its right stick is Rx/Ry */
static GamepadHidReach hidReach[4];           /* X, Y, right X, right Y; used in the Switch Pro order */
static float hidReachLogged[4][2];

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

/* The furthest each stick direction has reached, in the log whenever one grew by 0.05 (Switch Pro order only). */
static void hid_reach_note(void) {
    int grown = 0;
    for (int a = 0; a < 4; a++)
        if (hidReach[a].neg >= hidReachLogged[a][0] + 0.05f || hidReach[a].pos >= hidReachLogged[a][1] + 0.05f) grown = 1;
    if (!grown) return;
    for (int a = 0; a < 4; a++) { hidReachLogged[a][0] = hidReach[a].neg; hidReachLogged[a][1] = hidReach[a].pos; }
    fprintf(stderr, "[totk-input] HID stick reach: left -%.2f/+%.2f, -%.2f/+%.2f; right -%.2f/+%.2f, -%.2f/+%.2f (1 = the HID range's end)\n",
            hidReach[0].neg, hidReach[0].pos, hidReach[1].neg, hidReach[1].pos, hidReach[2].neg, hidReach[2].pos, hidReach[3].neg, hidReach[3].pos);
}

/* The same physical bits gamepad_buttons() produces, so the layout file cannot tell the two sources apart. */
static void hid_pad_compute(void) {
    /* The face buttons are Switch-style positions (B bottom, A right, Y left, X top), so they go in as GameController's
       buttonA (bottom), buttonB (right), buttonX (left), buttonY (top) and are always taken by position, whatever the
       maker's name says; which usage is which comes from gamepad_hid_order. */
    uint64_t buttons = gamepad_hid_buttons(hidButtons, hidOrder);
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
    float lx = hidX, ly = hidY, rx = hidZ, ry = hidRz;
#ifdef RUNTIME_STICK_CALIBRATION
    /* Calibration UI always measures descriptor coordinates, never the learning reach transform. */
    hidRawLx=gamepad_game_stick(0,lx); hidRawLy=gamepad_game_stick(1,-ly);
    hidRawRx=gamepad_game_stick(2,rx); hidRawRy=gamepad_game_stick(3,-ry);
#endif
    if (hidOrder == GAMEPAD_HID_SWITCH_PRO
#ifdef RUNTIME_STICK_CALIBRATION
        && !(calibration_hid_matches() && calibration.stableHID)
#endif
    ) {   /* sticks that stop short of the HID range (gamepad_hid_reach) */
        lx = gamepad_hid_reach(&hidReach[0], lx); ly = gamepad_hid_reach(&hidReach[1], ly);
        rx = gamepad_hid_reach(&hidReach[2], rx); ry = gamepad_hid_reach(&hidReach[3], ry);
        hid_reach_note();
    }
    hidPad.present = 1;
    hidPad.buttons = buttons;
    float cx = gamepad_game_stick(0, lx), cy = gamepad_game_stick(1, -ly);
    float crx = gamepad_game_stick(2, rx), cry = gamepad_game_stick(3, -ry);
    hidPad.lx = native_stick_component(0, cx, cy, calibration_hid_matches());
    hidPad.ly = native_stick_component(1, cx, cy, calibration_hid_matches());
    hidPad.rx = native_stick_component(2, crx, cry, calibration_hid_matches());
    hidPad.ry = native_stick_component(3, crx, cry, calibration_hid_matches());
#ifdef RUNTIME_STICK_CALIBRATION
    hidCaptureActive = source_capture_active(0,cx,cy,calibration_hid_matches()) || source_capture_active(1,crx,cry,calibration_hid_matches());
#endif
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
#ifdef RUNTIME_STICK_CALIBRATION
        hidSampleAxes |= 1;
#endif
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Y) {
        hidY = hid_axis(element, v);
#ifdef RUNTIME_STICK_CALIBRATION
        hidSampleAxes |= 2;
#endif
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Z) {
        hidZ = hid_axis(element, v);
#ifdef RUNTIME_STICK_CALIBRATION
        hidSampleAxes |= 4;
#endif
    } else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Rz) {
        hidRz = hid_axis(element, v);
#ifdef RUNTIME_STICK_CALIBRATION
        hidSampleAxes |= 8;
#endif
    } else if (hidRightRxRy && page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Rx) {
        hidZ = hid_axis(element, v);
#ifdef RUNTIME_STICK_CALIBRATION
        hidSampleAxes |= 4;
#endif
    } else if (hidRightRxRy && page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Ry) {
        hidRz = hid_axis(element, v);
#ifdef RUNTIME_STICK_CALIBRATION
        hidSampleAxes |= 8;
#endif
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

#ifdef RUNTIME_STICK_CALIBRATION
/* Change-only HID devices need not emit idle reports: prime genuine current axis values once
   on attach. Missing/unreadable axes remain invalid until their first actual callback. */
static void hid_prime_axes(IOHIDDeviceRef device) {
    CFArrayRef elements=IOHIDDeviceCopyMatchingElements(device,NULL,kIOHIDOptionsTypeNone);
    if (!elements) return;
    for (CFIndex i=0;i<CFArrayGetCount(elements);i++) {
        IOHIDElementRef element=(IOHIDElementRef)CFArrayGetValueAtIndex(elements,i);
        uint32_t page=IOHIDElementGetUsagePage(element), usage=IOHIDElementGetUsage(element);
        if (page!=kHIDPage_GenericDesktop || !(usage==kHIDUsage_GD_X || usage==kHIDUsage_GD_Y ||
            usage==kHIDUsage_GD_Z || usage==kHIDUsage_GD_Rz || (hidRightRxRy && (usage==kHIDUsage_GD_Rx || usage==kHIDUsage_GD_Ry)))) continue;
        IOHIDValueRef value=NULL;
        if (IOHIDDeviceGetValue(device,element,&value)==kIOReturnSuccess && value) hid_value(NULL,kIOReturnSuccess,device,value);
    }
    CFRelease(elements);
}
#endif

static void hid_pad_reset(void) {
    memset(hidButtons, 0, sizeof hidButtons);
    hidHat = 8;
    hidX = hidY = hidZ = hidRz = 0;
    memset(hidReach, 0, sizeof hidReach);
    memset(hidReachLogged, 0, sizeof hidReachLogged);
    hidPad = (HIDPadState){0};
#ifdef RUNTIME_STICK_CALIBRATION
    hidRawLx = hidRawLy = hidRawRx = hidRawRy = 0;
    hidCaptureActive = 0;
    hidSampleAxes=0;hidConnectionGeneration++;calibration_sample_invalidate();
#endif
}

/* 1: the pad's right stick is Rx/Ry (it has both and neither Z nor Rz). */
static int hid_right_rx_ry(IOHIDDeviceRef device) {
    CFArrayRef elements = IOHIDDeviceCopyMatchingElements(device, NULL, kIOHIDOptionsTypeNone);
    if (!elements) return 0;
    int rx = 0, ry = 0, z = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(elements); i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(elements, i);
        if (IOHIDElementGetUsagePage(e) != kHIDPage_GenericDesktop) continue;
        uint32_t u = IOHIDElementGetUsage(e);
        rx |= u == kHIDUsage_GD_Rx; ry |= u == kHIDUsage_GD_Ry; z |= u == kHIDUsage_GD_Z || u == kHIDUsage_GD_Rz;
    }
    CFRelease(elements);
    return rx && ry && !z;
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

/* Why a pad was not taken: HID_LEFT for good (GameController's, or another pad is in use), HID_NOT_READY may change
   (2026-10-28: a PowerA that reconnected while Super Smash Bros. Ultimate ran was never taken again and the log said nothing, because every early
   return here was silent; the GameController flag read No at once, so the reason is named now instead of guessed). */
enum { HID_TAKEN, HID_LEFT, HID_NOT_READY };
static int hid_pad_attach(IOHIDDeviceRef device, char *why, size_t n) {
    if (hidDevice) { snprintf(why, n, "another HID gamepad is in use"); return HID_LEFT; }
    /* Absent means GameController may well own it; only a definite No says the pad is unreachable otherwise. The flag also
       reads No on pads GameController does adopt (a Bluetooth Switch Pro Controller), so Nintendo, Sony and Microsoft
       devices are left to it. */
    CFTypeRef vendorId = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    int vid = 0;
    if (vendorId && CFGetTypeID(vendorId) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)vendorId, kCFNumberIntType, &vid);
    if (vid == 0x057e || vid == 0x054c || vid == 0x045e) { snprintf(why, n, "vendor 0x%04x is left to GameController", vid); return HID_LEFT; }
    CFTypeRef supported = IOHIDDeviceGetProperty(device, CFSTR("GameControllerSupportedHIDDevice"));
    if (!supported) { snprintf(why, n, "GameControllerSupportedHIDDevice not set"); return HID_NOT_READY; }
    if (CFGetTypeID(supported) != CFBooleanGetTypeID() || CFBooleanGetValue((CFBooleanRef)supported)) {
        snprintf(why, n, "GameController takes it"); return HID_LEFT;
    }
    IOReturn opened = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) { snprintf(why, n, "IOHIDDeviceOpen 0x%08x", opened); return HID_NOT_READY; }

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
    hidOrder = gamepad_hid_order(name);
    hidRightRxRy = hid_right_rx_ry(device);
    IOHIDDeviceRegisterInputValueCallback(device, hid_value, NULL);
    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    hidPad.present = 1;
#ifdef RUNTIME_STICK_CALIBRATION
    hid_prime_axes(device);
#endif
    fprintf(stderr, "[totk-input] HID gamepad connected: %s (%s)\n", name ? name : "unknown", manufacturer ? manufacturer : "unknown");
    fprintf(stderr, "[totk-input] HID gamepad buttons in the %s order, right stick %s", hidOrder == GAMEPAD_HID_SWITCH_PRO ?
            "Switch Pro Controller" : "generic", hidRightRxRy ? "Rx/Ry" : "Z/Rz");
    if (hidOrder == GAMEPAD_HID_SWITCH_PRO) fprintf(stderr, ", sticks scaled to their reach, full at %.0f%%", GAMEPAD_HID_FULL_AT * 100);
    fprintf(stderr, "\n");
    refresh_gamepads();
    return HID_TAKEN;
}

/* Offers a pad that appeared: one not ready yet is tried again after 0.25, 0.5, 1, 2 and 4 s, then given up; the log
   names the reason at the first try and at the last. */
enum { HID_TRIES = 6 };
static void hid_pad_offer(IOHIDDeviceRef device, int attempt) {
    char why[128] = "";
    int taken = hid_pad_attach(device, why, sizeof why);
    if (taken == HID_TAKEN) { if (attempt) fprintf(stderr, "[totk-input] HID gamepad taken at try %d\n", attempt + 1); return; }
    CFTypeRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    const char *name = product && CFGetTypeID(product) == CFStringGetTypeID() ? [(__bridge NSString *)product UTF8String] : "unknown";
    if (taken == HID_LEFT) { fprintf(stderr, "[totk-input] HID gamepad not taken: %s: %s\n", name, why); return; }
    if (attempt + 1 >= HID_TRIES) { fprintf(stderr, "[totk-input] HID gamepad not taken: %s: %s (gave up after %d tries)\n", name, why, HID_TRIES); return; }
    if (!attempt) fprintf(stderr, "[totk-input] HID gamepad not ready: %s: %s; trying again\n", name, why);
    CFRetain(device);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.25 * (1 << attempt) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        hid_pad_offer(device, attempt + 1);
        CFRelease(device);
    });
}

static void hid_matched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)context; (void)sender;
    if (result != kIOReturnSuccess) fprintf(stderr, "[totk-input] HID gamepad matched with result 0x%08x\n", result);
    hid_pad_offer(device, 0);
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

#endif
