/* Native controllers feed stable FullKey player slots. The legacy/iOS path
   keeps one aggregate input; LM3 on Mac separates two full-size controllers. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "gamepad_layout.h"   // face buttons by position (PlayStation, Xbox) or label (Nintendo)
#include "controller_map.h"   // the player's button layout (controls.txt)

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
#if defined(GAME_LM3) && TARGET_OS_OSX
#define LM3_GAMEPAD_TWO_PLAYERS 1
enum { NATIVE_GAMEPAD_PLAYERS = 2 };
static GCController *playerControllers[RUNTIME_GAMEPAD_PLAYERS];   /* main queue, strong references */
#else
#define LM3_GAMEPAD_TWO_PLAYERS 0
enum { NATIVE_GAMEPAD_PLAYERS = 1 };
#endif
typedef struct {
    uint64_t physicalPrev, tapUntil[CONTROLLER_BUTTONS];
    uint8_t toggleOn[CONTROLLER_BUTTONS], toggleMoved[CONTROLLER_BUTTONS];
} GamepadModes;
static RuntimeGamepadState gamepadState[RUNTIME_GAMEPAD_PLAYERS];
static uint64_t pressedEdges[RUNTIME_GAMEPAD_PLAYERS], generation[RUNTIME_GAMEPAD_PLAYERS], inputEpoch;
static GamepadModes modeState[RUNTIME_GAMEPAD_PLAYERS];
static RuntimeGamepadState latestPhysical[RUNTIME_GAMEPAD_PLAYERS];
static uint64_t blockedPhysical[RUNTIME_GAMEPAD_PLAYERS];
static unsigned blockedAxes[RUNTIME_GAMEPAD_PLAYERS];
static unsigned physicalConnected;
static char playerNames[RUNTIME_GAMEPAD_PLAYERS][160];
static BOOL started;
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
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
static void copy_player_name(char *out, size_t size, const char *name) {
    if (!out || !size) return;
    if (!name) name = "";
    size_t n = strlen(name);
    if (n >= size) {
        n = size - 1;
        while (n && ((unsigned char)name[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out, name, n); out[n] = 0;
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

/* An overlay (the cheat panel, frontend.m) may take the controllers: the hook sees the held and newly pressed buttons
   (left stick up / down as bits 16 / 17) and returns 1 to keep them from the game. Buttons still held when it lets go
   stay hidden from the game until released, so the button that closed the overlay does not also act in the game. */
static int (*overlayHook)(uint64_t held, uint64_t pressed);
static uint64_t overlayPrev, overlaySwallow[RUNTIME_GAMEPAD_PLAYERS], overlayHeld[RUNTIME_GAMEPAD_PLAYERS];
static RuntimeGamepadState touchState;
static uint64_t touchEdges, touchSwallow, touchOverlayHeld;
static unsigned touchBlockedAxes;
static int overlayActive;
void runtime_gamepad_set_overlay(int (*hook)(uint64_t held, uint64_t pressed)) {
    pthread_mutex_lock(&gamepadLock); overlayHook = hook; pthread_mutex_unlock(&gamepadLock);
}
/* Pure production mode processing: no input, edge, tap, toggle or aim state is
   shared between players. The caller schedules only the newly started taps. */
static RuntimeGamepadState map_gamepad_state(GamepadModes *m, RuntimeGamepadState raw,
                                             const uint16_t map[CONTROLLER_BUTTONS],
                                             const uint8_t modes[CONTROLLER_BUTTONS], uint64_t now, uint64_t *taps) {
    *taps = 0;
    if (!raw.connected) { memset(m, 0, sizeof *m); return (RuntimeGamepadState){0}; }
    RuntimeGamepadState next = raw; next.buttons = 0;
    uint64_t physical = raw.buttons;
    /* Modes work on the physical buttons, not on the mapped ones: two physical buttons may share a Switch button
       (X and L3 both running), and a mapped edge would be lost while the other one is held. */
    uint64_t edges = physical & ~m->physicalPrev, synthetic = 0, suppressed = 0;
    int moving = next.lx || next.ly;
    m->physicalPrev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(modes[i]);
        if (kind == CONTROLLER_TAP && (edges & bit)) {
            m->tapUntil[i] = now + 400000000ULL;
            *taps |= bit;
        }
        if (kind == CONTROLLER_TOGGLE) {
            if (edges & bit) { m->toggleOn[i] = !m->toggleOn[i]; m->toggleMoved[i] = moving; }
            if (m->toggleOn[i] && moving) m->toggleMoved[i] = 1;
            /* A toggle ends when the stick returns to center after moving, or the controller goes away; clicking at rest
               waits for the first move, and attacking while running does not stop it. */
            if (m->toggleOn[i] && m->toggleMoved[i] && !moving) m->toggleOn[i] = 0;
        }
        if (kind == CONTROLLER_AIM && (physical & bit)) {
            /* Aim: the Switch button is held while drawing, released while the fire button is down (the shot), and its
               fire button does nothing else meanwhile. */
            uint64_t fire = 1ULL << controller_mode_fire(modes[i]);
            suppressed |= fire;
            if (!(physical & fire)) synthetic |= map[i];
        }
        int active = kind == CONTROLLER_TAP ? now < m->tapUntil[i] : kind == CONTROLLER_TOGGLE ? m->toggleOn[i] : 0;
        if (active) synthetic |= map[i];
    }
    uint64_t held = physical & ~suppressed;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) if (modes[i] != CONTROLLER_HOLD) held &= ~(1ULL << i);
    next.buttons = controller_map_apply(map, held) | synthetic;
    return next;
}
/* gamepadLock held. Focus clearing keeps physical presence, so opening a Mac
   sheet does not look like player 2 unplugging the controller. */
static void mask_axes_until_center(RuntimeGamepadState *state, unsigned *blocked) {
    int32_t *axes[] = {&state->lx, &state->ly, &state->rx, &state->ry};
    for (unsigned i = 0; i < 4; i++) if (*blocked & (1u << i)) {
        if (!*axes[i]) *blocked &= ~(1u << i);
        *axes[i] = 0;
    }
}
static unsigned held_axes(RuntimeGamepadState state) {
    return (state.lx ? 1u : 0) | (state.ly ? 2u : 0) | (state.rx ? 4u : 0) | (state.ry ? 8u : 0);
}
static void filter_physical_locked(unsigned player, RuntimeGamepadState *raw) {
    latestPhysical[player] = *raw;
    blockedPhysical[player] &= raw->buttons;
    raw->buttons &= ~blockedPhysical[player];
    mask_axes_until_center(raw, &blockedAxes[player]);
}
/* A clear can run on a guest thread while the main thread samples the native
   device. An overlapping sample must not turn its old held input into a new
   post-clear press, even if the previous cached sample had still been neutral. */
static void swallow_crossed_sample_locked(unsigned player, RuntimeGamepadState raw, uint64_t expectedGeneration, uint64_t expectedEpoch) {
    if (generation[player] != expectedGeneration || inputEpoch != expectedEpoch) {
        blockedPhysical[player] |= raw.buttons;
        blockedAxes[player] |= held_axes(raw);
    }
}
static void clear_player_locked(unsigned player, int swallow) {
    if (swallow) {
        blockedPhysical[player] |= latestPhysical[player].buttons;
        blockedAxes[player] |= held_axes(latestPhysical[player]);
    } else {
        latestPhysical[player] = (RuntimeGamepadState){0};
        blockedPhysical[player] = blockedAxes[player] = 0;
    }
    generation[player]++;
    memset(&modeState[player], 0, sizeof modeState[player]);
    gamepadState[player] = (RuntimeGamepadState){.connected = !!(physicalConnected & (1u << player))};
    pressedEdges[player] = overlaySwallow[player] = overlayHeld[player] = 0;
}
static void publish_player_locked(unsigned player, RuntimeGamepadState next) {
    if (!next.connected) {
        next = (RuntimeGamepadState){0}; pressedEdges[player] = 0;
    } else pressedEdges[player] |= next.buttons & ~gamepadState[player].buttons;
    gamepadState[player] = next;
}
static void apply_overlay_locked(RuntimeGamepadState next[RUNTIME_GAMEPAD_PLAYERS], int take) {
    if (overlayActive && !take) {
        for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) overlaySwallow[p] = next[p].buttons & overlayHeld[p];
        touchSwallow = touchState.buttons & touchOverlayHeld;
    }
    overlayActive = take;
    for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) {
        if (take) {
            overlayHeld[p] = next[p].buttons;
            pressedEdges[p] = 0;
            next[p] = (RuntimeGamepadState){.connected = next[p].connected};
        } else {
            overlaySwallow[p] &= next[p].buttons;
            next[p].buttons &= ~overlaySwallow[p];
        }
    }
    if (take) { touchOverlayHeld = touchState.buttons; touchEdges = 0; }
}
/* Identity, not enumeration order or vendor/product strings, owns a slot.
   Existing players never move when the other player disconnects. */
