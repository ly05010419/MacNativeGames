/* Native macOS controllers feed the same FullKey state as the keyboard. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"

extern void runtime_title_gamepad_event(RuntimeGamepadState) __attribute__((weak_import));
extern uint64_t runtime_title_physical_buttons(uint64_t,const char *,const char *) __attribute__((weak_import));
static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState gamepadState;
static uint64_t pressedEdges;
static BOOL started;

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

static uint64_t gamepad_buttons(GCExtendedGamepad *pad) {
    uint64_t buttons = 0;
    if (pad.buttonA.isPressed) buttons |= 1ULL << 0;
    if (pad.buttonB.isPressed) buttons |= 1ULL << 1;
    if (pad.buttonX.isPressed) buttons |= 1ULL << 2;
    if (pad.buttonY.isPressed) buttons |= 1ULL << 3;
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
    return buttons;
}

/* The older, non-extended profile: face buttons, shoulders, triggers and the d-pad (no thumbsticks). Most macOS
   controllers expose GCExtendedGamepad, but a few cheap / legacy pads only expose GCGamepad; map what exists. */
static uint64_t gamepad_buttons_legacy(GCGamepad *pad) {
    uint64_t buttons = 0;
    if (pad.buttonA.isPressed) buttons |= 1ULL << 0;
    if (pad.buttonB.isPressed) buttons |= 1ULL << 1;
    if (pad.buttonX.isPressed) buttons |= 1ULL << 2;
    if (pad.buttonY.isPressed) buttons |= 1ULL << 3;
    if (pad.leftShoulder.isPressed) buttons |= 1ULL << 6;
    if (pad.rightShoulder.isPressed) buttons |= 1ULL << 7;
    if (pad.dpad.left.isPressed) buttons |= 1ULL << 12;
    if (pad.dpad.up.isPressed) buttons |= 1ULL << 13;
    if (pad.dpad.right.isPressed) buttons |= 1ULL << 14;
    if (pad.dpad.down.isPressed) buttons |= 1ULL << 15;
    return buttons;
}

/* TOTK_GAMEPAD_SWAP_ABXY=1: swap the face buttons (A<->B, X<->Y) for users who prefer the opposite layout. */
static int gamepad_swap_abxy_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_GAMEPAD_SWAP_ABXY"); on = e && atoi(e); } return on; }
static uint64_t gamepad_swap_abxy(uint64_t b) {
    if (!gamepad_swap_abxy_on()) return b;
    uint64_t r = b & ~0xFULL;
    if (b & 1) r |= 2; if (b & 2) r |= 1; if (b & 4) r |= 8; if (b & 8) r |= 4;
    return r;
}

static int gamepad_log_on(void) { static int on = -1; if (on < 0) on = getenv("TOTK_GAMEPAD_LOG") != NULL; return on; }

static void refresh_gamepads(void) {
    RuntimeGamepadState next = {0};
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (pad) {
            next.connected = 1;
            uint64_t buttons=gamepad_buttons(pad);
            next.buttons |= runtime_title_physical_buttons?runtime_title_physical_buttons(buttons,controller.productCategory.UTF8String,controller.vendorName.UTF8String):buttons;
            next.lx = stronger_axis(next.lx, stick_axis(pad.leftThumbstick.xAxis.value));
            next.ly = stronger_axis(next.ly, stick_axis(pad.leftThumbstick.yAxis.value));
            next.rx = stronger_axis(next.rx, stick_axis(pad.rightThumbstick.xAxis.value));
            next.ry = stronger_axis(next.ry, stick_axis(pad.rightThumbstick.yAxis.value));
        } else if (controller.gamepad) {
            next.connected = 1;
            uint64_t buttons=gamepad_buttons_legacy(controller.gamepad);
            next.buttons |= runtime_title_physical_buttons?runtime_title_physical_buttons(buttons,controller.productCategory.UTF8String,controller.vendorName.UTF8String):buttons;
        }
    }
    next.buttons = gamepad_swap_abxy(next.buttons);
    pthread_mutex_lock(&gamepadLock);
    uint64_t prev = gamepadState.buttons;
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    /* Sticks change continuously; log them only after a sizeable move (and the return to centre), so a pad test
       shows the actual axis values without flooding the log. */
    static RuntimeGamepadState logged;
    int stick_moved = abs(next.lx - logged.lx) > 8000 || abs(next.ly - logged.ly) > 8000 ||
                      abs(next.rx - logged.rx) > 8000 || abs(next.ry - logged.ry) > 8000;
    if (gamepad_log_on() && (next.buttons != prev || next.connected != gamepadState.connected || stick_moved)) {
        fprintf(stderr, "[totk-input] gamepad state: connected=%d buttons=%#llx lx=%d ly=%d rx=%d ry=%d\n",
                next.connected, (unsigned long long)next.buttons, next.lx, next.ly, next.rx, next.ry);
        logged = next;
    }
    gamepadState = next;
    pthread_mutex_unlock(&gamepadLock);
    if(runtime_title_gamepad_event)runtime_title_gamepad_event(next);
}

static void attach_gamepad(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    GCGamepad *legacy = pad ? nil : controller.gamepad;
    if (!pad && !legacy) {
        fprintf(stderr, "[totk-input] native controller %s (%s) has no gamepad profile (extended=%d gamepad=%d)\n",
                (controller.vendorName ?: @"unknown").UTF8String,
                (controller.productCategory ?: @"unknown").UTF8String,
                controller.extendedGamepad != nil, controller.gamepad != nil);
        return;
    }
    if (pad) {
        pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
            (void)changed; (void)element;
            refresh_gamepads();
        };
    } else {
        legacy.valueChangedHandler = ^(GCGamepad *changed, GCControllerElement *element) {
            (void)changed; (void)element;
            refresh_gamepads();
        };
    }
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s), %s profile\n",
            (controller.vendorName ?: @"unknown").UTF8String,
            (controller.productCategory ?: @"unknown").UTF8String,
            pad ? "extended" : "legacy");
    runtime_feedback_attach(controller);
    refresh_gamepads();
}

void runtime_gamepad_start(void) {
    if (started) return;
    started = YES;
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
    fprintf(stderr, "[totk-input] native gamepads at start: %lu\n", (unsigned long)GCController.controllers.count);
    for (GCController *controller in GCController.controllers) attach_gamepad(controller);
    /* 无线发现要等主 run loop 起来：runtime_gamepad_start 在 [NSApp run] 之前调用，直接扫可能永远扫不到。
       Wireless discovery needs the main run loop, which starts after this setup ([NSApp run]); starting the scan
       before that can make it never run. */
    dispatch_async(dispatch_get_main_queue(), ^{
        fprintf(stderr, "[totk-input] controller discovery started\n");
        [GCController startWirelessControllerDiscoveryWithCompletionHandler:^{
            NSUInteger n = GCController.controllers.count;
            fprintf(stderr, "[totk-input] controller discovery finished: %lu native gamepad(s); keyboard still works\n", (unsigned long)n);
        }];
    });
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
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
    pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}
