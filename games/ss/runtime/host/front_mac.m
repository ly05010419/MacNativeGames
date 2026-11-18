// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard and controllers → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// The window is fullscreen-capable (the green button and ⌃⌘F); DM_FULLSCREEN starts it that way. ⌘Q quits; the left mouse button is the touchscreen.
// Cheats (hle_cheats.c): the Cheats menu's rows toggle, reload (⌘R) and open the cheats folder; its first item "Show Cheats
// Window" (⌘⇧C, or DM_CHEATS_WINDOW=1 at launch) opens a Cheats panel beside the game: sections, search, the master switch,
// safe mode, values, details and the import of cheat files (see the code below).
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−, Space=shake the controller.
// Controllers: Nintendo-named pads use their labels (A right, B bottom, X top, Y left); other pads use positions as on the iPhone. GameController names
// its face buttons by the Xbox positions (A bottom, B right, X left, Y top), so a Nintendo pad needs A/B and X/Y exchanged to keep its printed labels.
// can force either mapping or swap A/B, and saves it next to the save directory. Shoulders, triggers, Menu / Options and both sticks are unchanged.
// Each controller is a player of its own (local co-op): the first one connected is player 1, together with the keyboard, the next player 2, and so on up to 8 (pad_slots.h);
// DM_PAD_MERGE=1 merges every controller into player 1 instead, DM_KBD_PLAYER=own makes the keyboard player 1 on its own, with the controllers from player 2 on.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "dm.h"
#include "dm_input.h"

// Use the Mac's real thermal/power state in the shared diagnostic summaries.
// The generic backend's weak defaults report "unknown" on macOS.
int dm_thermal_now(void) { return (int)NSProcessInfo.processInfo.thermalState; }
int dm_low_power_now(void) { return NSProcessInfo.processInfo.lowPowerModeEnabled; }
#include "front.h"
#include "hle.h"
#include "pad_slots.h"
#include "haptics_apple.h"
#include "motion_apple.h"
#include "motion_pro_mac.h"
#include "pad_apple.h"
#include "pad_hid_mac.h"
#import "front_settings.h"
#import "front_hud.h"
#import "pso_export.h"
#include "front_shortcuts.h"
#include "../gfx/nvn/aoc_timing.h"
#include "mac_display_settings.h"
#include <signal.h>
#include <math.h>
#include <sys/stat.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;
static int g_rl, g_ru, g_rr, g_rd;   // arrow keys pressed with Shift held: the right stick (each goes back up as it went down, whatever Shift does meanwhile)
static int g_input_on = 1;   // main thread: the game window is key, so its keyboard, mouse and controllers reach the game (input_release / input_resume)

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
static _Atomic int g_screen_fps;   // the window's screen's maximumFramesPerSecond, kept up to date when the window moves to another screen
int front_screen_max_fps(void) { return g_screen_fps; }
static _Atomic int g_screen_variable_refresh;
int front_screen_variable_refresh(void) { return g_screen_variable_refresh; }
static void screen_refresh(NSScreen *screen) {   // main thread: refresh capabilities of the current screen and mode, including a mode change without moving the window
    if (!screen) return;
    g_screen_fps = (int)screen.maximumFramesPerSecond;
    g_screen_variable_refresh = screen.minimumRefreshInterval > 0 && screen.maximumRefreshInterval > screen.minimumRefreshInterval;
}
static _Atomic int g_fullscreen;   // the window is in fullscreen (set when it has entered and when it has left)
int front_fullscreen(void) { return g_fullscreen; }   // nvn_mtl.m link_latency: frame latency 2 in variable-refresh fullscreen
// The screen's refresh rate matched to the game's frame rate in fullscreen (front_screen_match, from PR #79; DM_SCREEN_MATCH=0 turns it off). 60 fps on a 144 Hz screen cannot be paced
// evenly: the display link counts a tick on the refresh nearest each 1/60 s slot (nvn_mtl.m link_range), so frames stay up 2 and 3 refreshes in turn (13.9 / 20.8 ms) and Apple's HUD shows
// the frame interval jumping between them. In fullscreen, a screen whose rate is not a multiple of the game's switches to the mode of the same size with the highest rate that is (144 → 120 Hz
// for 60 and 30 fps), for this app only: macOS switches it back when the host quits, crashes included, and leaving fullscreen or that screen switches it back at once. A window keeps the
// screen's rate, because the switch changes the whole screen for every app and most external screens go black for a second or more. A screen with no such mode keeps the virtual ticks
static NSWindow *g_win; static _Atomic int g_match_want;   // the game's window; the frame rate to match (0 = not known yet), written by the present thread only
static CGDirectDisplayID g_match_id; static CGDisplayModeRef g_match_orig;   // main thread: the screen this host switched and its own mode (NULL = none switched)
static double mode_hz(CGDisplayModeRef m) { double hz = CGDisplayModeGetRefreshRate(m); return hz > 0 ? hz : 0; }   // 0 = not known (older built-in screens report none)
static int hz_multiple(double hz, int fps) { double k = round(hz / fps); return k >= 1 && fabs(hz / fps - k) < 0.01; }   // 59.94 Hz counts as 60
static CGError screen_set_mode(CGDirectDisplayID id, CGDisplayModeRef m) {   // for this app only (kCGConfigureForAppOnly)
    CGDisplayConfigRef cfg; CGError r = CGBeginDisplayConfiguration(&cfg);
    if (r != kCGErrorSuccess) return r;
    r = CGConfigureDisplayWithDisplayMode(cfg, id, m, NULL);
    if (r != kCGErrorSuccess) { CGCancelDisplayConfiguration(cfg); return r; }
    return CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
}
static void screen_match_restore(int on_it) {   // main thread: the screen this host switched gets its own mode again; on_it: the window is on that screen
    double hz = mode_hz(g_match_orig); CGError r = screen_set_mode(g_match_id, g_match_orig);
    LOG("front end: screen %s %.0f Hz", r == kCGErrorSuccess ? "back at" : "could not be switched back to", hz);
    if (r == kCGErrorSuccess && on_it && hz > 0) g_screen_fps = (int)round(hz);
    CGDisplayModeRelease(g_match_orig); g_match_orig = NULL;
}
static void screen_match_apply(void) {   // main thread: on a new frame rate, on entering and leaving fullscreen and on a move to another screen
    static int off = -1; if (off < 0) { const char *e = getenv("DM_SCREEN_MATCH"); off = e && !strcmp(e, "0"); if (off) LOG("front end: the screen keeps its refresh rate in fullscreen too (DM_SCREEN_MATCH=0)"); }
    NSScreen *scr = g_win.screen; int want = g_match_want, full = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    CGDirectDisplayID id = scr ? [scr.deviceDescription[@"NSScreenNumber"] unsignedIntValue] : kCGNullDirectDisplay;
    if (g_match_orig && (!full || id != g_match_id)) screen_match_restore(id == g_match_id);
    if (off || !full || want <= 0 || !scr) return;
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    if (!cur) return;
    double hz = mode_hz(cur);
    if (hz <= 0 || hz_multiple(hz, want)) {   // already a multiple, or not known
        if (g_match_orig && hz > 0) g_screen_fps = (int)round(hz);   // the screen this host switched: its rate from the mode, which maximumFramesPerSecond (the screen observer) may not show yet
        CGDisplayModeRelease(cur); return;
    }
    size_t w = CGDisplayModeGetWidth(cur), h = CGDisplayModeGetHeight(cur), pw = CGDisplayModeGetPixelWidth(cur), ph = CGDisplayModeGetPixelHeight(cur);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)@{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES});
    CGDisplayModeRef pick = NULL; double pick_hz = 0;
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i); double r = mode_hz(m);
        if (CGDisplayModeGetWidth(m) != w || CGDisplayModeGetHeight(m) != h || CGDisplayModeGetPixelWidth(m) != pw || CGDisplayModeGetPixelHeight(m) != ph || !CGDisplayModeIsUsableForDesktopGUI(m)) continue;
        if (hz_multiple(r, want) && r > pick_hz) { pick = m; pick_hz = r; }
    }
    if (!pick) LOG("front end: screen at %.0f Hz has no mode of this size at a multiple of %d Hz: frames are paced on the nearest refreshes", hz, want);
    else {
        CGError r = screen_set_mode(id, pick);
        LOG("front end: screen at %.0f Hz %s %.0f Hz for %d fps in fullscreen (DM_SCREEN_MATCH=0 keeps it)", hz, r == kCGErrorSuccess ? "switched to" : "could not be switched to", pick_hz, want);
        if (r == kCGErrorSuccess) { if (!g_match_orig) { g_match_orig = CGDisplayModeRetain(cur); g_match_id = id; } g_screen_fps = (int)round(pick_hz); }
    }
    if (modes) CFRelease(modes);
    CGDisplayModeRelease(cur);
}
void front_screen_match(int fps) {
    if (!g_layer || g_match_want == fps) return;   // no window (a windowless run's main thread runs the guest), or the rate as before
    g_match_want = fps; dispatch_async(dispatch_get_main_queue(), ^{ screen_match_apply(); });
}
// The pointer hides on game input while it is over the picture of the active game window (issue #675): AppKit shows it again when the mouse moves, input_release when
// the window loses focus. The title bar, other windows and other apps keep theirs; the mouse is the touch screen (DmGameView), and moving it shows the pointer
static void cursor_hide_for_input(void) {
    if (!g_win.isKeyWindow || !NSApp.isActive) return;
    NSView *v = g_win.contentView;
    if (NSPointInRect([v convertPoint:g_win.mouseLocationOutsideOfEventStream fromView:nil], v.bounds)) [NSCursor setHiddenUntilMouseMoves:YES];
}
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    int32_t rx = (g_rr - g_rl) * 32767, ry = (g_ru - g_rd) * 32767;
    dm_input_set(g_btn, lx, ly, rx, ry);
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return 10; case 48: return 11;                                          // Return Tab → + −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
static void on_key(NSEvent *e, int down) {
    if (!g_input_on) return;
    unsigned short c = e.keyCode;
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    // An arrow key pressed with Shift held is the right stick until it is released; without Shift, the D-pad. The keyboard had no right stick: a 3D game's camera
    // could be turned only with a controller (Donkey Kong Bananza)
    int *rs = c == 123 ? &g_rl : c == 126 ? &g_ru : c == 124 ? &g_rr : c == 125 ? &g_rd : NULL;
    if (rs && (down ? (e.modifierFlags & NSEventModifierFlagShift) != 0 : *rs)) { *rs = down; push(); return; }
    int b = key_bit(c);
    uint64_t m = b >= 0 ? 1ull << b : c == 49 ? DM_BTN_SHAKE : 0;   // Space: shaken while held
    g_btn = down ? (g_btn | m) : (g_btn & ~m);
    if (down && (m || c == 13 || c == 0 || c == 1 || c == 2)) cursor_hide_for_input();   // a key the game takes (WASD: the left stick)
    push();
}

// Physical controllers (GameController) go to dm_pad_set_player, mapped as the iPhone's (DmGamepad.m read_pad, same layout); player 1's is ORed with the keyboard, a stick takes
// whichever is deflected more (hle_hid.c). DM_KEYS uses player 1's path, so player 1's controller touched during a script takes it over.
// Players: every controller with an extended gamepad profile holds a slot (pad_slots.h: player 1 first, a reconnected controller gets its player back), and hle_hid.c reports
// player n on its own npad while the slot is held (dm_pad_players). A one-player game that supports only No1 / Handheld still sees every controller as player 1 (hle_hid.c).
// With DM_PAD_MERGE=1 every controller is player 1, as before (the last one moved wins; when the last one goes away its buttons and sticks are released)
static pad_slots g_slots;
static GCController *g_held[DM_PLAYERS];   // the controller of each slot, kept alive while it holds it (the slot table keys on its pointer)
static int g_merge, g_kbd_own;
enum { DM_FACE_AUTO, DM_FACE_POSITION, DM_FACE_LABEL };
static _Atomic int g_face_layout = DM_FACE_AUTO, g_swap_ab;
static int g_start_fullscreen, g_output_preset;
static DmSettingsPanel *g_settings_panel;
static DmCornerHUD *g_hud_view;
static DmHud g_hud; static DmHudImpose g_hud_imp;   // the corner box: the player's saved choice, and what DM_AOC_STATS / DM_AOC_FRAME_GRAPH impose this run (hud_settings.h)
static void settings_changed(DmMacSetting setting, NSInteger value);
static void settings_refresh(void);
static NSString *g_pad_settings_path;
static NSMenuItem *g_face_items[3], *g_swap_item, *g_fullscreen_item, *g_hints_item, *g_output_items[5];

