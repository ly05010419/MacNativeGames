// Switch 2 Pro Controller over USB (057e:2069), for a Mac where GameController never lists it: macOS enumerates its HID
// interface as a plain game pad (usage page 1, usage 5) and marks GameControllerSupportedHIDDevice No, so
// GCController.controllers() stays empty (checked on this pad over USB, 2026-11-10) and runtime/gamepad.m's GameController
// path can never see it. Reading its own report 0x09 here is then the only path.
//
// Ported from the archived upstream patch upstream-patches/0002-switch2-usb-hid.patch (dm/host/pad_switch2_usb_apple.m, 196
// lines; the same code in nxnative), with one deliberate difference: that file calls dm_pad_set_player (nxnative's npad
// layer) and this port's sink is runtime/gamepad.m, which merges the decoded state with the GameController pads into the
// port's one RuntimeGamepadState -- exactly how games/totk/runtime/gamepad.m (hid_switch2_input) does it in this same
// lineage. Everything else is kept as it is there: the same two interfaces, the same four init frames and order, the same
// report 9 decoding (byte for byte), the same own thread with its own run loop.
//
// Two interfaces: 0 is HID and carries the input reports, 1 is vendor class and holds the command channel. The pad pushes
// nothing at all until the four init frames below are written to interface 1's bulk-out pipe 0x02; writing HID Output Report
// 2 instead reports success and does nothing (that report is the rumble channel). The initialisation is sticky: once done the
// pad reports until it is unplugged or the Mac restarts, so a re-attach sends it again and may find it already effate.
//
// The report is decoded from its bytes, not from HID elements: macOS creates no Z element for this pad, so its right stick is
// unreachable through elements, and the buttons come in the pad's own order.
//
// On by default (this port has no other Nintendo path); TOTK_SW2_USB=0 turns it off, and only then is no HID device opened.
#import <Foundation/Foundation.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDKeys.h>
#import <IOUSBHost/IOUSBHost.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "gamepad_layout.h"   /* gamepad_hid_reach: the proven reach self-calibration (games/totk) */

#define SW2_USB_VID 0x057E
#define SW2_USB_PID 0x2069
#define SW2_LOG(...) do { fprintf(stderr, "[totk-input] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* The decoded state, written by the report callback (its own thread) and read by refresh_gamepads (main queue). */
static pthread_mutex_t sw2Lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t sw2Buttons;                     /* physical bitmap, bit i = runtime/controller_map.h controller_physical_names[i] */
static float sw2Lx, sw2Ly, sw2Rx, sw2Ry;        /* -1..1, y up (the report's y grows downwards) */
static int sw2Connected;
static IOHIDManagerRef sw2Manager;
static IOHIDDeviceRef sw2Device;
static uint8_t sw2Report[512];                  /* the buffer the input callback is registered with: it must outlive the device */
static int sw2Woken;
static unsigned sw2Reports;

int runtime_sw2_state(uint64_t *buttons, float *lx, float *ly, float *rx, float *ry) {
    pthread_mutex_lock(&sw2Lock);
    int connected = sw2Connected;
    if (buttons) *buttons = sw2Buttons;
    if (lx) *lx = sw2Lx;
    if (ly) *ly = sw2Ly;
    if (rx) *rx = sw2Rx;
    if (ry) *ry = sw2Ry;
    pthread_mutex_unlock(&sw2Lock);
    return connected;
}

// ---- Interface 1: the vendor command channel, which unlocks the pad's reports ----
static void sw2_usb_frame(IOUSBHostPipe *pipe, const unsigned char *frame, NSUInteger length) {
    NSMutableData *data = [NSMutableData dataWithBytes:frame length:length];
    NSUInteger transferred = 0;
    NSError *error = nil;
    if (![pipe sendIORequestWithData:data bytesTransferred:&transferred completionTimeout:1.0 error:&error])
        SW2_LOG("switch 2 usb: command 0x%02x/0x%02x over interface 1 was refused (%s)", frame[0], frame[3],
                error.localizedDescription.UTF8String ?: "no text");
}