static int slot_for_identity(const void *const identities[RUNTIME_GAMEPAD_PLAYERS], const void *identity, unsigned count) {
    if (!identity) return -1;
    for (unsigned p = 0; p < count; p++) if (identities[p] == identity) return (int)p;
    for (unsigned p = 0; p < count; p++) if (!identities[p]) return (int)p;
    return -1;
}
static RuntimeGamepadState physical_state(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad) return (RuntimeGamepadState){0};
    return (RuntimeGamepadState){.connected = 1, .buttons = gamepad_buttons(pad, face_by_position(controller)),
        .lx = stick_axis(pad.leftThumbstick.xAxis.value), .ly = stick_axis(pad.leftThumbstick.yAxis.value),
        .rx = stick_axis(pad.rightThumbstick.xAxis.value), .ry = stick_axis(pad.rightThumbstick.yAxis.value)};
}
static void refresh_gamepads(void) {   /* main queue: controller callbacks, notifications and tap expiry */
    RuntimeGamepadState raw[RUNTIME_GAMEPAD_PLAYERS] = {0}, next[RUNTIME_GAMEPAD_PLAYERS] = {0};
    uint64_t taps[RUNTIME_GAMEPAD_PLAYERS] = {0}, stamp[RUNTIME_GAMEPAD_PLAYERS], epoch, seen, pressed;
    int (*hook)(uint64_t, uint64_t);
    pthread_mutex_lock(&gamepadLock);
    epoch = inputEpoch;
    for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) stamp[p] = generation[p];
    pthread_mutex_unlock(&gamepadLock);