static NSString *pad_settings_path(void) {
    const char *save = getenv("DM_SAVE");
    if (!save || !*save) return nil;
    return [[@(save) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"controller-layout.txt"];
}

static void pad_settings_save(void) {
    if (!g_pad_settings_path) return;
    static const char *layout[] = {"auto", "position", "label"};
    char hud[96]; dm_hud_format(g_hud, hud, sizeof hud);   // the player's own lines, never what the environment imposed
    NSString *text = [NSString stringWithFormat:@"layout=%s\nswap_ab=%d\nfullscreen=%d\noutput=%d\n%s",   // no fps_target: the game paces itself at 60
                                                layout[g_face_layout], !!g_swap_ab, !!g_start_fullscreen, g_output_preset, hud];
    NSError *error = nil;
    if (![text writeToFile:g_pad_settings_path atomically:YES encoding:NSUTF8StringEncoding error:&error])
        LOG("front end: could not save controller settings to %s: %s", g_pad_settings_path.fileSystemRepresentation,
            error.localizedDescription.UTF8String ?: "unknown error");
}

static void pad_settings_load(void) {
    g_pad_settings_path = pad_settings_path();
    g_face_layout = DM_FACE_AUTO;
    g_swap_ab = 0;
    g_start_fullscreen = 0; g_output_preset = 0;
    const char *e;
    int fps=aoc_timing_target(), dynamic=aoc_timing_dynamic();
    NSString *text = g_pad_settings_path ? [NSString stringWithContentsOfFile:g_pad_settings_path encoding:NSUTF8StringEncoding error:nil] : nil;
    for (NSString *raw in [text componentsSeparatedByString:@"\n"]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSArray<NSString *> *pair = [line componentsSeparatedByString:@"="];
        if (pair.count != 2) continue;
        NSString *key = [pair[0] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSString *value = [pair[1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([key isEqualToString:@"layout"]) {
            if ([value isEqualToString:@"position"]) g_face_layout = DM_FACE_POSITION;
            else if ([value isEqualToString:@"label"]) g_face_layout = DM_FACE_LABEL;
        } else if ([key isEqualToString:@"swap_ab"]) g_swap_ab = value.intValue != 0;
        else if ([key isEqualToString:@"fullscreen"]) g_start_fullscreen = value.intValue != 0;
        else if ([key isEqualToString:@"output"]) { NSInteger n = value.integerValue; if (n >= 0 && n < 5) g_output_preset = (int)n; }
    }
    g_hud = dm_hud_parse(text.UTF8String);   // hud= / hud_lines=, or an earlier build's hints / stats / frame_graph
    // Explicit diagnostics override saved preferences, including invalid-value fallback.
    if(getenv("DM_FPS_TARGET")) fps=aoc_timing_target();
    if(getenv("DM_DYNAMIC_FPS")) dynamic=aoc_timing_dynamic();
    g_hud_imp = dm_hud_impose(getenv("DM_AOC_STATS"), getenv("DM_AOC_FRAME_GRAPH"));
    if (g_hud_imp.invalid) LOG("[RED] DM_AOC_STATS=%s: must be 0, 1 or 2; FPS, shaders and timing hidden this run", getenv("DM_AOC_STATS"));
    aoc_stats_enable(dm_hud_needs_stats(dm_hud_effective(g_hud, g_hud_imp)));
    aoc_timing_set(fps,dynamic);
    e = getenv("DM_PAD_LAYOUT");
    if (e && !strcmp(e, "auto")) g_face_layout = DM_FACE_AUTO;
    else if (e && !strcmp(e, "position")) g_face_layout = DM_FACE_POSITION;
    else if (e && !strcmp(e, "label")) g_face_layout = DM_FACE_LABEL;
    e = getenv("DM_PAD_SWAP_AB");
    if (e) g_swap_ab = atoi(e) != 0;
    e = getenv("DM_FULLSCREEN");
    if (e) g_start_fullscreen = atoi(e) != 0;
    static const char *layout[] = {"auto", "position", "label"};
    DmHud eff = dm_hud_effective(g_hud, g_hud_imp); LOG("front end: controller face buttons %s%s%s; corner box %s, lines %d (saved: %s, lines %d), statistics %s", layout[g_face_layout], g_swap_ab ? ", A/B swapped" : "",
        g_pad_settings_path ? "" : " (no DM_SAVE: settings are not kept)", dm_hud_visible(eff) ? "shown" : "hidden", eff.lines, g_hud.on ? "on" : "off", g_hud.lines, dm_hud_needs_stats(eff) ? "collected" : "off");
}

static int pad_names_nintendo(NSString *text) {
    if (!text) return 0;
    NSString *lower = text.lowercaseString;
    for (NSString *word in @[@"nintendo", @"switch", @"joy-con", @"joycon", @"pro controller"])
        if ([lower containsString:word]) return 1;
    return 0;
}

static int pad_face_swap_abxy(GCController *controller) {
    int nintendo = pad_names_nintendo(controller.vendorName) || pad_names_nintendo(controller.productCategory);
    if (g_face_layout == DM_FACE_LABEL) return 1;
    if (g_face_layout == DM_FACE_POSITION) return 0;   // GameController's native face-button positions are the Xbox / PlayStation layout
    return nintendo;
}

static uint64_t pad_face_buttons(GCExtendedGamepad *g, int swap_abxy) {
    uint64_t m = swap_abxy
        ? (g.buttonA.isPressed ? DM_BTN_B : 0) | (g.buttonB.isPressed ? DM_BTN_A : 0) |
          (g.buttonX.isPressed ? DM_BTN_Y : 0) | (g.buttonY.isPressed ? DM_BTN_X : 0)  // Nintendo printed labels (A right, B bottom, X top, Y left)
        : (g.buttonA.isPressed ? DM_BTN_A : 0) | (g.buttonB.isPressed ? DM_BTN_B : 0) |
          (g.buttonX.isPressed ? DM_BTN_X : 0) | (g.buttonY.isPressed ? DM_BTN_Y : 0); // Xbox / PlayStation printed positions
    if (g_swap_ab) {
        uint64_t a = m & DM_BTN_A, b = m & DM_BTN_B;
        m &= ~(DM_BTN_A | DM_BTN_B);
        m |= (a ? DM_BTN_B : 0) | (b ? DM_BTN_A : 0);
    }
    return m;
}

static void pad_read(GCExtendedGamepad *g);
static void pad_apply_settings(void) {
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad) pad_read(controller.extendedGamepad);
    for (int i = 0; i < 3; i++) if (g_face_items[i]) g_face_items[i].state = g_face_layout == i;
    if (g_swap_item) g_swap_item.state = g_swap_ab;
    if (g_fullscreen_item) g_fullscreen_item.state = g_start_fullscreen;
}

static int32_t pad_axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * DM_STICK_MAX); }
// A controller's state for player p, GameController or raw HID: a pressed button or a stick past 15% hides the pointer (cursor_hide_for_input), a resting stick's drift does not
static void pad_set(int p, uint64_t m, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    const int32_t dead = DM_STICK_MAX * 15 / 100;
    if (m || abs(lx) > dead || abs(ly) > dead || abs(rx) > dead || abs(ry) > dead) cursor_hide_for_input();
    dm_pad_set_player(p, m, lx, ly, rx, ry);
}
static void pad_read(GCExtendedGamepad *g) {
    if (!g_input_on) return;
    int p = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)g.controller); if (p < 0) return;
    dm_haptics_apple_controller(p, g.controller);
    uint64_t m = pad_face_buttons(g, pad_face_swap_abxy(g.controller));
    #define K(btn, bit) if (btn.isPressed) m |= bit
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R); K(g.leftTrigger, DM_BTN_ZL); K(g.rightTrigger, DM_BTN_ZR);
    K(g.buttonMenu, DM_BTN_PLUS); K(g.buttonOptions, DM_BTN_MINUS); K(g.leftThumbstickButton, DM_BTN_LS); K(g.rightThumbstickButton, DM_BTN_RS);   // nil on pads without them: not pressed
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
    #undef K
    pad_set(p, m, pad_axis(g.leftThumbstick.xAxis.value), pad_axis(g.leftThumbstick.yAxis.value), pad_axis(g.rightThumbstick.xAxis.value), pad_axis(g.rightThumbstick.yAxis.value));
}
// Home and Capture: without this macOS takes them as system gestures (Home opens the Games app); the host gives them no function
static void pad_keep_system_buttons(GCController *c) { for (NSString *b in @[GCInputButtonHome, GCInputButtonShare]) c.physicalInputProfile.buttons[b].preferredSystemGestureState = GCSystemGestureStateDisabled; }
static const char *pad_name(GCController *c) { return c.vendorName.UTF8String ?: "?"; }
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }
static void pro_players_changed(GCController *gone) {
    int count = 0, player = -1;
    for (GCController *c in GCController.controllers) if (c != gone && c.extendedGamepad && [c.productCategory isEqualToString:@"Switch Pro Controller"]) {
        count++; player = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)c);
    }
    dm_motion_pro_players(count, player);
}
static void pro_devices_changed(void) { pro_players_changed(nil); }
// DM_PAD_MERGE: whether a controller still plays player 1 (gone: one just removed, not counted), a GameController one or a raw HID pad (pad_hid_mac.h)
static int merge_pads_left(GCController *gone) {
    if (g_hidpads) return 1;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) return 1;
    return 0;
}
// Called at start and on every connect / disconnect (gone: the controller that was just removed, nil on a connect)
static void pads_changed(GCController *gone) {
    if (g_merge) {
        int any = 0;
        for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) {
            any = 1; pad_keep_system_buttons(c); c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
            dm_motion_attach(c, 0);   // every controller is player 1: the last one moved drives its six-axis sensor
            pad_read(c.extendedGamepad);
        }
        if (gone) dm_motion_detach(gone, 0);
        if (!any) { if (!g_hidpads) dm_pad_set(0, 0, 0, 0, 0); dm_haptics_apple_controller(0, nil); }   // a raw HID pad still plays player 1 (pad_hid_mac.h)
        pro_players_changed(gone);
        return;
    }
    NSArray<GCController *> *now = GCController.controllers; char nb[16];
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p] && (g_held[p] == gone || ![now containsObject:g_held[p]])) {
        npad_name(p, nb, sizeof nb); LOG("controller '%s' disconnected: player %d (npad %s) is free", pad_name(g_held[p]), p + 1, nb);
        g_held[p].extendedGamepad.valueChangedHandler = nil;
        dm_motion_detach(g_held[p], p);
        dm_haptics_apple_controller(p, nil);
        pad_slot_free(&g_slots, (__bridge const void *)g_held[p]); g_held[p] = nil;
        dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
    }
    for (GCController *c in now) if (c.extendedGamepad && c != gone && pad_slot_of(&g_slots, (__bridge const void *)c) < 0) {
        int p = pad_slot_take(&g_slots, (__bridge const void *)c, pad_name(c));
        if (p < 0) { LOG("controller '%s' connected: all %d players are taken, not used", pad_name(c), DM_PLAYERS); continue; }
        g_held[p] = c; pad_keep_system_buttons(c);
        c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
        dm_motion_attach(c, p);
        npad_name(p, nb, sizeof nb); LOG("controller '%s' connected: player %d (npad %s), face buttons by %s", pad_name(c), p + 1, nb,
                                         pad_face_swap_abxy(c) ? "Nintendo labels" : "Xbox / PlayStation positions");
        pad_read(c.extendedGamepad);
    }
    dm_pad_players(pad_slot_mask(&g_slots));
    pro_players_changed(gone);
}
// Display sleep: macOS counts keyboard and mouse input as user activity, not a controller's, so in controller-only play the display dims and locks; the latency
// activity in front_run keeps the system awake, not the display. While the game window is key the display stays awake (NSActivityIdleDisplaySleepDisabled);
// losing focus or being minimized lets it sleep again. An explicit lock or sleep is not held off, and no input is synthesized
static id g_display_activity;
static void display_awake(int on) {
    if (on && !g_display_activity)
        g_display_activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityIdleDisplaySleepDisabled reason:@"NxNative active game window"];
    else if (!on && g_display_activity) { [NSProcessInfo.processInfo endActivity:g_display_activity]; g_display_activity = nil; }
}
// Focus: a window that is no longer key sees no key-up, mouse-up or controller release, so whatever is held when it loses focus (another app, the text box, minimized) would
// stay pressed in the game. The front end releases only what it pressed itself: scripted buttons and sticks have their own path, and the touch is also DM_KEYS / DM_CTL's (hle_hid.c).
// Out of focus a controller's changes are ignored, so back in focus each controller is read as it is now (a button still held, a stick still deflected)
static int g_mouse_down;   // the mouse holds the touch (DmGameView)
static void input_release(void) {
    [NSCursor setHiddenUntilMouseMoves:NO];
    display_awake(0);
    g_input_on = 0;
    dm_motion_pro_active(0);
    g_btn = 0; g_w = g_a = g_s = g_d = 0; g_rl = g_ru = g_rr = g_rd = 0; push();
    if (g_mouse_down) { g_mouse_down = 0; dm_touch_set(NULL, 0); }
    if (g_merge) { if (merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0); return; }
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p]) dm_pad_set_player(p, 0, 0, 0, 0, 0);
    for (hid_pad *d = g_hidpads; d; d = d->next) { int p = pad_slot_of(&g_slots, d); if (p >= 0) dm_pad_set_player(p, 0, 0, 0, 0, 0); }
}
static void input_resume(void) {
    display_awake(1);
    g_input_on = 1;
    dm_motion_pro_active(1);
    if (g_merge) { for (GCController *c in GCController.controllers) if (c.extendedGamepad) pad_read(c.extendedGamepad); }
    else for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p].extendedGamepad) pad_read(g_held[p].extendedGamepad);
    for (hid_pad *d = g_hidpads; d; d = d->next) hidpad_read(d);
}
// Raw HID gamepads (pad_hid_mac.h): a player slot each, as a GameController controller, named by the device's product name
static void hidpad_read(hid_pad *d) {
    if (!g_input_on) return;
    int p = g_merge ? 0 : pad_slot_of(&g_slots, d); if (p < 0) return;
    int32_t s[4]; pad_hid_sticks(&d->st, s);
    pad_set(p, pad_hid_buttons(&d->st), s[0], s[1], s[2], s[3]);
}
static void hidpad_attach(hid_pad *d) {
    int p = g_merge ? 0 : pad_slot_take(&g_slots, d, d->name); char nb[16];
    if (p < 0) { LOG("controller '%s' (HID) connected: all %d players are taken, not used", d->name, DM_PLAYERS); return; }
    npad_name(p, nb, sizeof nb); LOG("controller '%s' (HID) connected: player %d (npad %s)", d->name, p + 1, nb);
    if (!g_merge) dm_pad_players(pad_slot_mask(&g_slots));   // merged, every controller is player 1, as pads_changed has it
    hidpad_read(d);
}
static void hidpad_detach(hid_pad *d) {   // d is already off g_hidpads
    if (g_merge) {   // as pads_changed: released when the last controller goes away
        LOG("controller '%s' (HID) disconnected", d->name);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d); char nb[16]; if (p < 0) return;
    npad_name(p, nb, sizeof nb); LOG("controller '%s' (HID) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
    dm_pad_players(pad_slot_mask(&g_slots));
}
static void pads_start(void) {
    const char *e = getenv("DM_PAD_MERGE"); g_merge = e && atoi(e);
    e = getenv("DM_KBD_PLAYER"); g_kbd_own = !g_merge && e && !strcmp(e, "own");
    static const char kbd = 0;
    if (g_kbd_own) pad_slot_take(&g_slots, &kbd, "keyboard");   // holds player 1 for good
    LOG("front end: %s", g_merge ? "every controller is player 1 (DM_PAD_MERGE)" : g_kbd_own ? "the keyboard is player 1, each controller a player of its own from player 2 on (DM_KBD_PLAYER=own)"
                                                                           : "each controller is a player of its own, the keyboard plays with player 1");
    pads_changed(nil);
    dm_motion_pro_start(pro_devices_changed);
    hidpad_start();
}

// Mouse as a single-point touchscreen (front.h dm_touch_set): left button down / drag = finger 0, release = lifted. The content view takes the clicks,
// not a window-wide event monitor, so clicks on the title bar, the window buttons and the resize edges still reach AppKit.
@interface DmGameView : NSView @end
@implementation DmGameView
- (void)mouseDown:(NSEvent *)e { [self mouseDragged:e]; }
- (void)mouseDragged:(NSEvent *)e {
    if (!g_input_on) return;
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil]; NSSize b = self.bounds.size;
    double w = b.width, h = b.height;
    if (w <= 0 || h <= 0) return;
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;   // the centered 16:9 picture the present fits into the view, as on the iPhone (DmGamepad.m)
    double x0 = (b.width - w) / 2, y0 = (b.height - h) / 2;
    dm_touch t = {0, (float)fmin(fmax((p.x - x0) / w, 0), 0.999), (float)fmin(fmax(1 - (p.y - y0) / h, 0), 0.999)};   // The AppKit view origin is at bottom left, the touchscreen origin is at top left
    dm_touch_set(&t, 1); g_mouse_down = 1;
}
- (void)mouseUp:(NSEvent *)e { (void)e; if (g_mouse_down) { g_mouse_down = 0; dm_touch_set(NULL, 0); } }
// The drawable follows the view in pixels: a layer assigned to a layer-backed view kept the drawable at the first window's 1280x720 in fullscreen too, so the game picture was
// scaled down to it and up again by the compositor
- (void)fitDrawable {
    CAMetalLayer *l = (CAMetalLayer *)self.layer; if (![l isKindOfClass:CAMetalLayer.class] || !self.window) return;
    l.contentsScale = self.window.backingScaleFactor;
    NSSize px = [self convertSizeToBacking:self.bounds.size]; if (px.width < 1 || px.height < 1) return;
    DmOutputSize size = dm_output_size((uint32_t)px.width, (uint32_t)px.height, g_output_preset);
    NSSize output = NSMakeSize(size.width, size.height);
    if (!CGSizeEqualToSize(l.drawableSize, output)) { l.drawableSize = output; LOG("front end: drawable %.0fx%.0f (output cap %d)", output.width, output.height, g_output_preset); }
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self fitDrawable]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self fitDrawable]; }
@end