/* 1 when the frames went out. The property filter finds nothing on IOUSBHostInterface (idVendor and the rest are on the
   node, not matchable here), so every interface is read and compared instead. */
static int sw2_usb_wake(void) {
    static const unsigned char init_usb[]     = { 0x03,0x91,0x00,0x0D,0x00,0x08,0x00,0x00,0x01,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    static const unsigned char feature_mask[] = { 0x0C,0x91,0x00,0x02,0x00,0x04,0x00,0x00,0x27,0x00,0x00,0x00 };
    static const unsigned char enable[]       = { 0x0C,0x91,0x00,0x04,0x00,0x04,0x00,0x00,0x27,0x00,0x00,0x00 };
    static const unsigned char select_09[]    = { 0x03,0x91,0x00,0x0A,0x00,0x04,0x00,0x00,0x09,0x00,0x00,0x00 };
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostInterface");
    if (!match) return 0;
    io_iterator_t found = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &found) != KERN_SUCCESS) return 0;
    int done = 0;
    io_service_t service;
    while ((service = IOIteratorNext(found))) {
        int v = 0, p = 0, number = -1;
        CFTypeRef vv = IORegistryEntryCreateCFProperty(service, CFSTR("idVendor"), NULL, 0);
        CFTypeRef pp = IORegistryEntryCreateCFProperty(service, CFSTR("idProduct"), NULL, 0);
        CFTypeRef nn = IORegistryEntryCreateCFProperty(service, CFSTR("bInterfaceNumber"), NULL, 0);
        if (vv && CFGetTypeID(vv) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)vv, kCFNumberIntType, &v);
        if (pp && CFGetTypeID(pp) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)pp, kCFNumberIntType, &p);
        if (nn && CFGetTypeID(nn) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)nn, kCFNumberIntType, &number);
        if (vv) CFRelease(vv);
        if (pp) CFRelease(pp);
        if (nn) CFRelease(nn);
        if (v != SW2_USB_VID || p != SW2_USB_PID || number != 1) { IOObjectRelease(service); continue; }
        NSError *error = nil;
        IOUSBHostInterface *interface = [[IOUSBHostInterface alloc] initWithIOService:service options:0 queue:nil
                                                                               error:&error interestHandler:nil];
        if (!interface) {
            SW2_LOG("switch 2 usb: vendor interface 1 is unavailable (%s)", error.localizedDescription.UTF8String ?: "no text");
            IOObjectRelease(service);
            continue;
        }
        IOUSBHostPipe *pipe = [interface copyPipeWithAddress:0x02 error:&error];
        if (pipe) {
            sw2_usb_frame(pipe, init_usb, sizeof init_usb);
            sw2_usb_frame(pipe, feature_mask, sizeof feature_mask);
            sw2_usb_frame(pipe, enable, sizeof enable);
            sw2_usb_frame(pipe, select_09, sizeof select_09);
            SW2_LOG("switch 2 usb: input reports enabled over the vendor interface (vid 0x%04x pid 0x%04x, interface 1, bulk-out 0x02)", v, p);
            done = 1;
        } else {
            SW2_LOG("switch 2 usb: interface 1 has no bulk-out pipe 0x02 (%s)", error.localizedDescription.UTF8String ?: "no text");
        }
        [interface destroy];
        IOObjectRelease(service);
        if (done) break;
    }
    IOObjectRelease(found);
    return done;
}

// ---- Interface 0: the input reports ----
// Report 9, from its bytes: [0] 0x09, [1] counter, [2] power, [3..5] the buttons, [6..8] left stick, [9..11] right stick, two
// packed 12-bit values each. Button bits in the pad's own order: B A Y X R ZR plus RS | down right left up L ZL minus LS |
// Home Capture. Home and Capture are decoded and dropped: this port's physical table (controller_map.h) has no bit for them
// and the Switch buttons the game reads do not include them -- a console button with no consumer.
static uint64_t sw2Pending; static int sw2PendingSeen;   /* the button change waiting for a second report to confirm it */
static GamepadHidReach sw2Reach[4];   /* lx, ly, rx, ry: the furthest each direction has reached since the pad connected */