#if LM3_GAMEPAD_TWO_PLAYERS
    for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) if (playerControllers[p]) raw[p] = physical_state(playerControllers[p]);
#else
    /* Existing iOS/single-player semantics: all physical devices feed player 1. */
    for (GCController *controller in GCController.controllers) {
        RuntimeGamepadState s = physical_state(controller);
        raw[0].connected |= s.connected; raw[0].buttons |= s.buttons;
        raw[0].lx = stronger_axis(raw[0].lx, s.lx); raw[0].ly = stronger_axis(raw[0].ly, s.ly);
        raw[0].rx = stronger_axis(raw[0].rx, s.rx); raw[0].ry = stronger_axis(raw[0].ry, s.ry);
    }
#endif
    pthread_mutex_lock(&gamepadLock);
    hook = overlayHook;
    for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) {
#if !LM3_GAMEPAD_TWO_PLAYERS
        physicalConnected = raw[0].connected ? 1u : 0;
#endif
        swallow_crossed_sample_locked(p, raw[p], stamp[p], epoch);
        filter_physical_locked(p, &raw[p]);
        if (overlayActive) {
            /* Panel navigation must not change a player's tap/toggle/aim. */
            memset(&modeState[p], 0, sizeof modeState[p]); modeState[p].physicalPrev = raw[p].buttons;
            next[p] = raw[p]; next[p].buttons = controller_map_apply(controllerMap, raw[p].buttons);
        } else next[p] = map_gamepad_state(&modeState[p], raw[p], controllerMap, controllerModes, runtime_now_ns(), &taps[p]);
    }
    /* Only player 1 can drive a host overlay; two people's L3/R3 cannot combine. */
    seen = raw[0].buttons | (raw[0].ly > 16384 ? 1ULL << 16 : 0) | (raw[0].ly < -16384 ? 1ULL << 17 : 0);
    pressed = seen & ~overlayPrev; overlayPrev = seen;
    pthread_mutex_unlock(&gamepadLock);
    /* The hook may open a panel and call runtime_input_clear: never call it
       while holding gamepadLock, and do not republish a pre-clear snapshot. */
    int take = hook ? hook(seen, pressed) : 0;
    pthread_mutex_lock(&gamepadLock);
    if (epoch != inputEpoch && !take) { pthread_mutex_unlock(&gamepadLock); return; }
    apply_overlay_locked(next, take);
    for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) {
        if (stamp[p] != generation[p] && !take) continue;
        next[p].connected = !!(physicalConnected & (1u << p));
        publish_player_locked(p, next[p]);
    }
    pthread_mutex_unlock(&gamepadLock);
    for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) if (taps[p]) {
        unsigned player = p; uint64_t expectedGeneration = stamp[p];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(400 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
            pthread_mutex_lock(&gamepadLock); int valid = generation[player] == expectedGeneration; pthread_mutex_unlock(&gamepadLock);
            if (valid) refresh_gamepads();
        });
    }
}