// AppKit reports a refused fullscreen request only to the window delegate (there is no notification for it)
static void (^g_fs_failed)(void);
@interface DmWinDelegate : NSObject <NSWindowDelegate> @end
@implementation DmWinDelegate
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { (void)w; if (g_fs_failed) g_fs_failed(); }
@end

@interface DmSettingsMenu : NSObject
- (void)chooseFaceLayout:(NSMenuItem *)item;
- (void)toggleSwapAB:(NSMenuItem *)item;
- (void)resetController:(id)sender;
- (void)toggleStartFullscreen:(NSMenuItem *)item;
- (void)openSettings:(id)sender;
- (void)chooseOutput:(NSMenuItem *)item;
- (void)toggleHints:(id)sender;
@end

@implementation DmSettingsMenu
- (void)chooseFaceLayout:(NSMenuItem *)item { settings_changed(DmSettingFaceLayout, item.tag); }
- (void)toggleSwapAB:(NSMenuItem *)item { (void)item; settings_changed(DmSettingSwapAB, !g_swap_ab); }
- (void)resetController:(id)sender { (void)sender; settings_changed(DmSettingResetController, 0); }
- (void)toggleStartFullscreen:(NSMenuItem *)item { (void)item; settings_changed(DmSettingStartFullscreen, !g_start_fullscreen); }
- (void)openSettings:(id)sender { (void)sender; [g_settings_panel showPanel]; }
- (void)chooseOutput:(NSMenuItem *)item { settings_changed(DmSettingOutput, item.tag); }
- (void)toggleHints:(id)sender { (void)sender; settings_changed(DmSettingHudKey, 0); }
@end

static DmSettingsMenu *g_settings_menu_target;
static void install_controller_menu(NSMenu *bar) {
    g_settings_menu_target = [DmSettingsMenu new];
    NSMenu *controller = [[NSMenu alloc] initWithTitle:@"Controller"];
    const char *titles[] = {"Automatic (recommended)", "By position (Xbox / PlayStation)", "By label (Nintendo)"};
    for (int i = 0; i < 3; i++) {
        NSMenuItem *item = [controller addItemWithTitle:@(titles[i]) action:@selector(chooseFaceLayout:) keyEquivalent:@""];
        item.target = g_settings_menu_target;
        item.tag = i;
        g_face_items[i] = item;
    }
    [controller addItem:NSMenuItem.separatorItem];
    g_swap_item = [controller addItemWithTitle:@"Swap A / B" action:@selector(toggleSwapAB:) keyEquivalent:@""];
    g_swap_item.target = g_settings_menu_target;
    [controller addItemWithTitle:@"Reset controller layout" action:@selector(resetController:) keyEquivalent:@""].target = g_settings_menu_target;
    [[bar addItemWithTitle:@"Controller" action:nil keyEquivalent:@""] setSubmenu:controller];

    NSMenu *view = [[NSMenu alloc] initWithTitle:@"Display"];
    g_fullscreen_item = [view addItemWithTitle:@"Start in full screen" action:@selector(toggleStartFullscreen:) keyEquivalent:@""];
    g_fullscreen_item.target = g_settings_menu_target;
    NSMenu *outputs = [[NSMenu alloc] initWithTitle:@"Output size cap"];
    NSArray<NSString *> *names = @[@"Automatic (window size)", @"720p", @"1080p", @"1440p", @"2160p"];
    for (int i = 0; i < 5; i++) {
        NSMenuItem *item = [outputs addItemWithTitle:names[i] action:@selector(chooseOutput:) keyEquivalent:@""];
        item.tag = i; item.target = g_settings_menu_target; g_output_items[i] = item;
    }
    [[view addItemWithTitle:outputs.title action:nil keyEquivalent:@""] setSubmenu:outputs];
    g_hints_item = [view addItemWithTitle:@"Corner box" action:@selector(toggleHints:) keyEquivalent:@""];
    g_hints_item.target = g_settings_menu_target;
    [[bar addItemWithTitle:@"Display" action:nil keyEquivalent:@""] setSubmenu:view];
    pad_apply_settings();
}

static DmMacSettings settings_snapshot(void) {
    CGSize size = g_layer.drawableSize;
    NSUInteger connected = 0; for (GCController *pad in GCController.controllers) if (pad.extendedGamepad) connected++;
    return (DmMacSettings){.face_layout = g_face_layout, .swap_ab = g_swap_ab, .start_fullscreen = g_start_fullscreen,
        .output_preset = g_output_preset, .fullscreen = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0,
        .fps_target=aoc_timing_target(), .dynamic_fps=aoc_timing_dynamic(), .screen_fps=front_screen_max_fps(),
        .hud_on = dm_hud_effective(g_hud, g_hud_imp).on, .hud_lines = dm_hud_effective(g_hud, g_hud_imp).lines,
        .performance=aoc_stats_snapshot(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)),
        .output_width = (NSUInteger)size.width, .output_height = (NSUInteger)size.height, .controllers = connected};
}
static void settings_refresh(void) {
    pad_apply_settings();
    for (int i = 0; i < 5; i++) g_output_items[i].state = g_output_preset == i;
    g_hints_item.state = dm_hud_visible(dm_hud_effective(g_hud, g_hud_imp));
    [g_settings_panel refresh]; [g_hud_view refresh];
}
static void settings_changed(DmMacSetting setting, NSInteger value) {
    switch (setting) {
        case DmSettingFpsTarget: aoc_timing_set((int)value,aoc_timing_dynamic()); break;
        case DmSettingDynamicFps: aoc_timing_set(aoc_timing_target(),value!=0); break;
        case DmSettingHudOn: dm_hud_set_on(&g_hud, &g_hud_imp, value != 0); break;
        case DmSettingHudKey: dm_hud_key(&g_hud, &g_hud_imp, (int)value & HUD_LINES_ALL); break;
        case DmSettingHudLine: dm_hud_set_line(&g_hud, &g_hud_imp, (int)(value >> 1) & HUD_LINES_ALL, (int)(value & 1)); break;
        case DmSettingFaceLayout: if (value >= 0 && value < 3) g_face_layout = (int)value; break;
        case DmSettingSwapAB: g_swap_ab = value != 0; break;
        case DmSettingStartFullscreen: g_start_fullscreen = value != 0; break;
        case DmSettingOutput:
            if (value >= 0 && value < 5) g_output_preset = (int)value;
            [(DmGameView *)g_win.contentView fitDrawable]; break;
        case DmSettingFullscreen:
            if (((g_win.styleMask & NSWindowStyleMaskFullScreen) != 0) != (value != 0)) [g_win toggleFullScreen:nil];
            break;
        case DmSettingResetController: g_face_layout = DM_FACE_AUTO; g_swap_ab = 0; break;
    }
    aoc_stats_enable(dm_hud_needs_stats(dm_hud_effective(g_hud, g_hud_imp)));   // collected while a line that needs it shows
    pad_settings_save(); settings_refresh();
    LOG("front end: settings layout=%d swap_ab=%d output=%d hud=%d lines=%d start_fullscreen=%d", g_face_layout, g_swap_ab, g_output_preset, g_hud.on, g_hud.lines, g_start_fullscreen);
}

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); dm_host_shutdown("guest thread finished");
    return NULL;
}

// Metal reads MTL_HUD_ENABLED when the process makes its first device, which can be before the window: set from DM_METAL_HUD before main (set in front_run, the HUD did not show)
__attribute__((constructor)) static void hud_env(void) { front_hud_env(); }

// ---------- Cheats: the menu and the panel (hle_cheats.c holds every state; these only show it and call it) ----------

// A string of the engine as an NSString that is never nil: cheat names, sections and notes come from the player's files,
// which may be Shift-JIS or Latin-1, and a UTF-8 name cut at the engine's buffer size may end in half a character. UTF-8
// first, then UTF-8 without up to three trailing bytes, then Shift-JIS, then Windows-1252 (which decodes any byte)
static NSString *engine_str(const char *s) {
    if (!s) return @"";
    size_t n = strlen(s);
    for (size_t cut = 0; cut <= 3 && cut <= n; cut++) {
        NSString *u = [[NSString alloc] initWithBytes:s length:n - cut encoding:NSUTF8StringEncoding];
        if (u) return u;
    }
    return [[NSString alloc] initWithBytes:s length:n encoding:NSShiftJISStringEncoding]
        ?: [[NSString alloc] initWithBytes:s length:n encoding:NSWindowsCP1252StringEncoding] ?: @"";
}

