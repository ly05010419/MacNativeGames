/* Native macOS controllers feed the same FullKey state as the keyboard: one player, every controller, generic HID pad,
   DSU pad, the mouse's buttons and the iPhone's touch controls together (shared/README.md). The plumbing is in
   runtime/gamepad_core.h; what one game adds is in its runtime/gamepad_game.h (Tears of the Kingdom's live button prompts,
   ability wheel and zeldaVR; Ocarina of Time's input trace; which games have mouse mode). Linked by games/totk (also Breath
   of the Wild, Donkey Kong Bananza and Pokopia), games/oot, games/acnh and games/eow; games/odyssey keeps its own, with two
   seats for Mario and Cappy. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
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
   game: textPad and textEdges are the physical buttons (controller_map.h order, the controller's own, not the mouse's) and
   the sticks. */
static RuntimeGamepadState textPad;
static uint64_t textEdges;
static uint64_t physicalPrev;
static uint64_t tapUntil[CONTROLLER_BUTTONS];   /* ns, for tap */
static int toggleOn[CONTROLLER_BUTTONS];        /* for toggle */
static int toggleMoved[CONTROLLER_BUTTONS];     /* the stick moved since the toggle went on */
static uint64_t gamepadRawLeftAt, gamepadRawRightAt;
static int32_t gamepadNativeLx, gamepadNativeLy, gamepadNativeRx, gamepadNativeRy;   /* the sticks as GameController reads them */
static RuntimeGamepadState touchState;
enum { CONTROLLER_OWN_BUTTONS = 0x1ffff };   /* physical buttons 0-16: the controller's, not the mouse's */

#include "gamepad_game.h"   /* this game's hooks and switches */
#include "gamepad_core.h"   /* the shared plumbing (shared/runtime) */

static int settings_chord_held(void) { return (textPad.buttons & settings_trigger()) == settings_trigger(); }   /* gamepadLock */

static void gamepad_mapping_reset(void) {   /* main thread */
    physicalPrev = 0;
    memset(tapUntil, 0, sizeof tapUntil); memset(toggleOn, 0, sizeof toggleOn); memset(toggleMoved, 0, sizeof toggleMoved);
}