static void attach_gamepad(GCController *controller) {
    GCExtendedGamepad *pad = controller.extendedGamepad;
    if (!pad) return;
#if LM3_GAMEPAD_TWO_PLAYERS
    const void *identities[RUNTIME_GAMEPAD_PLAYERS];
    for (unsigned p = 0; p < RUNTIME_GAMEPAD_PLAYERS; p++) identities[p] = (__bridge const void *)playerControllers[p];
    int player = slot_for_identity(identities, (__bridge const void *)controller, NATIVE_GAMEPAD_PLAYERS);
    if (player < 0) {
        fprintf(stderr, "[totk-input] extra native gamepad waiting for a free player slot: %s\n", (controller.vendorName ?: @"unknown").UTF8String);
        return;
    }
    if (playerControllers[player] == controller) return;
    playerControllers[player] = controller;
    controller.playerIndex = (GCControllerPlayerIndex)player;
    pthread_mutex_lock(&gamepadLock);
    physicalConnected |= 1u << player; clear_player_locked((unsigned)player, 0);
    NSString *name = [NSString stringWithFormat:@"%@ (%@)", controller.vendorName ?: @"Controller", controller.productCategory ?: @"Unknown"];
    copy_player_name(playerNames[player], sizeof playerNames[player], name.UTF8String);
    pthread_mutex_unlock(&gamepadLock);
    uint64_t assigned;
    pthread_mutex_lock(&gamepadLock); assigned = generation[player]; pthread_mutex_unlock(&gamepadLock);
#endif
    controller.handlerQueue = dispatch_get_main_queue();
    pad.valueChangedHandler = ^(GCExtendedGamepad *changed, GCControllerElement *element) {
        (void)changed; (void)element;
#if LM3_GAMEPAD_TWO_PLAYERS
        pthread_mutex_lock(&gamepadLock); int valid = generation[player] == assigned; pthread_mutex_unlock(&gamepadLock);
        /* Focus clearing invalidates timers, not the live device handler. */
        if (!valid && playerControllers[player] != controller) return;
#endif
        refresh_gamepads();
    };
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s), face buttons by %s\n",
            (controller.vendorName ?: @"unknown").UTF8String,
            (controller.productCategory ?: @"unknown").UTF8String, face_by_position(controller) ? "position" : "label");
#if LM3_GAMEPAD_TWO_PLAYERS
    fprintf(stderr, "[totk-input] native gamepad assigned: player %d\n", player + 1);
    runtime_feedback_attach_player(controller, (unsigned)player);
#else
    pthread_mutex_lock(&gamepadLock);
    copy_player_name(playerNames[0], sizeof playerNames[0], (controller.vendorName ?: @"Controller").UTF8String);
    pthread_mutex_unlock(&gamepadLock);
    runtime_feedback_attach(controller);
#endif
    refresh_gamepads();
}
static void detach_gamepad(GCController *controller) {
    controller.extendedGamepad.valueChangedHandler = nil;
#if LM3_GAMEPAD_TWO_PLAYERS
    for (unsigned p = 0; p < NATIVE_GAMEPAD_PLAYERS; p++) if (playerControllers[p] == controller) {
        playerControllers[p] = nil; controller.playerIndex = GCControllerPlayerIndexUnset;
        pthread_mutex_lock(&gamepadLock);
        physicalConnected &= ~(1u << p); clear_player_locked(p, 0); playerNames[p][0] = 0;
        if (!p) overlayPrev = 0;
        pthread_mutex_unlock(&gamepadLock);
        runtime_feedback_detach_player(controller, p);
        fprintf(stderr, "[totk-input] native gamepad player %u disconnected\n", p + 1);
        break;
    }
    /* A third controller may fill a vacancy, but never pushes a live player
       into a different slot. Ignore the notification's departing object. */
    for (GCController *candidate in GCController.controllers) if (candidate != controller) attach_gamepad(candidate);
#else
    runtime_feedback_detach(controller);
#endif
    refresh_gamepads();
}

void runtime_gamepad_start(void) {
    pthread_mutex_lock(&gamepadLock);
    if (started) { pthread_mutex_unlock(&gamepadLock); return; }
    started = YES;
    pthread_mutex_unlock(&gamepadLock);
    /* Reproducible timing checks must not merge a physical stick/button with
       the test keyboard. This opt-in is restricted to our isolated test owner;
       the user's launcher always retains normal controller discovery. */
    const char *isolated=getenv("LM3_TEST_NO_GAMEPAD"),*owner=getenv("TOTK_SLOT_OWNER");
    if(isolated&&!strcmp(isolated,"1")&&owner&&!strcmp(owner,"codex")){
        fprintf(stderr,"[totk-input] isolated keyboard timing test; physical gamepads ignored\n");
        return;
    }
    load_controller_map();
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
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
    pthread_mutex_lock(&gamepadLock);
    inputEpoch++;
    for (unsigned p = 0; p < RUNTIME_GAMEPAD_PLAYERS; p++) clear_player_locked(p, 1);
    touchSwallow |= touchState.buttons; touchBlockedAxes |= held_axes(touchState);
    touchState = (RuntimeGamepadState){.connected = touchState.connected};
    touchEdges = touchOverlayHeld = overlayPrev = 0;
    overlayActive = 0;
    pthread_mutex_unlock(&gamepadLock);
}
void runtime_gamepad_clear_player(unsigned player) {
    if (player >= NATIVE_GAMEPAD_PLAYERS) return;
    pthread_mutex_lock(&gamepadLock);
    clear_player_locked(player, 1);
    if (!player) {
        touchSwallow |= touchState.buttons; touchBlockedAxes |= held_axes(touchState);
        touchState = (RuntimeGamepadState){.connected = touchState.connected};
        touchEdges = touchOverlayHeld = overlayPrev = 0;
    }
    pthread_mutex_unlock(&gamepadLock);
    runtime_hid_feedback_clear_player(player);
}