// A path quoted for the shell (single quotes; a quote inside becomes '\'')
static NSString *shell_quote(NSString *p) { return [NSString stringWithFormat:@"'%@'", [p stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]]; }

// The name a row shows: the on half of an on / off pair loses its " - on" (the pair is one switch, its off half has no row)
static NSString *cheat_display_name(int i) {
    NSString *n = engine_str(dm_cheat_name_at(i));
    if (!(dm_cheat_flags_at(i) & DM_CHEAT_PAIR_ON)) return n;
    static NSRegularExpression *re;
    if (!re) re = [NSRegularExpression regularExpressionWithPattern:@"(\\s*[-:_]\\s*|\\s+|\\s*[(\\[]\\s*)on\\s*[)\\]]?\\s*$" options:NSRegularExpressionCaseInsensitive error:nil];
    NSString *base = [re stringByReplacingMatchesInString:n options:0 range:NSMakeRange(0, n.length) withTemplate:@""];
    return base.length ? base : n;
}

// Does entry i get a row of its own? Not the off half of a pair (its on half is the switch) and not a section marker
// (the section shows as a header above its entries)
static BOOL cheat_has_row(int i) {
    int f = dm_cheat_flags_at(i);
    return !(f & (DM_CHEAT_PAIR_OFF | DM_CHEAT_SECTION_START | DM_CHEAT_SECTION_END));
}

// The row's whole title, the same string the menu's row carries: the name, plus the run-time reason when there is one,
// "— master code (always on)" for a master code, "(nxnative only)" for a cheat this host runs itself (never-decrease)
static NSString *cheat_row_title(int i) {
    const char *why = dm_cheat_why_at(i);
    int kind = dm_cheat_kind_at(i), f = dm_cheat_flags_at(i);
    NSString *n = cheat_display_name(i);
    if (kind == 1) return [n stringByAppendingString:@" — master code (always on)"];
    if (why && *why) return [n stringByAppendingString:engine_str(why)];
    if (f & DM_CHEAT_HOST) return [n stringByAppendingString:@" (nxnative only)"];
    return n;
}

// The row's tooltip: the title, the meta file's note (how scripts/cheat_anchor.py compiled it), what a host-only or a
// one-pass cheat does
static NSString *cheat_row_tip(int i) {
    NSMutableString *t = [cheat_row_title(i) mutableCopy];
    int f = dm_cheat_flags_at(i);
    const char *note = dm_cheat_note_at(i);
    if (note && *note) [t appendFormat:@"\n%@", engine_str(note)];
    if (f & DM_CHEAT_HOST) [t appendString:@"\nRun by nxnative itself: it keeps the highest value seen since it was turned on; other engines see an empty entry."];
    if (f & DM_CHEAT_ONCE) [t appendString:@"\nApplied once: it writes one pass, then turns itself off."];
    if (f & DM_CHEAT_PAIR_ON) [t appendString:@"\nSwitching it off puts the game's original code back."];
    if (f & DM_CHEAT_WEAK) [t appendString:@"\nResolved weakly for this build: check its effect."];
    return t;
}

// The row's state as hle_cheats.c has it: a plain cheat carries its own on state, a master code is always on, and a
// disabled row (unsupported opcode, malformed, or killed at run time) or a label is off, its checkbox not clickable
static NSControlStateValue cheat_row_state(int i) {
    int kind = dm_cheat_kind_at(i);
    return kind == 1 || (kind == 0 && dm_cheat_on_at(i)) ? NSControlStateValueOn : NSControlStateValueOff;
}

// A build's name for a player: "1.0.3 (build E1D8B399B787AE41)", or the build ID alone when its version is unknown
static NSString *build_label(const char *bid, const char *ver) {
    return ver && *ver ? [NSString stringWithFormat:@"%@ (build %@)", engine_str(ver), engine_str(bid)] : [NSString stringWithFormat:@"build %@", engine_str(bid)];
}

// "Open cheats folder": the menu's and the panel's action, one helper — create the folder when missing, then reveal it in Finder
static void cheats_open_folder(void) {
    const char *dir = dm_cheats_dir_path();
    if (dir && *dir) { mkdir(dir, 0755); [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:@(dir)]]; }
}

// The Cheats panel (the menu's "Show Cheats Window", ⌘⇧C, or DM_CHEATS_WINDOW=1 at launch): a utility NSPanel that stays
// open beside the game window, toggles applied live, scrollable. It never takes the key window from the game for a click
// (becomesKeyOnlyIfNeeded, shown with orderFront), so the game keeps the keyboard and the controllers; it becomes key only
// while the player types in its search field or a value field, and Return or Escape there hands the keyboard back.
// Created once lazily and kept for the process's lifetime, closed only to hide (its place and size survive in the frame
// autosave "DmCheatsPanel"). Every row comes from the C API (dm_cheat* in dm.h); the class holds no cheats state of its own
// beyond the controls. Top to bottom:
//   - the header (game build and version, counts) and, when it applies, a line about safe mode with Restore;
//   - Cheats On (the master switch), Select All / Deselect All and the search field;
//   - the rows: section headers, labels, one row per cheat (an on / off pair is one row; a "set" cheat has an Apply button
//     instead of a checkbox; an "ask" cheat a value field; ⓘ shows its details), then the cheats the compiler left out,
//     the other builds' files, and the anchored sources not compiled for this build;
//   - a details line (a cheat's dry run and note, or the load report);
//   - Reload, Open folder, Find Cheats… (in Ryujinx, the yuzu family and SD cards) and Report.
// A .txt or a .cheats.toml dropped on the panel is copied into the cheats folder (import below; the drop works from a
// background Finder window, see DmCheatsDropView). The rows are rebuilt
// when what they were built from changes (a reload, the search text), updated in place otherwise; a ~1 s timer runs only
// while the panel is visible
@interface DmCheatsPanel : NSObject <NSWindowDelegate, NSTextFieldDelegate, NSSearchFieldDelegate>
- (void)show;      // create the panel if needed, order it front (not key) and refresh
- (void)refresh;   // the one refresh for every trigger: shown, a click, Reload, a Cheats-menu toggle, the 1 s timer
- (BOOL)importFiles:(NSArray<NSURL *> *)urls;   // copy dropped / found cheat files into the cheats folder, reload
@end
static DmCheatsPanel *g_cheats_panel;   // created on the first Show Cheats Window, kept for the process's lifetime

// The panel class: a panel may become key (its search and value fields need the keyboard); becomesKeyOnlyIfNeeded keeps a
// checkbox click from making it key
@interface DmCheatsNSPanel : NSPanel @end
@implementation DmCheatsNSPanel
- (BOOL)canBecomeKeyWindow { return YES; }
@end

// The panel's content view: it takes dropped files (a .txt or .cheats.toml) for the import. The utility panel hides while
// another app is in front, so a drop works from a Finder window dragged without activating Finder first (a background
// window), not after clicking into Finder; Find Cheats… covers the usual places without a drag
@interface DmCheatsDropView : NSView @end
@implementation DmCheatsDropView
// A drag enters: accept it as a copy when it carries a cheat file
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
    NSArray *urls = [info.draggingPasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    for (NSURL *u in urls) {
        NSString *e = u.lastPathComponent.lowercaseString;
        if ([e hasSuffix:@".txt"] || [e hasSuffix:@".cheats.toml"]) return NSDragOperationCopy;
    }
    return NSDragOperationNone;
}
// The drop: the import runs a main-queue turn later, so its questions (modal alerts) do not hold Finder's drag session open
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSArray *urls = [info.draggingPasteboard readObjectsForClasses:@[NSURL.class] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (!urls.count || !g_cheats_panel) return NO;
    dispatch_async(dispatch_get_main_queue(), ^{ [g_cheats_panel importFiles:urls]; });
    return YES;
}
@end

// The scroll view's document view: flipped, so the stack view's rows start at the top and the scroll position counts
// downward like the rows (unflipped, everything would sit at the bottom of the document view)
@interface DmCheatsDocView : NSView @end
@implementation DmCheatsDocView
- (BOOL)isFlipped { return YES; }
@end

// The Cheats menu of the game window (hle_cheats.c's rows, only meaningful with a window): rebuilt on every open, so the
// loaded file's state, the menu's toggles and the cheats the VM disabled at run time are what the player sees. Rows: Show
// Cheats Window (⌘⇧C), Cheats On (the master switch) and, in safe mode, Restore Saved Cheats; then the panel's rows in
// short (section headers, labels, one row per cheat, a pair as one row, "set" cheats applied on click), the other builds'
// files when none of this build is loaded, and Reload cheats (⌘R) / Open cheats folder. A click here also refreshes the
// panel when one exists, so the two never disagree
@interface DmCheatsMenuDelegate : NSObject <NSMenuDelegate>
@end
@implementation DmCheatsMenuDelegate
// Rebuild the menu's rows from the engine (every open, and once at launch so its key equivalents exist)
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSMenuItem *sh = [menu addItemWithTitle:@"Show Cheats Window" action:@selector(showPanel:) keyEquivalent:@"c"];
    sh.target = self;
    sh.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;   // ⌘⇧C (the plain "c" already maps ZR)
    NSMenuItem *ms = [menu addItemWithTitle:@"Cheats On" action:@selector(toggleMaster:) keyEquivalent:@""];
    ms.target = self; ms.state = dm_cheats_master() ? NSControlStateValueOn : NSControlStateValueOff;
    if (dm_cheats_safe()) [menu addItemWithTitle:@"Restore Saved Cheats (safe mode)" action:@selector(restoreSafe:) keyEquivalent:@""].target = self;
    [menu addItem:NSMenuItem.separatorItem];
    const char *sec = "";
    for (int i = 0, n = dm_cheats_ncheats(); i < n; i++) {
        if (!cheat_has_row(i)) continue;
        const char *s = dm_cheat_section_at(i);
        if (strcmp(s, sec)) {   // a section header before its first entry
            sec = s;
            if (*s) [menu addItemWithTitle:[NSString stringWithFormat:@"— %@ —", engine_str(s)] action:nil keyEquivalent:@""].enabled = NO;
        }
        int kind = dm_cheat_kind_at(i), f = dm_cheat_flags_at(i);
        NSString *title = cheat_row_title(i);
        if (f & DM_CHEAT_ONCE) title = [title stringByAppendingString:@" (apply once)"];
        NSMenuItem *it = [menu addItemWithTitle:title action:nil keyEquivalent:@""];
        if (kind == 0) { it.target = self; it.action = @selector(toggleCheat:); it.tag = i; }
        it.enabled = kind == 0;
        it.state = cheat_row_state(i);
        it.toolTip = cheat_row_tip(i);
    }
    for (int i = 0, n = dm_cheats_nothers(); i < n; i++)   // .txt files of other builds, lightly disabled (the brief's wording)
        [menu addItemWithTitle:[NSString stringWithFormat:@"Cheats for %@ (another version of the game)", build_label(dm_cheats_other(i), dm_cheats_other_version(i))] action:nil keyEquivalent:@""].enabled = NO;
    if (!dm_cheats_found())
        [menu addItemWithTitle:[NSString stringWithFormat:@"No cheats for %@", build_label(dm_cheats_build(), dm_cheats_version())] action:nil keyEquivalent:@""].enabled = NO;
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *re = [menu addItemWithTitle:@"Reload cheats" action:@selector(reloadCheats:) keyEquivalent:@"r"];
    re.target = self;
    const char *dir = dm_cheats_dir_path();
    NSMenuItem *op = [menu addItemWithTitle:@"Open cheats folder" action:dir && *dir ? @selector(openCheatsFolder:) : nil keyEquivalent:@""];
    op.target = self;
    op.enabled = dir && *dir != 0;
}
// A cheat row: toggle it (a "set" cheat runs one pass)
- (void)toggleCheat:(NSMenuItem *)it {
    dm_cheat_toggle((int)it.tag);
    it.state = dm_cheat_on_at((int)it.tag) ? NSControlStateValueOn : NSControlStateValueOff;   // The new state right now; a run-time [RED] kill shows its reason when the menu next opens
    if (g_cheats_panel) [g_cheats_panel refresh];   // the panel (if created) follows the menu so the two never disagree; its visibility is checked there
}
// Cheats On: the master switch
- (void)toggleMaster:(NSMenuItem *)it { (void)it; dm_cheats_master_set(!dm_cheats_master()); if (g_cheats_panel) [g_cheats_panel refresh]; }
// Restore Saved Cheats: leave safe mode
- (void)restoreSafe:(NSMenuItem *)it { (void)it; dm_cheats_safe_restore(); if (g_cheats_panel) [g_cheats_panel refresh]; }
// Reload cheats (⌘R): re-read the file in place
- (void)reloadCheats:(NSMenuItem *)it { (void)it; dm_cheats_reload(); if (g_cheats_panel) [g_cheats_panel refresh]; }
// Open cheats folder: create it when missing, reveal it in Finder
- (void)openCheatsFolder:(NSMenuItem *)it { (void)it; cheats_open_folder(); }
// Lazily create the panel on the first Show Cheats Window (it is kept for the process's lifetime) and order it front:
// orderFront keeps the game the key window
- (void)showPanel:(NSMenuItem *)it {
    (void)it;
    if (!g_cheats_panel) g_cheats_panel = [DmCheatsPanel new];   // created here, on the first Show Cheats Window
    [g_cheats_panel show];
}
@end
static DmCheatsMenuDelegate *g_cheats_menu;   // NSMenu keeps its delegate unretained