static void refresh_gamepads(void) {
#if TARGET_OS_OSX
    /* The raw HID fallback must obey the same focus policy as GameController. Do not latch background presses. */
    if (!atomic_load(&gamepadFocused) && !runtime_gamepad_background_enabled()) {
        uint64_t trace = gamepad_game_trace_row();
        if (trace) fprintf(stderr, GAMEPAD_TRACE_TAG " %llu t=%llu refresh blocked: focused=0 background=0\n",
                           (unsigned long long)trace, (unsigned long long)runtime_now_ns());
#ifdef RUNTIME_STICK_CALIBRATION
        if (calibrationSample.valid) calibration_sample_invalidate();
#endif
        return;
    }
#endif
    RuntimeGamepadState next = {0};
    uint64_t physical = mouse_physical();
    int32_t nativeLx = 0, nativeLy = 0, nativeRx = 0, nativeRy = 0;
#ifdef RUNTIME_STICK_CALIBRATION
    float rawLx=0, rawLy=0, rawRx=0, rawRy=0, rawLT=0, rawRT=0;
    int captureActive=0,rawLeftCapture=0,rawRightCapture=0;
#endif
    uint64_t rawAt = 0, rawRightAt = 0;
    int capture = captureDown;
    for (GCController *controller in GCController.controllers) {
        GCExtendedGamepad *pad = controller.extendedGamepad;
        if (!pad) continue;
        uint64_t padPhysical = gamepad_buttons(pad, face_by_position(controller));
        float lx = pad.leftThumbstick.xAxis.value, ly = pad.leftThumbstick.yAxis.value;
        float rx = pad.rightThumbstick.xAxis.value, ry = pad.rightThumbstick.yAxis.value;
        const float axes[] = {lx, ly, rx, ry};
        gamepad_game_pad_input(controller, padPhysical, axes, atomic_load(&controllerType));
        next.connected = 1;
        physical |= padPhysical;
        if (!capture && pad.buttons[GCInputButtonShare].isPressed) capture = 1;   /* the capture button (see runtime_capture_set_hook) */
        nativeLx = stronger_axis(nativeLx, native_stick_component(0, lx, ly, calibration_source_matches(controller,0,0)));
        nativeLy = stronger_axis(nativeLy, native_stick_component(1, lx, ly, calibration_source_matches(controller,0,0)));
        int leftRaw=0,rightRaw=0;
        int wasActive = rawLeft.active;
        float rawX, rawY;
        if (controller == rawLeftController && switch_left_get(&rawLeft, lx, ly, runtime_now_ns(), &rawX, &rawY)) {
            lx = rawX; ly = rawY;leftRaw=1;
            rawAt = rawLeft.at;
            if (!wasActive) fprintf(stderr, "[totk-input] Switch left stick: native axes stuck at zero; using live HID input\n");
        }
        next.lx = stronger_axis(next.lx, native_stick_component(0, lx, ly, calibration_source_matches(controller,0,leftRaw)));
        next.ly = stronger_axis(next.ly, native_stick_component(1, lx, ly, calibration_source_matches(controller,0,leftRaw)));
        nativeRx = stronger_axis(nativeRx, native_stick_component(2, rx, ry, calibration_source_matches(controller,1,0)));
        nativeRy = stronger_axis(nativeRy, native_stick_component(3, rx, ry, calibration_source_matches(controller,1,0)));
        int rightWasActive = rawRight.active;
        float rawRX, rawRY;
        if (controller == rawRightController && switch_left_get(&rawRight, rx, ry, runtime_now_ns(), &rawRX, &rawRY)) {
            rx = rawRX; ry = rawRY;rightRaw=1;
            rawRightAt = rawRight.at;
            if (!rightWasActive) fprintf(stderr, "[totk-input] Switch right stick: native axes stuck at zero; using live HID input\n");
        }
        next.rx = stronger_axis(next.rx, native_stick_component(2, rx, ry, calibration_source_matches(controller,1,rightRaw)));
        next.ry = stronger_axis(next.ry, native_stick_component(3, rx, ry, calibration_source_matches(controller,1,rightRaw)));
#ifdef RUNTIME_STICK_CALIBRATION
        /* Non-fallback/native contributions stay live when either raw report expires. */
        captureActive |= source_capture_active(0,pad.leftThumbstick.xAxis.value,pad.leftThumbstick.yAxis.value,calibration_source_matches(controller,0,0));
        captureActive |= source_capture_active(1,pad.rightThumbstick.xAxis.value,pad.rightThumbstick.yAxis.value,calibration_source_matches(controller,1,0));
        if (leftRaw) rawLeftCapture |= source_capture_active(0,lx,ly,calibration_source_matches(controller,0,1));
        if (rightRaw) rawRightCapture |= source_capture_active(1,rx,ry,calibration_source_matches(controller,1,1));
        if (fabsf(lx) > fabsf(rawLx)) rawLx = lx;
        if (fabsf(ly) > fabsf(rawLy)) rawLy = ly;
        if (fabsf(rx) > fabsf(rawRx)) rawRx = rx;
        if (fabsf(ry) > fabsf(rawRy)) rawRy = ry;
        if (pad.leftTrigger.value > rawLT) rawLT = pad.leftTrigger.value;
        if (pad.rightTrigger.value > rawRT) rawRT = pad.rightTrigger.value;
#endif
    }
    if (hidPad.present) {
        const float axes[] = {hidPad.lx / 32767.f, hidPad.ly / 32767.f, hidPad.rx / 32767.f, hidPad.ry / 32767.f};
        gamepad_game_hid_input(hidPad.buttons, axes);
        next.connected = 1;
        physical |= hidPad.buttons;
        next.lx = stronger_axis(next.lx, hidPad.lx);
        next.ly = stronger_axis(next.ly, hidPad.ly);
        next.rx = stronger_axis(next.rx, hidPad.rx);
        next.ry = stronger_axis(next.ry, hidPad.ry);
#ifdef RUNTIME_STICK_CALIBRATION
        captureActive |= hidCaptureActive;
        if (fabsf(hidRawLx) > fabsf(rawLx)) rawLx = hidRawLx;
        if (fabsf(hidRawLy) > fabsf(rawLy)) rawLy = hidRawLy;
        if (fabsf(hidRawRx) > fabsf(rawRx)) rawRx = hidRawRx;
        if (fabsf(hidRawRy) > fabsf(rawRy)) rawRy = hidRawRy;
        if (hidPad.buttons & (1ULL << 8)) rawLT = 1.0f;   /* the HID fallback's triggers are buttons */
        if (hidPad.buttons & (1ULL << 9)) rawRT = 1.0f;
#endif
    }
#ifdef RUNTIME_STICK_CALIBRATION
    stick_response_limit(&next.lx, &next.ly);
    stick_response_limit(&next.rx, &next.ry);
#endif
#if TARGET_OS_OSX
    /* A DSU server's pad (dsu_client.h): its buttons (the physical order, controller_map.h) and sticks join the controllers'. */
    if (dsuInput.connected && dsuAt && runtime_now_ns() - dsuAt < 500000000ull) {
        next.connected = 1; physical |= dsuInput.buttons & CONTROLLER_OWN_BUTTONS;   /* never the mouse's buttons (games/acnh) */
        float axes[4];
        for (int i = 0; i < 4; i++) axes[i] = (dsuInput.axes[i] - 128) / (dsuInput.axes[i] >= 128 ? 127.f : 128.f);
#ifdef RUNTIME_STICK_CALIBRATION
        captureActive |= hypotf(axes[0],axes[1]) > .12f || hypotf(axes[2],axes[3]) > .12f;
#endif
        next.lx = stronger_axis(next.lx, stick_axis(axes[0])); next.ly = stronger_axis(next.ly, stick_axis(axes[1]));
        next.rx = stronger_axis(next.rx, stick_axis(axes[2])); next.ry = stronger_axis(next.ry, stick_axis(axes[3]));
        gamepad_game_dsu_input(dsuInput.buttons || stick_axis(axes[0]) || stick_axis(axes[1]) || stick_axis(axes[2]) || stick_axis(axes[3]));
    }
#endif
    /* Modes work on the physical buttons, not on the mapped ones: two physical buttons may share a Switch button
       (X and L3 both running), and a mapped edge would be lost while the other one is held. */
    if (capture && !captureSeen && captureHook) captureHook();
    captureSeen = capture;
    uint64_t now = runtime_now_ns(), edges = physical & ~physicalPrev, synthetic = 0, suppressed = 0, trigger = settings_trigger();
    int shotButton = atomic_load(&screenshotButton), shotHeld = shotButton >= 0 && (physical >> shotButton & 1), shotFire = 0;
    if (atomic_load(&settingsButton) >= 0) suppressed |= trigger;   /* a picked button is the panel's only */
    int moving = next.lx || next.ly;
    physicalPrev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(controllerModes[i]);
        if (!textCapture && !(settingsShortcutEnabled && (physical & trigger) == trigger) && kind == CONTROLLER_TAP && (edges & bit)) {
            tapUntil[i] = now + 400000000ULL;
            /* Release the pulse even if the controller sends no further event. */
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(400 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
        }
        if (kind == CONTROLLER_TOGGLE) {
            if (!textCapture && !(settingsShortcutEnabled && (physical & trigger) == trigger) && (edges & bit)) { toggleOn[i] = !toggleOn[i]; toggleMoved[i] = moving; }
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
    int chord = settingsShortcutEnabled && (physical & trigger) == trigger;
    void (*tap)(void) = gamepad_game_chord_tap();
    if (tap) {
        /* The game gives the chord a tap of its own (zeldaVR running: "同时 按下 l3 r3 就自动 “对准正前方”", user
           2026-10-30): tapped it does that, held 0.8 s it opens the settings. The game gets nothing from the press either way. */
        static uint64_t chordAt; static int chordLong;
        if (chord && !settingsShortcutDown) {
            chordAt = now; chordLong = 0;
            if (!textCapture) textCapture = 2;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(850 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
        }
        if (chord && !chordLong && now - chordAt >= 800000000ULL) { chordLong = 1; settingsShortcutPending = 1; }
        if (!chord && settingsShortcutDown && !chordLong) tap();
    } else if (chord && !settingsShortcutDown) {
        settingsShortcutPending = 1;
        if (!textCapture) textCapture = 2;   /* from here the game no longer gets the controller, until it is released */
    }
    settingsShortcutDown = chord;
    /* The screenshot button held CONTROLLER_SCREENSHOT_HOLD_MS: one picture per hold. A button held still sends no report,
       so the refresh comes again when the time is up. What else the button does is untouched. */
    int shotStarted = shotHeld && !screenshotHold.down;
    screenshotHold = controller_hold_next(screenshotHold, shotHeld, now, CONTROLLER_SCREENSHOT_HOLD_MS * 1000000ull, &shotFire);
    uint64_t shotWait = shotStarted ? controller_hold_wait(screenshotHold, now, CONTROLLER_SCREENSHOT_HOLD_MS * 1000000ull) : UINT64_MAX;
    textEdges |= (physical & CONTROLLER_OWN_BUTTONS) & ~textPad.buttons;
    textPad = next; textPad.buttons = physical & CONTROLLER_OWN_BUTTONS;
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    int changed = next.buttons != gamepadState.buttons || next.lx != gamepadState.lx || next.ly != gamepadState.ly ||
                  next.rx != gamepadState.rx || next.ry != gamepadState.ry;
    gamepadState = next;
    gamepadRawLeftAt = rawAt; gamepadRawRightAt = rawRightAt;
    gamepadNativeLx = nativeLx; gamepadNativeLy = nativeLy; gamepadNativeRx = nativeRx; gamepadNativeRy = nativeRy;
#ifdef RUNTIME_STICK_CALIBRATION
    calibration_sample_update();
    captureStickActive=captureActive;
    captureRawLeftActive=rawLeftCapture;captureRawRightActive=rawRightCapture;
    captureRawLeftAt=rawAt;captureRawRightAt=rawRightAt;
    gamepadRawLx=rawLx; gamepadRawLy=rawLy; gamepadRawRx=rawRx; gamepadRawRy=rawRy;
    gamepadRawLT=rawLT; gamepadRawRT=rawRT;
#endif
    pthread_mutex_unlock(&gamepadLock);
    if (changed) {
        uint64_t trace = gamepad_game_trace_row();
        if (trace) fprintf(stderr, GAMEPAD_TRACE_TAG " %llu t=%llu mapped connected=%d physical=%#llx buttons=%#llx axes=%d,%d,%d,%d text_capture=%d\n",
                           (unsigned long long)trace, (unsigned long long)now, next.connected, (unsigned long long)physical,
                           (unsigned long long)next.buttons, next.lx, next.ly, next.rx, next.ry, atomic_load(&textCapture));
    }
    if (shotWait != UINT64_MAX)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(shotWait + 20 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
    if (shotFire) {
        fprintf(stderr, "[totk-input] screenshot: %s held %d s\n", controller_physical_names[shotButton], CONTROLLER_SCREENSHOT_HOLD_MS / 1000);
        if (captureHook) captureHook();   /* the capture button's own path (screenshot.h screenshot_request) */
    }
    if (changed && next.connected) awake_note_input();
}

static void attach_gamepad(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad) return;
    __weak GCController *source = controller;
    pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
        uint64_t trace = gamepad_game_trace_row();
        if (trace) {
            GCController *device = source;
            fprintf(stderr, GAMEPAD_TRACE_TAG " %llu t=%llu callback device=%p vendor=%s element=%s physical=%#llx axes=%g,%g,%g,%g background=%d\n",
                    (unsigned long long)trace, (unsigned long long)runtime_now_ns(), (__bridge void *)device,
                    (device.vendorName ?: @"unknown").UTF8String, (element.localizedName ?: @"unknown").UTF8String,
                    (unsigned long long)gamepad_buttons(changed, face_by_position(device)),
                    changed.leftThumbstick.xAxis.value, changed.leftThumbstick.yAxis.value,
                    changed.rightThumbstick.xAxis.value, changed.rightThumbstick.yAxis.value, runtime_gamepad_background_enabled());
        }
        refresh_gamepads();
    };
    gamepad_attach_note(controller);
    runtime_feedback_attach(controller);
    refresh_gamepads();
}

static void detach_gamepad(GCController *controller) {
#ifdef RUNTIME_STICK_CALIBRATION
    if (calibrationOwner==controller) calibration_unbind();
    if (calibrationSampleController==controller) calibration_sample_invalidate();
#endif
    controller.extendedGamepad.valueChangedHandler = nil;
    gamepad_forget_raw(controller);
    runtime_feedback_detach(controller);
    refresh_gamepads();
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
    gamepad_mapping_reset();
    pthread_mutex_lock(&gamepadLock);
    gamepadState = (RuntimeGamepadState){0};
    gamepadRawLeftAt = 0; gamepadRawRightAt = 0;
    gamepadNativeLx = gamepadNativeLy = 0;
#ifdef RUNTIME_STICK_CALIBRATION
    gamepadRawLx = gamepadRawLy = gamepadRawRx = gamepadRawRy = 0;
    gamepadRawLT = gamepadRawRT = 0;
    captureStickActive=captureRawLeftActive=captureRawRightActive=0;
    calibration_sample_invalidate();
#endif
    pressedEdges = 0; textEdges = 0; textPad = (RuntimeGamepadState){0};
    settingsShortcutDown = settingsShortcutPending = 0;
    captureDown = captureSeen = 0;   /* a button held at focus loss must not shoot on the way back */
    screenshotHold = (ControllerHold){0};   /* nor finish a screenshot hold */
    mouse_reset_locked();
    pthread_mutex_unlock(&gamepadLock);
}

/* On-screen touch controls (iPhone, ios/app/Sources/touch_controls.m) feed a second state merged with the physical
   controllers: buttons are OR-ed (a tap shorter than one game frame is latched like a controller press), each stick
   axis takes the stronger value. */
void runtime_touch_pad_set(const RuntimeGamepadState *s) {
    pthread_mutex_lock(&gamepadLock);
    pressedEdges |= s->buttons & ~touchState.buttons;
    touchState = *s;
    pthread_mutex_unlock(&gamepadLock);
}

/* 1: the panel or the text entry bar owns the controller (the game reads a released one); 0: back to the game once
   everything is released. */
void runtime_gamepad_text_capture(int on) {
    pthread_mutex_lock(&gamepadLock);
    textCapture = on ? 1 : 2;
    textEdges = pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
    /* UI actions must not start persistent gameplay tap/toggle bindings. */
    memset(tapUntil, 0, sizeof tapUntil); memset(toggleOn, 0, sizeof toggleOn); memset(toggleMoved, 0, sizeof toggleMoved);
    refresh_gamepads();
}
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
/* The physical buttons (controller_map.h order) held or pressed since the last read, and the sticks, for the panel. */
void runtime_gamepad_text_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    *out = text_state_locked(); out->buttons |= textEdges; textEdges = 0;
    gamepad_game_merge(out, 1);   /* the settings panel and the text keyboard: the game's other pads by name (zeldaVR's Quest) */
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
    mouse_stick_merge_locked(out);
    if (touchState.connected) out->connected = 1;
    gamepad_game_merge(out, 0);   /* the game's other pads, merged like the touch screen (zeldaVR's Quest 3 controllers) */
    pressedEdges = 0;
    if (textCapture) {
        RuntimeGamepadState held = text_state_locked();
        if (textCapture == 2 && !held.buttons &&
#ifdef RUNTIME_STICK_CALIBRATION
            !capture_active_locked()
#else
            !held.lx && !held.ly && !held.rx && !held.ry
#endif
        )
            textCapture = 0;
        *out = (RuntimeGamepadState){.connected = out->connected};
    }
    uint64_t trace = gamepad_game_trace_row();
    if (trace) fprintf(stderr, GAMEPAD_TRACE_TAG " %llu t=%llu read connected=%d buttons=%#llx axes=%d,%d,%d,%d text_capture=%d\n",
                       (unsigned long long)trace, (unsigned long long)runtime_now_ns(), out->connected, (unsigned long long)out->buttons,
                       out->lx, out->ly, out->rx, out->ry, atomic_load(&textCapture));
    pthread_mutex_unlock(&gamepadLock);
}