/* On-screen touch controls (iPhone, ios/app/Sources/touch_controls.m) feed a second state merged with the physical
   controllers: buttons are OR-ed (a tap shorter than one game frame is latched like a controller press), each stick
   axis takes the stronger value. */
void runtime_touch_pad_set(const RuntimeGamepadState *s) {
    if (!s) return;
    pthread_mutex_lock(&gamepadLock);
    touchSwallow &= s->buttons;
    if (!overlayActive) touchEdges |= s->buttons & ~touchState.buttons & ~touchSwallow;
    touchState = *s;
    mask_axes_until_center(&touchState, &touchBlockedAxes);
    pthread_mutex_unlock(&gamepadLock);
}

/* Cheats: the held buttons without consuming the press edges the game's next read still has to see. */
uint64_t runtime_gamepad_held(void) {
    return runtime_gamepad_held_player(0);
}
uint64_t runtime_gamepad_held_player(unsigned player) {
    if (player >= NATIVE_GAMEPAD_PLAYERS) return 0;
    pthread_mutex_lock(&gamepadLock);
    uint64_t b = gamepadState[player].buttons;
    if (!player && !overlayActive) b |= touchState.buttons & ~touchSwallow;
    pthread_mutex_unlock(&gamepadLock);
    return b;
}
void runtime_gamepad_read(RuntimeGamepadState *out) {
    runtime_gamepad_read_player(0, out);
}
void runtime_gamepad_read_player(unsigned player, RuntimeGamepadState *out) {
    if (!out) runtime_fail("controller read output pointer");
    *out = (RuntimeGamepadState){0};
    if (player >= NATIVE_GAMEPAD_PLAYERS) return;
    pthread_mutex_lock(&gamepadLock);
    *out = gamepadState[player];
    out->buttons |= pressedEdges[player]; pressedEdges[player] = 0;
    if (!player && !overlayActive) {
        out->buttons |= (touchState.buttons & ~touchSwallow) | touchEdges;
        out->lx = stronger_axis(out->lx, touchState.lx); out->ly = stronger_axis(out->ly, touchState.ly);
        out->rx = stronger_axis(out->rx, touchState.rx); out->ry = stronger_axis(out->ry, touchState.ry);
        if (touchState.connected) out->connected = 1;
        touchEdges = 0;
    }
    pthread_mutex_unlock(&gamepadLock);
}
unsigned runtime_gamepad_connected_mask(void) {
    pthread_mutex_lock(&gamepadLock); unsigned mask = physicalConnected; pthread_mutex_unlock(&gamepadLock); return mask;
}
void runtime_gamepad_player_name(unsigned player, char *out, size_t size) {
    if (!out || !size) return;
    pthread_mutex_lock(&gamepadLock);
    copy_player_name(out, size, player < NATIVE_GAMEPAD_PLAYERS ? playerNames[player] : "");
    pthread_mutex_unlock(&gamepadLock);
}

static void require_gamepad_probe(void) {
    const char *owner = getenv("TOTK_SLOT_OWNER"), *probe = getenv("LM3_INPUT_PROBE");
    pthread_mutex_lock(&gamepadLock); int live = started; pthread_mutex_unlock(&gamepadLock);
    if (!LM3_GAMEPAD_TWO_PLAYERS || live || !owner || strcmp(owner, "codex") || !probe || strcmp(probe, "1"))
        runtime_fail("gamepad injection is restricted to the isolated LM3 input probe before controller discovery");
}
/* This publishes already mapped Npad buttons, using the same cached state,
   release filtering and edge latch as live controllers. It never enumerates
   or changes an actual controller, and cannot be enabled in a running game. */