@implementation DmCheatsPanel {
    NSPanel *_panel;              // created once in show, kept for the process's lifetime
    NSTextField *_header;         // the one-line status line above the rows
    NSStackView *_safeRow;        // safe mode's line and Restore, hidden otherwise
    NSButton *_master;            // Cheats On: the master switch
    NSButton *_selectAll;         // enabled while some plain cheat is off
    NSButton *_deselectAll;       // enabled while some plain cheat is on
    NSSearchField *_search;       // filters the rows by name and section
    NSScrollView *_scroll;        // scrolls the rows
    NSStackView *_stack;          // the rows (vertical, leading alignment), inside _doc
    NSView *_doc;                 // the flipped document view the stack is pinned to
    NSTextField *_details;        // a cheat's dry run and note, or the load report
    NSButton *_openFolder;        // disabled while dm_cheats_dir_path() is empty
    NSMutableDictionary<NSNumber *, NSButton *> *_checks;   // cheat index -> its checkbox (or Apply button), to update in place
    NSMutableDictionary<NSNumber *, NSTextField *> *_values; // cheat index -> its value field
    NSMutableDictionary<NSNumber *, NSTextField *> *_labels; // cheat index -> the name label of a "set" cheat's row
    BOOL _rebuilding;             // the rows are being replaced: a field's end of editing then applies nothing
    NSString *_rows_sig;          // what the rows on screen were built from: a change rebuilds
    int _detailsFor;              // the cheat whose details are shown (-1: the report, -2: nothing)
    NSTimer *_refreshTimer;       // ~1 s, scheduled only while the panel is on screen; invalidated when it hides
    BOOL _upOnDeactivate;         // the panel was on screen when the app was deactivated: AppKit brings it back on activation
    BOOL _finding;                // a Find Cheats… scan is running on a background queue: another click does not start a second one
}

// A button of the panel's toolbar rows
static NSButton *panel_button(NSString *title, id target, SEL action) { return [NSButton buttonWithTitle:title target:target action:action]; }

// A horizontal row of views, leading-aligned
static NSStackView *panel_hrow(NSArray<NSView *> *views) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation = NSUserInterfaceLayoutOrientationHorizontal; s.spacing = 8; s.translatesAutoresizingMaskIntoConstraints = NO;
    return s;
}