static int sw2_packed12(const uint8_t *b) { return b[0] | ((b[1] & 0x0F) << 8); }
static int sw2_packed12_high(const uint8_t *b) { return (b[1] >> 4) | (b[2] << 4); }

/* One 12-bit axis to -1..1. The pad's own travel is 1200 counts either side of the centre 2048 (the default calibration of
   the Bluetooth path, switch2_proto.h s2_cal_default). No deadzone here: runtime/gamepad.m's stick_axis applies the port's
   single deadzone, so this pad is shaped exactly like a GameController one. */
static float sw2_axis(int v) {
    /* The 12-bit value spans 0..4095 about the centre 2048, so full travel is 2048 counts (games/totk/runtime/gamepad.m
       axis12 divides by 2047.5 for the same reason). Dividing by 1200, as the Bluetooth path's default calibration does,
       reached full tilt at 59% of the travel: one small push read as a held full deflection and the game's own menu
       repeat fired several steps at once ("推一下跳好几下", measured 2026-11-13). */
    float f = ((float)v - 2048.0f) / 2047.5f;
    return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

/* The pad does not rest exactly on 2048, and the offset is amplified by the reach self-calibration that follows (which
   divides by a fraction of the travel until it has seen a full push): the measured rest of the right stick (0.07..0.09)
   became 0.12..0.17 after amplification, i.e. it landed exactly on gamepad.m's 0.12 deadzone and flickered in and out of
   it -- the menu moved by itself now and then ("漂移", measured 2026-11-13). Gating the drift BEFORE the amplification
   removes it without touching the game-visible range: a real push still starts at the same physical deflection, and the
   helper below re-maps the rest of the travel to the full range. */
#define SW2_DRIFT 0.10f
static float sw2_drift_gate(float f) {
    if (f > -SW2_DRIFT && f < SW2_DRIFT) return 0.0f;
    return f > 0 ? (f - SW2_DRIFT) / (1.0f - SW2_DRIFT) : (f + SW2_DRIFT) / (1.0f - SW2_DRIFT);
}

static uint64_t sw2_physical(const uint8_t *b) {
    uint64_t m = 0;
    if (b[3] & 0x01) m |= 1ULL << 0;    /* B      -> south */
    if (b[3] & 0x02) m |= 1ULL << 1;    /* A      -> east  */
    if (b[3] & 0x08) m |= 1ULL << 2;    /* Y      -> west  */
    if (b[3] & 0x04) m |= 1ULL << 3;    /* X      -> north */
    if (b[4] & 0x80) m |= 1ULL << 4;    /* LS     -> l3 */
    if (b[3] & 0x80) m |= 1ULL << 5;    /* RS     -> r3 */
    if (b[4] & 0x10) m |= 1ULL << 6;    /* L      -> l1 */
    if (b[3] & 0x10) m |= 1ULL << 7;    /* R      -> r1 */
    if (b[4] & 0x20) m |= 1ULL << 8;    /* ZL     -> l2 */
    if (b[3] & 0x20) m |= 1ULL << 9;    /* ZR     -> r2 */
    if (b[3] & 0x40) m |= 1ULL << 10;   /* plus   -> menu */
    if (b[4] & 0x40) m |= 1ULL << 11;   /* minus  -> view */
    if (b[4] & 0x04) m |= 1ULL << 12;   /* left */
    if (b[4] & 0x08) m |= 1ULL << 13;   /* up */
    if (b[4] & 0x02) m |= 1ULL << 14;   /* right */
    if (b[4] & 0x01) m |= 1ULL << 15;   /* down */
    return m;
}

static void sw2_usb_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id,
                           uint8_t *bytes, CFIndex length) {
    (void)ctx; (void)sender; (void)type;
    if (result != kIOReturnSuccess || report_id != 9 || length < 12) return;
    /* Only the pad's full report carries the button bitmap and both sticks (64 bytes, measured on this pad every time).
       A shorter id=9 report is a different format and decoding it as state made the merged buttons fall to zero and rise
       again between full reports -- every rise is a fresh press edge for the game, which the player saw as "按一下变成连按"
       (2026-11-13). Shorter reports are logged once per length and otherwise ignored. */
    if (length < 49) {
        static int seen[64];
        if (length < 64 && !seen[length]++) SW2_LOG("switch 2 pro: id=9 report of %ld bytes is not the full format, ignored", (long)length);
        return;
    }
    uint64_t buttons = sw2_physical(bytes);
    /* y is NOT negated, unlike games/totk/runtime/gamepad.m hid_switch2_input: this pad's 12-bit y already grows upward,
       and the game reads RuntimeGamepadState.ly as y-up (runtime/input.c: ly = keys[W] - keys[S]). Measured twice on the
       real pad -- with the negation the player reported up as down ("左摇杆上下反了", 2026-11-13), and after the reach
       calibration was ported over the report stayed the same, so the sign was the whole of it. The reach self-calibration
       (gamepad_layout.h) is the part that had to come from TotK: it is what makes a half push read as half. */
    float lx = gamepad_hid_reach(&sw2Reach[0], sw2_drift_gate(sw2_axis(sw2_packed12(bytes + 6))));
    float ly = gamepad_hid_reach(&sw2Reach[1], sw2_drift_gate(sw2_axis(sw2_packed12_high(bytes + 6))));
    float rx = gamepad_hid_reach(&sw2Reach[2], sw2_drift_gate(sw2_axis(sw2_packed12(bytes + 9))));
    float ry = gamepad_hid_reach(&sw2Reach[3], sw2_drift_gate(sw2_axis(sw2_packed12_high(bytes + 9))));
    pthread_mutex_lock(&sw2Lock);
    /* One press is one press: a button change is published only when the next report repeats it (about 4 ms at 240 Hz).
       A single report that disagrees clears the bit in the merged state for a moment, and the report after it sets it
       again -- the game counts that as a second press ("推一下跳好几下 / 按一下连按", measured 2026-11-13). Sticks are not
       debounced: they are analog and the deadzone in stick_axis already ignores the rest. */
    if (buttons != sw2Buttons) {
        if (buttons != sw2Pending) { sw2Pending = buttons; sw2PendingSeen = 0; }
        else if (!sw2PendingSeen) sw2PendingSeen = 1;
        else { sw2Buttons = buttons; sw2PendingSeen = 0; }
    } else sw2Pending = buttons;
    sw2Lx = lx; sw2Ly = ly; sw2Rx = rx; sw2Ry = ry; sw2Connected = 1;
    pthread_mutex_unlock(&sw2Lock);
    /* The first three reports and then every 600th: what the decoding makes of real bytes, length included, so a stub or
       empty report could not pass for a decoded one. */
    if (sw2Reports < 3 || sw2Reports % 600 == 0)
        SW2_LOG("switch 2 pro report #%u: %ld bytes id=%u buttons=%#06llx sticks L%+.3f,%+.3f R%+.3f,%+.3f",
                sw2Reports + 1, (long)length, report_id, (unsigned long long)buttons,
                (double)lx, (double)ly, (double)rx, (double)ry);
    sw2Reports++;
    runtime_gamepad_refresh();   /* main queue: recompute the merged state (gamepad.m) */
}

