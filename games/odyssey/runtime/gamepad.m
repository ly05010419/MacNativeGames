/* Native macOS controllers feed the same FullKey state as the keyboard, for one player or two (Mario and Cappy): this
   game's own seats, layout per seat and stick reach. The plumbing every game shares is runtime/gamepad_core.h
   (shared/README.md); this game's hooks are in runtime/gamepad_game.h. */
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
#include "controller_map.h"   // the player's button layout (controls.txt), with this game's stick reach lines

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
static RuntimeGamepadState touchState;
static void update_feedback_controllers(void);

#include "gamepad_game.h"   /* this game's hooks: mouse mode, the stick reach */
#include "gamepad_core.h"   /* the shared plumbing (shared/runtime) */

static int settings_chord_held(void) {    /* gamepadLock held */
    uint64_t trigger = settings_trigger();
    if (!twoPlayers) return (shared.native.buttons & trigger) == trigger;
    return (samples[0].native.buttons & trigger) == trigger || (samples[1].native.buttons & trigger) == trigger;
}

static void gamepad_mapping_reset(void) {   /* main thread: every seat's taps and toggles */
    pthread_mutex_lock(&gamepadLock);
    memset(mappings, 0, sizeof mappings);
    pthread_mutex_unlock(&gamepadLock);
}

static void forget_seat(int seat) {      /* physical disconnect: discard the pad's pulses, keep other input sources */
    if (seat < 0) return;
    pthread_mutex_lock(&gamepadLock);
    if (twoPlayers) mappings[seat] = (PadMapping){0};
    pthread_mutex_unlock(&gamepadLock);
}

/* A stick axis (0 lx, 1 ly, 2 rx, 3 ry) through the worn stick's reach (gamepad_game.h), then the dead zone. */
static int32_t reach_axis(int axis, float value) { return stick_axis(gamepad_game_stick(axis, value)); }

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
        .lx = reach_axis(0, lx), .ly = reach_axis(1, ly), .rx = reach_axis(2, rx), .ry = reach_axis(3, ry)}};
    int wasActive = rawLeft.active;
    if (controller == rawLeftController && switch_left_get(&rawLeft, lx, ly, runtime_now_ns(), &lx, &ly)) {
        s.leftAt = rawLeft.at; s.lx = reach_axis(0, lx); s.ly = reach_axis(1, ly);
        if (!wasActive) fprintf(stderr, "[totk-input] Switch left stick: native axes stuck at zero; using live HID input\n");
    }
    wasActive = rawRight.active;
    if (controller == rawRightController && switch_left_get(&rawRight, rx, ry, runtime_now_ns(), &rx, &ry)) {
        s.rightAt = rawRight.at; s.rx = reach_axis(2, rx); s.ry = reach_axis(3, ry);
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
    int capture = captureDown;
    if (!hidPad.present && hidSeat >= 0) { forget_seat(hidSeat); hidSeat = -1; }   /* the HID pad went away (gamepad_hid_pad.h) */
    for (GCController *controller in controllers) {
        int seat = controller == seats[0] ? 0 : controller == seats[1] ? 1 : free_seat();
        PadSample pad = sample_controller(controller);
        if (!capture && controller.extendedGamepad.buttons[GCInputButtonShare].isPressed) capture = 1;   /* the capture button (see runtime_capture_set_hook) */
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
#if TARGET_OS_OSX
    /* A DSU server's pad (dsu_client.h): its buttons and sticks join the other pads that have no seat of their own. */
    if (dsuInput.connected && dsuAt && runtime_now_ns() - dsuAt < 500000000ull) {
        float axes[4];
        for (int i = 0; i < 4; i++) axes[i] = (dsuInput.axes[i] - 128) / (dsuInput.axes[i] >= 128 ? 127.f : 128.f);
        RuntimeGamepadState pad = {.connected = 1, .buttons = dsuInput.buttons,
            .lx = stick_axis(axes[0]), .ly = stick_axis(axes[1]), .rx = stick_axis(axes[2]), .ry = stick_axis(axes[3])};
        merge_pad(&next[EXTRA_PADS].native, &pad);
    }
#endif
    next[0].native.buttons |= mouse_physical();
    PadSample combined = combined_sample(next);
    if (capture && !captureSeen && captureHook) captureHook();
    captureSeen = capture;
    uint64_t now = runtime_now_ns();
    int shotButton = atomic_load(&screenshotButton), shotHeld = shotButton >= 0 && (combined.native.buttons >> shotButton & 1), shotFire = 0;
    pthread_mutex_lock(&gamepadLock);
    const RuntimeGamepadState *was = &shared.native, *is = &combined.native;
    int changed = is->buttons != was->buttons || is->lx != was->lx || is->ly != was->ly || is->rx != was->rx || is->ry != was->ry;
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
    /* The screenshot button held CONTROLLER_SCREENSHOT_HOLD_MS: one picture per hold. A button held still sends no report,
       so the refresh comes again when the time is up. What else the button does is untouched. */
    int shotStarted = shotHeld && !screenshotHold.down;
    screenshotHold = controller_hold_next(screenshotHold, shotHeld, now, CONTROLLER_SCREENSHOT_HOLD_MS * 1000000ull, &shotFire);
    uint64_t shotWait = shotStarted ? controller_hold_wait(screenshotHold, now, CONTROLLER_SCREENSHOT_HOLD_MS * 1000000ull) : UINT64_MAX;
    pthread_mutex_unlock(&gamepadLock);
    if (shotWait != UINT64_MAX)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(shotWait + 20 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{ refresh_gamepads(); });
    if (shotFire) {
        fprintf(stderr, "[totk-input] screenshot: %s held %d s\n", controller_physical_names[shotButton], CONTROLLER_SCREENSHOT_HOLD_MS / 1000);
        if (captureHook) captureHook();   /* the capture button's own path (screenshot.h screenshot_request) */
    }
    if (changed && combined.native.connected) awake_note_input();   /* the display stays awake while a controller is used */
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
    gamepad_attach_note(controller);
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
    gamepad_forget_raw(controller);
    refresh_gamepads();
    update_feedback_controllers();
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
        mouse_reset_locked();
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
        mouse_stick_merge_locked(out);
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