// A secondary-colour label that truncates instead of widening the panel, its whole text in the tooltip
static NSTextField *panel_note(NSString *text) {
    NSTextField *l = [NSTextField labelWithString:text];
    l.textColor = NSColor.secondaryLabelColor; l.lineBreakMode = NSLineBreakByTruncatingTail; l.toolTip = text;
    [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    return l;
}

// Create the panel, the content view and the fixed layout once (header, safe-mode line, toolbar, scrolled rows, details,
// bottom buttons); refresh fills the rows afterwards
- (void)makePanel {
    _panel = [[DmCheatsNSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 480, 560)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                        | NSWindowStyleMaskUtilityWindow
                                                  backing:NSBackingStoreBuffered defer:NO];
    _panel.title = @"Cheats";
    _panel.releasedWhenClosed = NO;         // the close button only hides (windowShouldClose)
    _panel.floatingPanel = YES;             // above the game window
    _panel.becomesKeyOnlyIfNeeded = YES;    // a checkbox click never takes the key window: the game keeps keyboard and controllers
    _panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;   // shows over the fullscreen game too
    _panel.frameAutosaveName = @"DmCheatsPanel";   // place and size remembered (set after the frame is read back: makePanel's end)
    _panel.minSize = NSMakeSize(360, 260);
    _panel.delegate = self;
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:self selector:@selector(appResignedActive:) name:NSApplicationDidResignActiveNotification object:NSApp];
    [nc addObserver:self selector:@selector(appBecameActive:) name:NSApplicationDidBecomeActiveNotification object:NSApp];
    DmCheatsDropView *content = [[DmCheatsDropView alloc] initWithFrame:NSMakeRect(0, 0, 480, 560)];
    [content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    _panel.contentView = content;

    _header = [NSTextField labelWithString:@""];
    _header.translatesAutoresizingMaskIntoConstraints = NO;
    _header.lineBreakMode = NSLineBreakByTruncatingTail;
    [_header setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSTextField *safeText = panel_note(@"Last run ended unexpectedly with cheats on: all are off.");
    safeText.textColor = NSColor.systemOrangeColor;
    _safeRow = panel_hrow(@[safeText, panel_button(@"Restore", self, @selector(restoreSafe:))]);
    _master = [NSButton checkboxWithTitle:@"Cheats On" target:self action:@selector(toggleMaster:)];
    _selectAll = panel_button(@"Select All", self, @selector(selectAllCheats:));
    _deselectAll = panel_button(@"Deselect All", self, @selector(deselectAllCheats:));
    _search = [NSSearchField new];
    _search.placeholderString = @"Search";
    _search.delegate = self;
    _search.sendsSearchStringImmediately = YES;
    [_search setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *tools = panel_hrow(@[_master, _selectAll, _deselectAll, _search]);

    _scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scroll.hasVerticalScroller = YES; _scroll.autohidesScrollers = YES;
    _scroll.translatesAutoresizingMaskIntoConstraints = NO;
    _doc = [[DmCheatsDocView alloc] initWithFrame:NSZeroRect];
    _doc.translatesAutoresizingMaskIntoConstraints = NO;
    _scroll.documentView = _doc;
    _stack = [[NSStackView alloc] initWithFrame:NSZeroRect];
    _stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _stack.alignment = NSLayoutAttributeLeading;
    _stack.spacing = 4;
    _stack.edgeInsets = NSEdgeInsetsMake(8, 8, 8, 8);
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_doc addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[
        [_stack.topAnchor constraintEqualToAnchor:_doc.topAnchor],
        [_stack.leadingAnchor constraintEqualToAnchor:_doc.leadingAnchor],
        [_stack.trailingAnchor constraintEqualToAnchor:_doc.trailingAnchor],
        [_doc.leadingAnchor constraintEqualToAnchor:_scroll.contentView.leadingAnchor],
        [_doc.widthAnchor constraintEqualToAnchor:_scroll.contentView.widthAnchor],     // no horizontal scrolling
        [_doc.topAnchor constraintEqualToAnchor:_scroll.contentView.topAnchor],
        [_doc.bottomAnchor constraintEqualToAnchor:_stack.bottomAnchor],
    ]];

    _details = [NSTextField wrappingLabelWithString:@""];
    _details.translatesAutoresizingMaskIntoConstraints = NO;
    _details.selectable = NO;   // a selectable label would make the panel key on a click and keep the keyboard from the game
    _details.font = [NSFont monospacedSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular];
    _details.maximumNumberOfLines = 8;
    [_details setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    _detailsFor = -2;

    _openFolder = panel_button(@"Open Folder", self, @selector(openFolder:));
    NSStackView *bottom = panel_hrow(@[panel_button(@"Reload", self, @selector(reload:)), _openFolder,
                                       panel_button(@"Find Cheats…", self, @selector(findCheats:)), panel_button(@"Report", self, @selector(showReport:))]);
    // the header, the safe-mode line and the toolbar in one column: a stack view detaches a hidden view, so the safe-mode
    // line takes no room while it is hidden
    NSStackView *top = [NSStackView stackViewWithViews:@[_header, _safeRow, tools]];
    top.orientation = NSUserInterfaceLayoutOrientationVertical; top.alignment = NSLayoutAttributeLeading; top.spacing = 6;
    top.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView *v in @[top, _scroll, _details, bottom]) [content addSubview:v];
    [NSLayoutConstraint activateConstraints:@[
        [top.topAnchor constraintEqualToAnchor:content.topAnchor constant:10],
        [top.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
        [top.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-8],
        [_header.trailingAnchor constraintEqualToAnchor:top.trailingAnchor],
        [tools.trailingAnchor constraintEqualToAnchor:top.trailingAnchor],
        [_scroll.topAnchor constraintEqualToAnchor:top.bottomAnchor constant:8],
        [_scroll.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
        [_scroll.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
        [_scroll.bottomAnchor constraintEqualToAnchor:_details.topAnchor constant:-6],
        [_details.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
        [_details.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-8],
        [_details.bottomAnchor constraintEqualToAnchor:bottom.topAnchor constant:-6],
        [bottom.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:8],
        [bottom.trailingAnchor constraintLessThanOrEqualToAnchor:content.trailingAnchor constant:-8],
        [bottom.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-8],
    ]];
    _checks = [NSMutableDictionary dictionary];
    _values = [NSMutableDictionary dictionary];
    _labels = [NSMutableDictionary dictionary];
    _rows_sig = @"";
    if (![_panel setFrameUsingName:@"DmCheatsPanel"] && g_win) {   // first use: beside the game window, not the screen's corner
        NSRect g = g_win.frame, f = _panel.frame;
        [_panel setFrameOrigin:NSMakePoint(NSMaxX(g) + 8, NSMaxY(g) - f.size.height)];
    }
}

// The panel became key: keep it only while one of its fields is being edited (a click elsewhere, Full Keyboard Access
// on a button, the search field's clear button); otherwise the keyboard goes back to the game a main-queue turn later,
// once the click has settled the first responder
- (void)windowDidBecomeKey:(NSNotification *)n {
    (void)n;
    dispatch_async(dispatch_get_main_queue(), ^{ if (![self editing]) [self giveKeyBack]; });
}

// Is the search field or a value field being edited (its field editor is the panel's first responder)?
- (BOOL)editing {
    if (_search.currentEditor) return YES;
    for (NSTextField *v in _values.allValues) if (v.currentEditor) return YES;
    return NO;
}

// Give the keyboard back to the game: after typing in the search or a value field (Return, Escape, the end of editing)
- (void)giveKeyBack {
    if (g_win && _panel.isKeyWindow) [g_win makeKeyWindow];
}

// ---- the actions ----

// Open Folder: create the cheats folder when missing, reveal it in Finder
- (void)openFolder:(NSButton *)b { (void)b; cheats_open_folder(); }
// Reload: re-read the file in place (edits during play need no re-launch)
- (void)reload:(NSButton *)b { (void)b; dm_cheats_reload(); [self refresh]; }
// Cheats On: the master switch (off stops every cheat without touching the toggles)
- (void)toggleMaster:(NSButton *)b { (void)b; dm_cheats_master_set(!dm_cheats_master()); [self refresh]; }
// Restore (safe mode): the saved toggles and the masters come back
- (void)restoreSafe:(NSButton *)b { (void)b; dm_cheats_safe_restore(); [self refresh]; }
// Report: the load report in the details line
- (void)showReport:(NSButton *)b { (void)b; _detailsFor = -1; [self fillDetails]; }

// Select All / Deselect All: flip every plain cheat (kind 0) that is not already in the wanted state, then one refresh.
// The loop over dm_cheat_toggle instead of a bulk call keeps the C API small; each toggle rewrites <build ID>.enabled once
// (a few tiny writes), and a pass between two toggles sees a mix for one frame, which is harmless. Masters, disabled
// cheats, labels and "set" cheats (Select All must not fire them) are never touched, nor the off half of a pair
// (hle_cheats.c switches it with its on half)
- (void)setPlainCheatsOn:(BOOL)on {
    for (int i = 0, n = dm_cheats_ncheats(); i < n; i++) {
        if (dm_cheat_kind_at(i) != 0 || (dm_cheat_flags_at(i) & (DM_CHEAT_PAIR_OFF | DM_CHEAT_ONCE))) continue;
        if (!!dm_cheat_on_at(i) == on) continue;
        dm_cheat_toggle(i);
    }
    [self refresh];
}
// The two buttons behind setPlainCheatsOn:
- (void)selectAllCheats:(NSButton *)b   { (void)b; [self setPlainCheatsOn:YES]; }
- (void)deselectAllCheats:(NSButton *)b { (void)b; [self setPlainCheatsOn:NO]; }

// A checkbox click (or an Apply button: a "set" cheat runs one pass and turns itself off): flip the cheat (hle_cheats.c
// also rewrites <build ID>.enabled), then refresh — the state comes back from the model
- (void)cheatClicked:(NSButton *)b {
    int i = (int)b.tag;
    BOOL apply = i >= 0 && i < dm_cheats_ncheats() && (dm_cheat_flags_at(i) & DM_CHEAT_ONCE);
    if (apply && dm_cheat_on_at(i)) return;   // Apply only applies: one already waiting for its pass is not cancelled
    if (i >= 0 && i < dm_cheats_ncheats()) dm_cheat_toggle(i);
    if (_detailsFor == i) [self fillDetails];
    [self refresh];
}

// ⓘ: show the cheat's details below the rows (its dry run now, its note)
- (void)infoClicked:(NSButton *)b { _detailsFor = (int)b.tag; [self fillDetails]; }

// A field's edit ended. A value field (still the row's own: a rebuild ends edits without applying them) sets its value;
// a refused one beeps and shows the current value again. The keyboard goes back to the game only for Return (or the
// search field's Escape), not when the player moved on to another field of the panel (Tab, a click)
- (void)controlTextDidEndEditing:(NSNotification *)n {
    NSTextField *f = n.object;
    NSInteger how = [n.userInfo[@"NSTextMovement"] integerValue];
    BOOL done = how == NSTextMovementReturn || how == NSTextMovementCancel;
    if (f != (id)_search && !_rebuilding && _values[@(f.tag)] == f) {
        int i = (int)f.tag;
        if (dm_cheat_value_set(i, f.stringValue.UTF8String)) {
            NSBeep();
            char v[64]; if (dm_cheat_value_get(i, v, sizeof v)) f.stringValue = @(v);
        }
        if (_detailsFor == i) [self fillDetails];
    }
    if (done) [self giveKeyBack];
}

// The search text changed: the rows are filtered again (the signature includes it, so refresh rebuilds)
- (void)controlTextDidChange:(NSNotification *)n { if (n.object == _search) [self refresh]; }

// Escape: in a value field it abandons the typed text (the current value comes back, nothing is applied); in the search
// field it clears the search as usual. Either way the keyboard goes back to the game (Return ends editing,
// controlTextDidEndEditing)
- (BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel {
    (void)tv;
    if (sel != @selector(cancelOperation:)) return NO;
    if (c == (id)_search) { dispatch_async(dispatch_get_main_queue(), ^{ [self giveKeyBack]; }); return NO; }
    NSTextField *f = (NSTextField *)c;
    [f abortEditing];
    char v[64]; if (dm_cheat_value_get((int)f.tag, v, sizeof v)) f.stringValue = @(v);
    [self giveKeyBack];
    return YES;
}

// ---- Find Cheats… and the import ----

// The cheat files of this game the usual places hold, this build's own first:
//   - Ryujinx: mods/contents/<title ID>/cheats/ and its mods' own cheats folders (mods/contents/<title ID>/<mod>/cheats/);
//   - the yuzu family: load/<title ID>/<mod>/cheats/;
//   - the Atmosphère tree of a mounted local volume (an SD card); network volumes are not listed (a stall even off the main thread).
// Only the .txt files named after a build ID count (not Ryujinx's enabled.txt); each folder is read once whatever case
// the path came in. build: this build's ID (read from the engine on the main thread by the caller). Runs on a background
// queue (findCheats:): it touches only the file system and Foundation, never AppKit or the cheats engine
static NSArray<NSURL *> *found_cheat_files(NSString *build) {
    NSString *tid = [NSString stringWithFormat:@"%016llX", (unsigned long long)DM_GAME_PROGRAM_ID];
    NSString *as = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
    NSMutableArray<NSString *> *dirs = [NSMutableArray array];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *t in @[tid.lowercaseString, tid]) {
        NSString *mods = [as stringByAppendingFormat:@"/Ryujinx/mods/contents/%@", t];
        [dirs addObject:[mods stringByAppendingString:@"/cheats"]];
        for (NSString *m in [fm contentsOfDirectoryAtPath:mods error:nil] ?: @[]) [dirs addObject:[mods stringByAppendingFormat:@"/%@/cheats", m]];
    }
    for (NSString *emu in @[@"yuzu", @"citron", @"eden", @"sudachi", @"suyu"]) {
        NSString *load = [as stringByAppendingFormat:@"/%@/load/%@", emu, tid];
        for (NSString *m in [fm contentsOfDirectoryAtPath:load error:nil] ?: @[]) [dirs addObject:[load stringByAppendingFormat:@"/%@/cheats", m]];
    }
    for (NSURL *vol in [fm mountedVolumeURLsIncludingResourceValuesForKeys:@[NSURLVolumeIsLocalKey] options:NSVolumeEnumerationSkipHiddenVolumes] ?: @[]) {
        NSNumber *local = nil; [vol getResourceValue:&local forKey:NSURLVolumeIsLocalKey error:nil];
        NSString *root = vol.path;   // nil for a volume without a file-system path: nothing to look in
        if (local.boolValue && root) [dirs addObject:[root stringByAppendingFormat:@"/atmosphere/contents/%@/cheats", tid]];
    }
    NSMutableArray<NSURL *> *mine = [NSMutableArray array], *rest = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    NSCharacterSet *nothex = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"].invertedSet;
    for (NSString *d in dirs) {
        NSString *real = [d stringByResolvingSymlinksInPath].lowercaseString;   // one folder however its path was spelled
        if ([seen containsObject:real]) continue;
        [seen addObject:real];
        for (NSString *f in [fm contentsOfDirectoryAtPath:d error:nil] ?: @[]) {
            if (f.length != 20 || ![f.lowercaseString hasSuffix:@".txt"] || [[f substringToIndex:16] rangeOfCharacterFromSet:nothex].location != NSNotFound) continue;
            NSURL *u = [NSURL fileURLWithPath:[d stringByAppendingPathComponent:f]];
            [([f.uppercaseString hasPrefix:build] ? mine : rest) addObject:u];
        }
    }
    [mine addObjectsFromArray:rest];
    return mine;
}

// Find Cheats…: a menu of the files found, under the button; picking one imports it. The folders are read on a background
// queue, since a sleeping external disk can take seconds to answer and the main thread would beachball the game window
// meanwhile; the button reads "Looking…" and is disabled until the scan ends, and _finding keeps a second click (or an
// action sent while disabled) from starting another scan. The menu is built and shown on the main queue, and only while
// the panel is still on screen (it hides when the app is deactivated): a menu popping up out of nowhere would be worse
- (void)findCheats:(NSButton *)b {
    if (_finding) return;
    _finding = YES;
    NSString *title = b.title;
    b.title = @"Looking…"; b.enabled = NO;
    NSString *build = engine_str(dm_cheats_build());   // the engine's state is read here, on the main thread, not by the scan
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSArray<NSURL *> *found = found_cheat_files(build);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_finding = NO;
            b.title = title; b.enabled = YES;
            if (b.window.visible) [self showFound:found under:b];
        });
    });
}
// The files a Find Cheats… scan found, as a menu under its button (main thread)
- (void)showFound:(NSArray<NSURL *> *)found under:(NSButton *)b {
    NSMenu *m = [NSMenu new];
    if (!found.count) [m addItemWithTitle:@"No cheat files of this game in Ryujinx, the yuzu family or an SD card" action:nil keyEquivalent:@""].enabled = NO;
    for (NSURL *u in found) {
        NSString *parent = u.URLByDeletingLastPathComponent.path.stringByAbbreviatingWithTildeInPath;
        NSMenuItem *it = [m addItemWithTitle:[NSString stringWithFormat:@"%@ — %@", u.lastPathComponent, parent] action:@selector(importFound:) keyEquivalent:@""];
        it.target = self; it.representedObject = u;
    }
    [m popUpMenuPositioningItem:nil atLocation:NSMakePoint(0, b.bounds.size.height) inView:b];
}
// A file picked in Find Cheats…: import it
- (void)importFound:(NSMenuItem *)it { if (it.representedObject) [self importFiles:@[it.representedObject]]; }

// A yes / no question as a modal alert, answered before returning (the import is the player's own action, so a modal
// question is fine; the keyboard goes back to the game after it): YES for the first button
- (BOOL)ask:(NSString *)text info:(NSString *)info yes:(NSString *)yes {
    NSAlert *a = [NSAlert new];
    a.messageText = text; a.informativeText = info ?: @"";
    [a addButtonWithTitle:yes]; [a addButtonWithTitle:@"Cancel"];
    BOOL r = [a runModal] == NSAlertFirstButtonReturn;
    [self giveKeyBack];
    return r;
}

// Copy cheat files into the cheats folder, then reload: a .cheats.toml (an anchored source) keeps its name; a .txt keeps a
// build ID name, and a file named otherwise (a Ryujinx list such as "all cheats.txt") becomes this build's after the
// player confirms it is for this build. A file already there is kept as <name>.bak (asked first). Returns whether
// something was imported
- (BOOL)importFiles:(NSArray<NSURL *> *)urls {
    const char *dirc = dm_cheats_dir_path();
    if (!dirc || !*dirc || strlen(dm_cheats_build()) != 16) { NSBeep(); return NO; }   // no folder, or no build ID to name a file by
    NSString *dir = engine_str(dirc), *build = engine_str(dm_cheats_build());
    NSString *tid = [NSString stringWithFormat:@"%016llX", (unsigned long long)DM_GAME_PROGRAM_ID];
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    BOOL any = NO;
    for (NSURL *u in urls) {
        NSString *name = u.lastPathComponent, *dest;
        if ([name.lowercaseString hasSuffix:@".cheats.toml"]) dest = name;
        else if (![name.lowercaseString hasSuffix:@".txt"]) continue;
        else {
            NSString *stem = name.length >= 16 ? [name substringToIndex:16].uppercaseString : @"";
            BOOL hex = stem.length == 16 && [stem rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789ABCDEF"].invertedSet].location == NSNotFound;
            if (hex && ![stem isEqualToString:tid]) dest = [stem stringByAppendingString:@".txt"];   // a title ID name says no build
            else {
                if (![self ask:[NSString stringWithFormat:@"Is “%@” for this version of the game?", name]
                          info:[NSString stringWithFormat:@"Its name does not say which build it is for. It will be used as the cheats of %@.", build_label(dm_cheats_build(), dm_cheats_version())]
                           yes:@"Use for This Build"]) continue;
                dest = [build stringByAppendingString:@".txt"];
            }
        }
        NSString *path = [dir stringByAppendingPathComponent:dest];
        if ([u.path.stringByStandardizingPath.lowercaseString isEqualToString:path.stringByStandardizingPath.lowercaseString]) continue;   // already the folder's own file
        if ([fm fileExistsAtPath:path]) {
            if (![self ask:[NSString stringWithFormat:@"Replace %@?", dest] info:[NSString stringWithFormat:@"The current one is kept as %@.bak (an older %@.bak is replaced).", dest, dest] yes:@"Replace"]) continue;
            NSString *bak = [path stringByAppendingString:@".bak"];
            [fm removeItemAtPath:bak error:nil];
            [fm moveItemAtPath:path toPath:bak error:nil];
        }
        NSError *e = nil;
        if ([fm copyItemAtURL:u toURL:[NSURL fileURLWithPath:path] error:&e]) any = YES;
        else LOG("[RED] cheats: cannot copy %s into the cheats folder: %s", u.path.UTF8String, e.localizedDescription.UTF8String);
    }
    if (any) { dm_cheats_reload(); _detailsFor = -1; [self refresh]; [self fillDetails]; }
    return any;
}

// ---- the rows ----

// A row of one cheat: its checkbox (or, for a "set" cheat, its name and an Apply button), its value field for an "ask"
// cheat, and ⓘ. The title truncates and is whole in the tooltip, so a long one never widens the panel
- (NSView *)cheatRow:(int)i {
    int kind = dm_cheat_kind_at(i), f = dm_cheat_flags_at(i);
    NSString *title = cheat_row_title(i), *tip = cheat_row_tip(i);
    NSMutableArray<NSView *> *views = [NSMutableArray array];
    NSButton *b;
    if (f & DM_CHEAT_ONCE) {
        NSTextField *l = [NSTextField labelWithString:title];
        l.lineBreakMode = NSLineBreakByTruncatingTail; l.toolTip = tip;
        _labels[@(i)] = l;
        [l setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        [views addObject:l];
        b = panel_button(@"Apply", self, @selector(cheatClicked:));
        b.controlSize = NSControlSizeSmall;
    } else {
        b = [NSButton checkboxWithTitle:title target:self action:@selector(cheatClicked:)];
        ((NSButtonCell *)b.cell).lineBreakMode = NSLineBreakByTruncatingTail;
        [b setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
        b.state = cheat_row_state(i);
    }
    b.tag = i; b.enabled = kind == 0; b.toolTip = tip;
    _checks[@(i)] = b;
    if (!(f & DM_CHEAT_ONCE)) [views addObject:b];
    if (f & DM_CHEAT_ASK) {
        NSTextField *v = [NSTextField textFieldWithString:@""];
        char cur[64]; if (dm_cheat_value_get(i, cur, sizeof cur)) v.stringValue = @(cur);
        v.placeholderString = @(dm_cheat_value_bounds(i));
        v.toolTip = [NSString stringWithFormat:@"Value (%s, %s)", dm_cheat_type_at(i), dm_cheat_value_bounds(i)];
        v.tag = i; v.delegate = self; v.controlSize = NSControlSizeSmall;
        [v.widthAnchor constraintEqualToConstant:90].active = YES;
        _values[@(i)] = v;
        [views addObject:v];
    }
    if (f & DM_CHEAT_ONCE) [views addObject:b];
    NSImage *icon = [NSImage imageWithSystemSymbolName:@"info.circle" accessibilityDescription:@"Details"];   // nil only if the symbol is gone
    NSButton *info = icon ? [NSButton buttonWithImage:icon target:self action:@selector(infoClicked:)] : [NSButton buttonWithTitle:@"i" target:self action:@selector(infoClicked:)];
    info.bordered = NO; info.tag = i; info.toolTip = @"Show what it writes now, and its note";
    [views addObject:info];
    return panel_hrow(views);
}

// A section header: the section's name in bold
- (NSView *)sectionRow:(const char *)name {
    NSTextField *l = [NSTextField labelWithString:engine_str(name)];
    l.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return l;
}

// Does entry i pass the search text (its name or its section contains it, any case)?
- (BOOL)matches:(int)i {
    NSString *q = _search.stringValue;
    if (!q.length) return YES;
    return [cheat_display_name(i) rangeOfString:q options:NSCaseInsensitiveSearch].location != NSNotFound
        || [engine_str(dm_cheat_section_at(i)) rangeOfString:q options:NSCaseInsensitiveSearch].location != NSNotFound;
}

// What the rows on screen were built from: the signature rebuilt on every refresh, the rows rebuilt when it differs (the
// list, the flags that choose a row's controls, the search text). States and titles are not in it: they change per cheat
// and the update path carries them in place
- (NSString *)rowsSig {
    NSMutableString *s = [NSMutableString stringWithFormat:@"%d/%d/%@/", dm_cheats_ncheats(), dm_cheats_found(), _search.stringValue];
    for (int i = 0, n = dm_cheats_ncheats(); i < n; i++) [s appendFormat:@"%@:%d|", engine_str(dm_cheat_name_at(i)), dm_cheat_flags_at(i)];
    for (int i = 0, n = dm_cheats_nothers(); i < n; i++) [s appendFormat:@"%@:%@|", engine_str(dm_cheats_other(i)), engine_str(dm_cheats_other_version(i))];
    for (int i = 0, n = dm_cheats_nskipped(); i < n; i++) [s appendFormat:@"%@|", engine_str(dm_cheats_skipped_name(i))];
    for (int i = 0, n = dm_cheats_nsources(); i < n; i++) [s appendFormat:@"%@|", engine_str(dm_cheats_source(i))];
    return s;
}

// Rebuild all rows (a reload, a first fill, a new search), keeping the scroll position as far as possible
- (void)rebuildRows {
    NSClipView *clip = _scroll.contentView;
    CGFloat keepY = clip.bounds.origin.y;
    _rebuilding = YES;   // an edit in progress ends now without being applied (its row may be another cheat after a reload)
    for (NSTextField *v in _values.allValues) if (v.currentEditor) [v abortEditing];
    for (NSView *v in [_stack.arrangedSubviews copy]) [v removeFromSuperview];
    _rebuilding = NO;
    [_checks removeAllObjects]; [_values removeAllObjects]; [_labels removeAllObjects];
    const char *sec = "";
    for (int i = 0, n = dm_cheats_ncheats(); i < n; i++) {
        if (!cheat_has_row(i) || ![self matches:i]) continue;
        const char *s = dm_cheat_section_at(i);
        if (strcmp(s, sec)) { sec = s; if (*s) [_stack addArrangedSubview:[self sectionRow:s]]; }
        if (dm_cheat_kind_at(i) == 3) [_stack addArrangedSubview:panel_note(cheat_display_name(i))];   // a label: credits and the like
        else [_stack addArrangedSubview:[self cheatRow:i]];
    }
    for (int i = 0, n = dm_cheats_nskipped(); i < n; i++)
        [_stack addArrangedSubview:panel_note([NSString stringWithFormat:@"Left out of this build: %@ — %@", engine_str(dm_cheats_skipped_name(i)), engine_str(dm_cheats_skipped_why(i))])];
    for (int i = 0, n = dm_cheats_nothers(); i < n; i++)
        [_stack addArrangedSubview:panel_note([NSString stringWithFormat:@"Cheats for %@: another version of the game, its addresses do not fit this one",
                                                build_label(dm_cheats_other(i), dm_cheats_other_version(i))])];
    if (!dm_cheats_found())
        for (int i = 0, n = dm_cheats_nsources(); i < n; i++) {
            NSButton *copy = panel_button(@"Copy Command", self, @selector(copyCompile:)); copy.tag = i; copy.controlSize = NSControlSizeSmall;
            [_stack addArrangedSubview:panel_hrow(@[panel_note([NSString stringWithFormat:@"%@: anchored cheats, not compiled for this build yet", engine_str(dm_cheats_source(i))]), copy])];
        }
    _rows_sig = [self rowsSig];
    if (keepY <= 0) return;   // at the top already: the new rows start at the top too
    __weak DmCheatsPanel *wself = self;   // one main-queue tick later: the panel may be hidden by then
    dispatch_async(dispatch_get_main_queue(), ^{
        DmCheatsPanel *s = wself;
        if (!s || !s->_panel || !s->_panel.isVisible) return;
        NSClipView *c = s->_scroll.contentView;
        CGFloat max = fmax(0, s->_doc.frame.size.height - c.bounds.size.height);
        [c setBoundsOrigin:NSMakePoint(0, fmin(keepY, max))];
    });
}

// Copy Command: the command that compiles an anchored source for this build (scripts/cheat_anchor.py, run from the
// nxnative repository's folder; the build's ExeFS/main is the one the NSP script extracted), paths quoted for the shell
- (void)copyCompile:(NSButton *)b {
    NSString *dir = engine_str(dm_cheats_dir_path());
    NSString *cmd = [NSString stringWithFormat:@"python3 scripts/cheat_anchor.py compile %@ --nso '<the ExeFS/main of build %@>' -o %@",
                     shell_quote([dir stringByAppendingPathComponent:engine_str(dm_cheats_source((int)b.tag))]), engine_str(dm_cheats_build()), shell_quote(dir)];
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:cmd forType:NSPasteboardTypeString];
    _detailsFor = -2;   // the timer leaves this text alone
    _details.stringValue = [@"Copied (run it in the nxnative folder): " stringByAppendingString:cmd];
}

// Update the rows in place (a click, a timer tick, a menu toggle): each control's state, enabled flag and title; a value
// field the player is editing is left alone
- (void)updateRows {
    [_checks enumerateKeysAndObjectsUsingBlock:^(NSNumber *k, NSButton *b, BOOL *stop) {
        (void)stop; int i = k.intValue;
        BOOL once = (dm_cheat_flags_at(i) & DM_CHEAT_ONCE) != 0;
        b.enabled = dm_cheat_kind_at(i) == 0 && (!once || (dm_cheats_master() && !dm_cheat_on_at(i)));   // Apply: not while one waits
        if (once) {
            NSTextField *l = self->_labels[k];
            NSString *full = cheat_row_title(i);
            if (l && ![l.stringValue isEqualToString:full]) { l.stringValue = full; l.toolTip = cheat_row_tip(i); }
        } else {
            b.state = cheat_row_state(i);
            NSString *full = cheat_row_title(i);
            if (![b.title isEqualToString:full]) { b.title = full; b.toolTip = cheat_row_tip(i); }
        }
    }];
    [_values enumerateKeysAndObjectsUsingBlock:^(NSNumber *k, NSTextField *v, BOOL *stop) {
        (void)stop;
        if (v.currentEditor) return;
        char cur[64]; if (dm_cheat_value_get(k.intValue, cur, sizeof cur) && ![v.stringValue isEqualToString:@(cur)]) v.stringValue = @(cur);
    }];
}

// The details line: the shown cheat's dry run and note, or the load report
- (void)fillDetails {
    if (_detailsFor == -1) { _details.stringValue = engine_str(dm_cheats_report()); return; }
    if (_detailsFor < 0 || _detailsFor >= dm_cheats_ncheats()) { _details.stringValue = @""; return; }
    char pv[1024]; dm_cheat_preview(_detailsFor, pv, sizeof pv);
    NSMutableString *t = [NSMutableString stringWithFormat:@"%@\n%@", cheat_display_name(_detailsFor), engine_str(pv)];
    const char *note = dm_cheat_note_at(_detailsFor);
    if (note && *note) [t appendString:engine_str(note)];
    _details.stringValue = [t stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

// The Select All / Deselect All buttons' enable state: Select All while some switchable plain cheat is off, Deselect All
// while one is on (the same cheats setPlainCheatsOn: flips); both off while Cheats On is off
- (void)updateSelectButtons {
    BOOL someOff = NO, someOn = NO;
    for (int i = 0, n = dm_cheats_ncheats(); i < n; i++) {
        if (dm_cheat_kind_at(i) != 0 || (dm_cheat_flags_at(i) & (DM_CHEAT_PAIR_OFF | DM_CHEAT_ONCE))) continue;
        if (dm_cheat_on_at(i)) someOn = YES; else someOff = YES;
        if (someOn && someOff) break;
    }
    _selectAll.enabled = someOff && dm_cheats_master();
    _deselectAll.enabled = someOn && dm_cheats_master();
}

// The header's one line (the folder missing, the counts and the build's version, or no file of this build) and the
// master switch and safe-mode controls
- (void)fillHeader {
    const char *dir = dm_cheats_dir_path();
    int n = 0, on = 0, masters = 0;   // switchable cheats and those on; master codes are counted apart (always on)
    for (int i = 0, c = dm_cheats_ncheats(); i < c; i++) {
        int kind = dm_cheat_kind_at(i);
        if (!cheat_has_row(i) || kind == 3) continue;
        if (kind == 1) { masters++; continue; }
        n++; on += kind == 0 && dm_cheat_on_at(i);
    }
    NSString *build = build_label(dm_cheats_build(), dm_cheats_version());
    if (!dir || !*dir) _header.stringValue = @"Cheats are off: no cheats folder (DM_CHEATS or DM_ROOT is not set)";
    else if (dm_cheats_found()) _header.stringValue = [NSString stringWithFormat:@"%@: %d cheats, %d on%@", build, n, on,
                                                        masters ? [NSString stringWithFormat:@", %d master", masters] : @""];
    else _header.stringValue = [NSString stringWithFormat:@"No cheats for %@", build];
    _header.toolTip = engine_str(dm_cheats_report());
    _master.state = dm_cheats_master() ? NSControlStateValueOn : NSControlStateValueOff;
    _safeRow.hidden = !dm_cheats_safe();
}

// The one refresh: the header and the buttons always, the rows rebuilt when what they show changed, only updated when it
// did not. Nothing happens while the panel is not on screen — it fills itself in show
- (void)refresh {
    if (!_panel || !_panel.isVisible) return;
    [self fillHeader];
    [self updateSelectButtons];
    _openFolder.enabled = ({ const char *d = dm_cheats_dir_path(); d && *d; });
    if (![_rows_sig isEqualToString:[self rowsSig]]) [self rebuildRows];
    else [self updateRows];
}

// Arm the ~1 s refresh unless it is already running: called by show and by the app's return to the foreground
- (void)startTimer {
    if (_refreshTimer.isValid) return;
    _refreshTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 target:self selector:@selector(tick) userInfo:nil repeats:YES];
}

// One timer tick: refresh the rows (and the details of a shown cheat, whose dry run changes with the game), and stop for a
// panel that is no longer on screen
- (void)tick {
    if (!_panel.isVisible) { [_refreshTimer invalidate]; _refreshTimer = nil; return; }
    [self refresh];
    if (_detailsFor >= 0) [self fillDetails];
}

// The app was deactivated: the utility panel goes off screen with it, so the refresh timer stops until the app comes back
- (void)appResignedActive:(NSNotification *)n {
    (void)n;
    _upOnDeactivate = _panel.isVisible;
    [_refreshTimer invalidate];
    _refreshTimer = nil;
}

// The app is active again: AppKit brings back a panel that was up, so the timer is armed a main-queue turn later
- (void)appBecameActive:(NSNotification *)n {
    (void)n;
    dispatch_async(dispatch_get_main_queue(), ^{ if (_upOnDeactivate) [self startTimer]; });
}

// Shown: order the panel front (created first if needed) — orderFront, not makeKeyAndOrderFront, so the game window stays
// key. While the panel is on screen a ~1 s timer refreshes it
- (void)show {
    if (!_panel) [self makePanel];
    [_panel orderFront:self];
    [self refresh];
    [self startTimer];
}

// The close button: hide instead of close (the panel object and its autosaved frame stay), and stop the refresh timer
- (BOOL)windowShouldClose:(NSWindow *)w {
    if (w != _panel) return YES;
    [_refreshTimer invalidate];
    _refreshTimer = nil;
    for (NSTextField *v in _values.allValues) if (v.currentEditor) [v abortEditing];   // an unfinished value is abandoned
    [self giveKeyBack];   // while the panel is still key
    [_panel orderOut:nil];
    return NO;
}
@end

void front_run(void (*guest)(void)) {
    // A game: tell macOS its work is latency-critical, so the system does not trade its frame time for power while the frame rate is capped
    static id activity; activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"game"];
    if (!getenv("DM_WINDOW")) { guest(); return; }
    @autoreleasepool {
        NSProcessInfo.processInfo.processName = @DM_GAME_NAME;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect r = NSMakeRect(0, 0, 1280, 720);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.contentMinSize = NSMakeSize(640, 360);
        win.title = @DM_GAME_NAME;   // the game profile's name (game name label)
        // Fullscreen: without FullScreenPrimary the green button only zooms the window. The present path already scales
        // the game picture to fit the drawable with black bars around it, so fullscreen keeps the game's aspect ratio.
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        NSView *v = win.contentView = [[DmGameView alloc] initWithFrame:r];
        v.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        // Apple's HUD over the game (frame rate, frame and GPU time, memory): front_hud_env turns it on before the first device. The layer's developerHUDProperties
        // stay unset: mode=default forces this layer's HUD on, so the Metal HUD menu's Hide HUD left it on screen
        if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) LOG("front end: Metal performance HUD on (DM_METAL_HUD)");
        v.layer = g_layer; [(DmGameView *)v fitDrawable];
        [win center];
        [win makeKeyAndOrderFront:nil];
        screen_refresh(win.screen ?: NSScreen.mainScreen);
        g_win = win;
        [NSApp activateIgnoringOtherApps:YES];
        // A host another process starts (scripts/regress.py) is not activated (its focus state stays 2), so its window opened wherever it landed in the window order, and behind
        // another app's window macOS fires its display link about once a second: Breath of the Wild stepped up to 3 vsyncs at boot and never came back (20 fps runs).
        // DM_WINDOW_TOP=1 orders it in front without activation and keeps it above other apps' windows; the log names every change of the window's visibility either way
        if (getenv("DM_WINDOW_TOP") && atoi(getenv("DM_WINDOW_TOP"))) { win.level = NSFloatingWindowLevel; [win orderFrontRegardless]; LOG("front end: window kept in front (DM_WINDOW_TOP)"); }
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeOcclusionStateNotification object:win queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
            (void)n; static int vis = -1; int v = (win.occlusionState & NSWindowOcclusionStateVisible) != 0; if (v == vis) return; vis = v;
            if (v) LOG("front end: window visible"); else LOG("front end: window covered (its display link fires about once a second: frame times measure nothing)"); }];
        pad_settings_load(); [(DmGameView *)v fitDrawable];
        g_settings_panel = [[DmSettingsPanel alloc] initWithParent:win snapshot:^{ return settings_snapshot(); }
            change:^(DmMacSetting setting, NSInteger value) { settings_changed(setting, value); }];
        if (pso_user_path() && getenv("DM_NVN_SHMAP")) g_settings_panel.shaderLists = @[pso_user_path(), [@(getenv("DM_NVN_SHMAP")).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"]];   // the seed beside the shader map, as nvn_mtl.m reads it; no map named: no export
        g_hud_view = [[DmCornerHUD alloc] initWithSnapshot:^{ return settings_snapshot(); }];
        [v addSubview:g_hud_view]; [g_hud_view refresh];
        // Main menu: AppKit runs ⌘ shortcuts only through menu items, so ⌘Q quits and ⌃⌘F toggles fullscreen from here (AppKit shows the item as Globe-F and keeps ⌃⌘F as a hidden twin)
        NSMenu *bar = [NSMenu new], *app = [[NSMenu alloc] initWithTitle:@DM_GAME_NAME], *wm = [[NSMenu alloc] initWithTitle:@"Window"];
        [[bar addItemWithTitle:@DM_GAME_NAME action:nil keyEquivalent:@""] setSubmenu:app];
        [app addItemWithTitle:@"Quit " DM_GAME_NAME action:@selector(terminate:) keyEquivalent:@"q"];
        // Cheats (hle_cheats.c): the rows rebuild on every open through the delegate, the same rows the file reload changes
        g_cheats_menu = [DmCheatsMenuDelegate new];
        NSMenu *cm = [[NSMenu alloc] initWithTitle:@"Cheats"];
        cm.delegate = g_cheats_menu;
        cm.autoenablesItems = NO;   // the delegate sets enabled on every row itself
        // Fill the rows now: AppKit matches a key equivalent (⌘R, ⌘⇧C) only against items that exist, and it calls the
        // delegate's menuNeedsUpdate: when the menu is opened, not at launch, so without this neither works until the
        // Cheats menu has been opened once. Every open rebuilds them through the delegate as before
        [g_cheats_menu menuNeedsUpdate:cm];
        [[bar addItemWithTitle:@"Cheats" action:nil keyEquivalent:@""] setSubmenu:cm];
        if (getenv("DM_CHEATS_WINDOW") && atoi(getenv("DM_CHEATS_WINDOW")))   // the Cheats panel open from the start (beside the game, not key)
            dispatch_async(dispatch_get_main_queue(), ^{ [g_cheats_menu showPanel:nil]; });
        install_controller_menu(bar);
        NSMenuItem *preferences = [app insertItemWithTitle:@"Settings…" action:@selector(openSettings:) keyEquivalent:@"," atIndex:0];
        preferences.target = g_settings_menu_target;
        [app insertItem:NSMenuItem.separatorItem atIndex:1];
        settings_refresh();
        [[bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""] setSubmenu:wm];
        [wm addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        NSApp.mainMenu = bar;
        const char *fs = getenv("DM_FULLSCREEN");   // 1 (or any value but 0): start in fullscreen; 0: a window; unset: the settings window's "Start in full screen" (MacNativeGames)
        if (fs ? strcmp(fs, "0") != 0 : g_start_fullscreen) {
            // A game opened from a fullscreen app starts while macOS leaves that Space. Wait for activation and for the window to reach the active Space before asking for fullscreen
            // (whichever happens last), and repeat a refused startup request once. Only a request made here owns a retry; a later player request must never restart startup handling
            static DmWinDelegate *wd; wd = [DmWinDelegate new]; win.delegate = wd;
            __block int want = 1, retried = 0, entering = 0;
            void (^enter)(void) = ^{
                if (!want || (win.styleMask & NSWindowStyleMaskFullScreen) || !NSApp.isActive || !win.isOnActiveSpace) return;
                want = 0; entering = 1; [win toggleFullScreen:nil]; };
            g_fs_failed = ^{
                if (!entering) return;
                entering = 0;
                LOG("front end: fullscreen was refused (app active %d, window on the active Space %d)%s", NSApp.isActive, win.isOnActiveSpace, retried ? "" : ", asking once more");
                if (retried) return;
                retried = 1; want = 1; dispatch_async(dispatch_get_main_queue(), enter); };
            [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; enter(); }];
            [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; enter(); }];
            for (NSNotificationName nm in @[NSWindowWillEnterFullScreenNotification, NSWindowWillExitFullScreenNotification])
                [NSNotificationCenter.defaultCenter addObserverForName:nm object:win queue:nil usingBlock:^(NSNotification *n) {
                    (void)n; want = 0;
                    if (!entering) g_fs_failed = nil; }];   // the player took over while startup was waiting; our own transition has entering set
            [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) {
                (void)n; want = 0; entering = 0; g_fs_failed = nil; }];   // startup succeeded: later manual refusals have no startup request to retry
            dispatch_async(dispatch_get_main_queue(), enter);
        }
        for (NSNotificationName nm in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
            [NSNotificationCenter.defaultCenter addObserverForName:nm object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                GCController *c = n.object; int conn = n.name == GCControllerDidConnectNotification;
                if (!c.extendedGamepad || g_merge) LOG("controller %s: %s%s", conn ? "connected" : "disconnected", pad_name(c), c.extendedGamepad ? "" : " (no extended gamepad profile: not used)");
                pads_changed(conn ? nil : c); }];
        dm_haptics_apple_init();
        pads_start();
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual
            if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand)) return e;   // ⌘ shortcuts go to the menu; a key released while ⌘ is held still reaches the game, or it would stay pressed
            if (!(e.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption)) &&
                dm_mac_is_shortcut(e.keyCode)) {
                if (e.type == NSEventTypeKeyDown) {
                    DmMacSetting setting; NSInteger value;
                    DmShortcutAction action=dm_mac_shortcut(e.keyCode,e.isARepeat,settings_snapshot(),&setting,&value);
                    if(action==DmShortcutSettings) [g_settings_panel showPanel];
                    else if(action==DmShortcutChange) settings_changed(setting,value);
                }
                return nil;
            }
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // Swallow it to avoid the system beep
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; dm_host_shutdown("Mac game window closed"); }];
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillTerminateNotification object:NSApp queue:nil
                                                   usingBlock:^(NSNotification *n) { (void)n; dm_host_shutdown("Mac application terminated"); }];
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1)
        // Input is released when the window stops being key (and when it is minimized) and taken up again when it is key again
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidChangeScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; screen_refresh(win.screen); if (win.screen) g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; screen_refresh(win.screen); }];
        // Entering and leaving fullscreen: the layer is layer-backed, so its frame (and with it the drawable) follows the
        // content view by itself; only the scale factor has to be carried over to the screen the window ended up on. The screen's rate is matched in fullscreen only (screen_match_apply)
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; screen_refresh(win.screen); g_fullscreen = 1; g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_fullscreen = 0; screen_refresh(win.screen); g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); if (!win.miniaturized) dm_focus_set(2); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_resume(); dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        display_awake(win.isKeyWindow);   // the window may have become key (makeKeyAndOrderFront above) before these observers were installed
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// Progress screen of the prepare step (front.h front_progress): a dark view over the game's, the title, a bar and a count, centered; AppKit views, so it draws while the game's thread waits
static NSView *g_prog; static NSTextField *g_prog_title, *g_prog_count; static NSProgressIndicator *g_prog_bar;
static NSTextField *prog_label(NSRect r, CGFloat size, NSFontWeight w, NSColor *c) {
    NSTextField *t = [NSTextField labelWithString:@""]; t.frame = r; t.alignment = NSTextAlignmentCenter; t.textColor = c;
    t.font = [NSFont monospacedDigitSystemFontOfSize:size weight:w]; return t;
}
void (*front_progress_hook)(const char *title, long done, long total);
void front_progress(const char *title, long done, long total) {
    if (!g_layer) return;
    NSString *tt = title ? @(title) : nil;
    void (*hook)(const char *, long, long) = front_progress_hook;
    if (hook) { dispatch_async(dispatch_get_main_queue(), ^{ hook(tt.UTF8String, done, total); }); return; }   // the host app draws it (front.h)
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!tt) { [g_prog removeFromSuperview]; g_prog = nil; return; }
        if (!g_prog) {
            NSView *cv = g_win.contentView;
            g_prog = [[NSView alloc] initWithFrame:cv.bounds]; g_prog.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            g_prog.wantsLayer = YES; g_prog.layer.backgroundColor = [NSColor colorWithWhite:0.06 alpha:0.94].CGColor;
            NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(floor((cv.bounds.size.width - 400) / 2), floor((cv.bounds.size.height - 96) / 2), 400, 96)];
            box.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin;
            g_prog_title = prog_label(NSMakeRect(0, 62, 400, 30), 20, NSFontWeightSemibold, NSColor.whiteColor);
            g_prog_bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 36, 360, 20)];
            g_prog_bar.style = NSProgressIndicatorStyleBar; g_prog_bar.indeterminate = NO; g_prog_bar.minValue = 0;
            g_prog_count = prog_label(NSMakeRect(0, 4, 400, 22), 13, NSFontWeightRegular, [NSColor colorWithWhite:0.72 alpha:1]);
            [box addSubview:g_prog_title]; [box addSubview:g_prog_bar]; [box addSubview:g_prog_count]; [g_prog addSubview:box];
            [cv addSubview:g_prog];
        }
        g_prog_title.stringValue = tt; g_prog_bar.maxValue = total > 0 ? (double)total : 1; g_prog_bar.doubleValue = (double)done;
        g_prog_count.stringValue = [NSString stringWithFormat:@"%@ of %@", [NSNumberFormatter localizedStringFromNumber:@(done) numberStyle:NSNumberFormatterDecimalStyle],
                                    [NSNumberFormatter localizedStringFromNumber:@(total) numberStyle:NSNumberFormatterDecimalStyle]];
    });
}

// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
extern int g_swkbd_min_chars;   // hle_swkbd.c: this input's minimum length from ShowKeyboardArg; the applet keeps OK disabled until the text is long enough
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        // The box takes the keyboard and the mouse, so the game window never sees the key-up of the button or the touch-up of the click that opened it: nothing may stay pressed
        // while it is up, or the game reads the stuck button / touch the moment the box closes (Stardew Valley's character creation re-opened the keyboard endlessly)
        g_btn = 0; g_w = g_a = g_s = g_d = 0; g_rl = g_ru = g_rr = g_rd = 0; push();
        dm_touch_set(NULL, 0);
        int min_chars = g_swkbd_min_chars > 0 ? g_swkbd_min_chars : 0;
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        for (;;) {
            NSAlert *a = [NSAlert new];
            a.messageText = title && *title ? @(title) : @"Input";
            a.informativeText = info;
            [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"];
            NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
            f.stringValue = text ?: (initial ? @(initial) : @"");
            if (guide && *guide) f.placeholderString = @(guide);
            a.accessoryView = f; a.window.initialFirstResponder = f;
            r = [a runModal] == NSAlertFirstButtonReturn;
            text = f.stringValue;
            if (!r || (int)text.length >= min_chars) break;
            // On hardware the applet would not have accepted the shorter text (OK stayed disabled); a game that rejects it re-opens the keyboard for ever, so the box comes back with the rule instead
            info = [info stringByAppendingFormat:@"\nAt least %d characters are needed here.", min_chars];
        }
    });
    if (!r) return 0;
    (void)max_chars;                              // Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