void runtime_gamepad_probe_publish(unsigned player, const RuntimeGamepadState *state) {
    require_gamepad_probe();
    if (player >= NATIVE_GAMEPAD_PLAYERS || !state) runtime_fail("gamepad probe player/state");
    pthread_mutex_lock(&gamepadLock);
    if (started) runtime_fail("gamepad probe cannot publish after controller discovery");
    unsigned bit = 1u << player, before = physicalConnected;
    if (state->connected) physicalConnected |= bit; else physicalConnected &= ~bit;
    if ((before ^ physicalConnected) & bit) clear_player_locked(player, 0);
    copy_player_name(playerNames[player], sizeof playerNames[player], state->connected ? (player ? "Probe controller 2" : "Probe controller 1") : "");
    RuntimeGamepadState next = *state;
    filter_physical_locked(player, &next);
    publish_player_locked(player, next);
    pthread_mutex_unlock(&gamepadLock);
}
static void gamepad_probe_reset(void) {
    pthread_mutex_lock(&gamepadLock);
    physicalConnected = 0; inputEpoch++;
    for (unsigned p = 0; p < RUNTIME_GAMEPAD_PLAYERS; p++) {
        clear_player_locked(p, 0); playerNames[p][0] = 0;
    }
    touchState = (RuntimeGamepadState){0}; touchEdges = touchSwallow = touchOverlayHeld = overlayPrev = 0;
    touchBlockedAxes = 0; overlayActive = 0;
    pthread_mutex_unlock(&gamepadLock);
}
int runtime_gamepad_probe(void) {
    require_gamepad_probe(); gamepad_probe_reset();
    int checked = 0;
#define PAD_CHECK(condition, message) do { if (!(condition)) runtime_fail("gamepad probe: " message); checked++; } while (0)
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS];
    controller_map_default(map, modes);
    GamepadModes a = {0}, b = {0}; uint64_t taps;
    RuntimeGamepadState one = {.connected = 1, .buttons = 1u << 1, .lx = 32767, .ry = -12345};
    RuntimeGamepadState two = {.connected = 1, .buttons = 1u << 2, .lx = -32767, .ry = 23456};
    RuntimeGamepadState out1 = map_gamepad_state(&a, one, map, modes, 100, &taps);
    RuntimeGamepadState out2 = map_gamepad_state(&b, two, map, modes, 100, &taps);
    PAD_CHECK(out1.buttons == 1 && out2.buttons == 8, "players' physical face buttons are independent");
    PAD_CHECK(out1.lx == 32767 && out2.lx == -32767 && out1.ry == -12345 && out2.ry == 23456, "opposite sticks remain independent");
    PAD_CHECK(stick_axis(0.12f) == 0 && stick_axis(-0.12f) == 0 && stick_axis(1) == 32767 && stick_axis(-1) == -32767, "deadzone and axis endpoints");
    const void *identities[RUNTIME_GAMEPAD_PLAYERS] = {NULL, NULL};
    int token1 = 0, token2 = 0, token3 = 0;
    PAD_CHECK(slot_for_identity(identities, &token1, 2) == 0, "first device player 1");
    identities[0] = &token1;
    PAD_CHECK(slot_for_identity(identities, &token2, 2) == 1 && slot_for_identity(identities, &token1, 2) == 0, "second device and idempotent attachment");
    identities[1] = &token2;
    PAD_CHECK(slot_for_identity(identities, &token3, 2) == -1, "third controller waits");
    identities[0] = NULL;
    PAD_CHECK(slot_for_identity(identities, &token2, 2) == 1 && slot_for_identity(identities, &token3, 2) == 0, "survivor never renumbers when vacancy fills");
    PAD_CHECK(slot_for_identity(identities, NULL, 2) == -1, "null device cannot claim a slot");

    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    modes[4] = CONTROLLER_TOGGLE;
    one = (RuntimeGamepadState){.connected = 1, .buttons = 1u << 4, .lx = 20000};
    two = (RuntimeGamepadState){.connected = 1};
    out1 = map_gamepad_state(&a, one, map, modes, 200, &taps);
    out2 = map_gamepad_state(&b, two, map, modes, 200, &taps);
    PAD_CHECK(out1.buttons == 16 && !out2.buttons, "toggle belongs to the player who pressed");
    one.buttons = 0; two.buttons = 1u << 4; two.lx = -20000;
    map_gamepad_state(&b, two, map, modes, 300, &taps);
    two.buttons = 0; two.lx = 0;
    out2 = map_gamepad_state(&b, two, map, modes, 400, &taps);
    out1 = map_gamepad_state(&a, one, map, modes, 400, &taps);
    PAD_CHECK(!out2.buttons && out1.buttons == 16, "one stick centering does not stop the other player's toggle");
    one.lx = 0; out1 = map_gamepad_state(&a, one, map, modes, 500, &taps);
    PAD_CHECK(!out1.buttons, "own stick centering stops toggle");
    one.buttons = 1u << 4; out1 = map_gamepad_state(&a, one, map, modes, 600, &taps);
    one.buttons = 0; out1 = map_gamepad_state(&a, one, map, modes, 700, &taps);
    PAD_CHECK(out1.buttons == 16, "toggle at rest waits for first movement");
    out1 = map_gamepad_state(&a, (RuntimeGamepadState){0}, map, modes, 800, &taps);
    PAD_CHECK(!out1.buttons && !a.toggleOn[4] && !a.physicalPrev, "disconnect clears toggle and previous physical state");

    controller_map_default(map, modes); modes[10] = CONTROLLER_TAP;
    memset(&a, 0, sizeof a); one = (RuntimeGamepadState){.connected = 1, .buttons = 1u << 10};
    out1 = map_gamepad_state(&a, one, map, modes, 1000, &taps);
    PAD_CHECK(out1.buttons == 1024 && taps == (1u << 10), "tap creates a timed pulse");
    one.buttons = 0; out1 = map_gamepad_state(&a, one, map, modes, 400000999, &taps);
    PAD_CHECK(out1.buttons == 1024 && !taps, "tap remains through 400ms minus one ns");
    out1 = map_gamepad_state(&a, one, map, modes, 400001000, &taps);
    PAD_CHECK(!out1.buttons, "tap expires exactly at deadline without a second physical event");
    one.buttons = 1u << 10; map_gamepad_state(&a, one, map, modes, 500001000, &taps);
    out1 = map_gamepad_state(&a, (RuntimeGamepadState){0}, map, modes, 500001001, &taps);
    PAD_CHECK(!out1.buttons && !a.tapUntil[10], "disconnect cancels active tap immediately");

    controller_map_default(map, modes); modes[8] = CONTROLLER_AIM | (9 << 3);
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    one = (RuntimeGamepadState){.connected = 1, .buttons = 1u << 8};
    two = (RuntimeGamepadState){.connected = 1, .buttons = 1u << 9};
    out1 = map_gamepad_state(&a, one, map, modes, 1, &taps);
    out2 = map_gamepad_state(&b, two, map, modes, 1, &taps);
    PAD_CHECK(out1.buttons == 256 && out2.buttons == 512, "one player's aim does not suppress the other's fire");
    one.buttons |= 1u << 9; out1 = map_gamepad_state(&a, one, map, modes, 2, &taps);
    PAD_CHECK(!out1.buttons, "aim fire suppresses only its own two mapped actions");

    one = (RuntimeGamepadState){.connected = 1, .buttons = 1, .lx = 20000};
    two = (RuntimeGamepadState){.connected = 1, .buttons = 2, .lx = -20000};
    runtime_gamepad_probe_publish(0, &one); runtime_gamepad_probe_publish(1, &two);
    one.buttons = two.buttons = 0;
    runtime_gamepad_probe_publish(0, &one); runtime_gamepad_probe_publish(1, &two);
    PAD_CHECK(runtime_gamepad_connected_mask() == 3 && !runtime_gamepad_held_player(0) && !runtime_gamepad_held_player(1), "connected and held queries do not consume short edges");
    runtime_gamepad_read_player(2, &out1);
    PAD_CHECK(!out1.connected && !out1.buttons, "invalid player read stays neutral");
    runtime_gamepad_read_player(0, &out1); runtime_gamepad_read_player(0, &out2);
    PAD_CHECK(out1.buttons == 1 && !out2.buttons && out1.lx == 20000, "player 1 consumes only its own short edge once");
    runtime_gamepad_read_player(1, &out2);
    PAD_CHECK(out2.buttons == 2 && out2.lx == -20000, "player 2 edge survives player 1 reads");
    char name[6]; memset(name, 0xa5, sizeof name); runtime_gamepad_player_name(1, name, 5);
    PAD_CHECK(name[4] == 0 && (unsigned char)name[5] == 0xa5, "player name output is bounded");
    char utf8[3]; copy_player_name(utf8, sizeof utf8, "\xe6\xb1\x89");
    PAD_CHECK(!utf8[0], "truncated player names do not split UTF8 characters");
    one.buttons = 4; two.buttons = 8;
    runtime_gamepad_probe_publish(0, &one); runtime_gamepad_probe_publish(1, &two);
    pthread_mutex_lock(&gamepadLock);
    uint64_t old0 = generation[0], old1 = generation[1];
    clear_player_locked(0, 1);
    PAD_CHECK(generation[0] != old0 && generation[1] == old1 && !pressedEdges[0] && pressedEdges[1] == 8, "one-player reset invalidates only its timers and pending edge");
    pthread_mutex_unlock(&gamepadLock);
    runtime_gamepad_read_player(0, &out1); runtime_gamepad_read_player(1, &out2);
    PAD_CHECK(out1.connected && !out1.buttons && !out1.lx && out2.buttons == 8 && out2.lx == -20000, "reset keeps physical connection and the other player's input");
    runtime_gamepad_probe_publish(0, &one); runtime_gamepad_read_player(0, &out1);
    PAD_CHECK(!out1.buttons && !out1.lx, "reset swallows held button and stick until release/center");
    one.buttons = 0; one.lx = 0; runtime_gamepad_probe_publish(0, &one);
    one.buttons = 4; one.lx = 20000; runtime_gamepad_probe_publish(0, &one); runtime_gamepad_read_player(0, &out1);
    PAD_CHECK(out1.buttons == 4 && out1.lx == 20000, "fresh press and stick after release resume normally");
    /* Deterministically place a clear between the native sample's initial
       stamp and publication, with no held input in the old cached sample. */
    one.buttons = 0; one.lx = 0; runtime_gamepad_probe_publish(0, &one);
    pthread_mutex_lock(&gamepadLock);
    uint64_t sampledGeneration = generation[0], sampledEpoch = inputEpoch;
    clear_player_locked(0, 1); inputEpoch++;
    RuntimeGamepadState crossed = {.connected = 1, .buttons = 64, .rx = 25000};
    swallow_crossed_sample_locked(0, crossed, sampledGeneration, sampledEpoch);
    filter_physical_locked(0, &crossed);
    PAD_CHECK(!crossed.buttons && !crossed.rx && (blockedPhysical[0] & 64) && (blockedAxes[0] & 4),
              "sample spanning a clear is neutral and swallows newly sampled held input");
    publish_player_locked(0, crossed);
    pthread_mutex_unlock(&gamepadLock);
    RuntimeGamepadState stillHeld = {.connected = 1, .buttons = 64, .rx = 25000};
    runtime_gamepad_probe_publish(0, &stillHeld); runtime_gamepad_read_player(0, &out1);
    PAD_CHECK(!out1.buttons && !out1.rx, "sample-clear race remains blocked on later device callbacks");
    runtime_gamepad_probe_publish(0, &one);
    runtime_gamepad_probe_publish(0, &stillHeld); runtime_gamepad_read_player(0, &out1);
    PAD_CHECK(out1.buttons == 64 && out1.rx == 25000, "sample-clear race releases after neutral then fresh input");
    one.buttons = 4; one.lx = 20000;

    RuntimeGamepadState next[RUNTIME_GAMEPAD_PLAYERS] = {one, two};
    pthread_mutex_lock(&gamepadLock);
    apply_overlay_locked(next, 1);
    publish_player_locked(0, next[0]); publish_player_locked(1, next[1]);
    PAD_CHECK(!gamepadState[0].buttons && !gamepadState[1].buttons && !gamepadState[0].lx && !gamepadState[1].lx &&
              !pressedEdges[0] && !pressedEdges[1], "host overlay freezes both players and pending edges");
    next[0] = one; next[1] = two; apply_overlay_locked(next, 0);
    PAD_CHECK(!next[0].buttons && !next[1].buttons, "overlay close swallows both players' held buttons");
    next[0].buttons = next[1].buttons = 0; apply_overlay_locked(next, 0);
    next[0] = one; next[1] = two; apply_overlay_locked(next, 0);
    PAD_CHECK(next[0].buttons == one.buttons && next[1].buttons == two.buttons, "overlay release allows later fresh buttons");
    pthread_mutex_unlock(&gamepadLock);
    gamepad_probe_reset();
    RuntimeGamepadState touch = {.connected = 1, .buttons = 32};
    runtime_touch_pad_set(&touch); touch.buttons = 0; runtime_touch_pad_set(&touch);
    PAD_CHECK(!runtime_gamepad_connected_mask(), "touch cannot create a physical player 2");
    runtime_gamepad_read_player(1, &out2); runtime_gamepad_read_player(0, &out1);
    PAD_CHECK(!out2.connected && !out2.buttons && out1.connected && out1.buttons == 32, "touch edge stays on player 1");
    gamepad_probe_reset();
#undef PAD_CHECK
    fprintf(stderr, "[totk-input] gamepad slot probe: %d checks; mapping, independent modes/edges, fixed identities, reset generations and overlays pass\n", checked);
    return checked;
}