static void sw2_usb_attach(IOHIDDeviceRef device) {
    sw2Device = (IOHIDDeviceRef)CFRetain(device);
    if (!sw2Woken) {
        sw2Woken = sw2_usb_wake();   // before the callback: the pad stays silent until interface 1 is initialised
        if (!sw2Woken) SW2_LOG("switch 2 usb: the pad was found but interface 1 could not be initialised: unplug and plug it back in");
    }
    IOHIDDeviceRegisterInputReportCallback(device, sw2Report, sizeof sw2Report, sw2_usb_report, NULL);
    SW2_LOG("switch 2 pro controller: reading %04x:%04x over USB HID (interface 0), reports on its own run loop",
            SW2_USB_VID, SW2_USB_PID);
}
static void sw2_usb_detach(void) {
    if (!sw2Device) return;
    IOHIDDeviceRegisterInputReportCallback(sw2Device, sw2Report, sizeof sw2Report, NULL, NULL);
    CFRelease(sw2Device);
    sw2Device = NULL;
    sw2Woken = 0;   // a pad that is plugged back in needs its interface initialised again
    sw2Reports = 0;
    pthread_mutex_lock(&sw2Lock);
    sw2Buttons = 0; sw2Lx = sw2Ly = sw2Rx = sw2Ry = 0; sw2Connected = 0;
    pthread_mutex_unlock(&sw2Lock);
    SW2_LOG("switch 2 pro controller: disconnected");
    runtime_gamepad_refresh();
}
static void sw2_usb_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || sw2Device) return;
    int vid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey)) intValue];
    int pid = [(__bridge NSNumber *)IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey)) intValue];
    if (vid != SW2_USB_VID || pid != SW2_USB_PID) return;
    sw2_usb_attach(device);
}
static void sw2_usb_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx; (void)result; (void)sender;
    if (sw2Device && CFEqual(sw2Device, device)) sw2_usb_detach();
}
static void sw2_usb_setup(void) {
    if (sw2Manager) return;
    sw2Manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!sw2Manager) { SW2_LOG("【红】switch 2 usb: cannot create the HID manager"); return; }
    // One dictionary only: a device that meets two matching dictionaries is handed over twice, and the pad declares the game pad usage.
    IOHIDManagerSetDeviceMatching(sw2Manager, (__bridge CFDictionaryRef)@{
        @kIOHIDDeviceUsagePageKey: @(kHIDPage_GenericDesktop), @kIOHIDDeviceUsageKey: @(kHIDUsage_GD_GamePad)});
    IOHIDManagerRegisterDeviceMatchingCallback(sw2Manager, sw2_usb_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(sw2Manager, sw2_usb_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(sw2Manager, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(sw2Manager, kIOHIDOptionsTypeNone);   // nonexclusive: GameController and other apps keep their devices
    if (r != kIOReturnSuccess) {
        SW2_LOG("【红】switch 2 usb: cannot open HID (0x%x)", r);
        IOHIDManagerUnscheduleFromRunLoop(sw2Manager, CFRunLoopGetCurrent(), kCFRunLoopCommonModes);
        CFRelease(sw2Manager); sw2Manager = NULL;
        return;
    }
    // A pad already plugged in when the port starts is handed to the matching callback as soon as this loop runs.
    SW2_LOG("switch 2 usb: waiting for the pad's HID interface");
}

/* Its own thread with its own run loop, as in the upstream file: the HID manager and its devices stay scheduled on this loop
   for the process's life, and reports can arrive while the main queue is busy with the game. runtime_gamepad_refresh hops
   to the main queue to merge them. */
static void *sw2_usb_thread(void *arg) {
    (void)arg;
    @autoreleasepool {
        sw2_usb_setup();
        CFRunLoopRun();
    }
    return NULL;
}

void runtime_sw2_start(void) {
    const char *e = getenv("TOTK_SW2_USB");
    if (e && !atoi(e)) { SW2_LOG("switch 2 usb: off (TOTK_SW2_USB=0)\n"); return; }
    pthread_t th;
    if (pthread_create(&th, NULL, sw2_usb_thread, NULL)) { SW2_LOG("【红】switch 2 usb: cannot start its thread"); return; }
    pthread_detach(th);
}
