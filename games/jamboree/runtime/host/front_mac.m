// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard and controllers → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// The window is fullscreen-capable (the green button and ⌃⌘F); DM_FULLSCREEN starts it that way. ⌘Q quits; the left mouse button is the touchscreen.
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
#include "front.h"
#include "hle.h"
#include "pad_slots.h"
#include "haptics_apple.h"
#import "front_settings.h"
#include "mac_display_settings.h"
#include <signal.h>
#include <math.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
static _Atomic int g_screen_fps;   // the window's screen's maximumFramesPerSecond, kept up to date when the window moves to another screen
int front_screen_max_fps(void) { return g_screen_fps; }
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
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    dm_input_set(g_btn, lx, ly, 0, 0);
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
    unsigned short c = e.keyCode;
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    int b = key_bit(c);
    uint64_t m = b >= 0 ? 1ull << b : c == 49 ? DM_BTN_SHAKE : 0;   // Space: shaken while held
    g_btn = down ? (g_btn | m) : (g_btn & ~m);
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
static int g_start_fullscreen, g_output_preset, g_show_hints = 1;
static DmSettingsPanel *g_settings_panel;
static DmShortcutHUD *g_shortcut_hud;
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
    NSString *text = [NSString stringWithFormat:@"layout=%s\nswap_ab=%d\nfullscreen=%d\noutput=%d\nhints=%d\n",
                                                layout[g_face_layout], !!g_swap_ab, !!g_start_fullscreen, g_output_preset, !!g_show_hints];
    NSError *error = nil;
    if (![text writeToFile:g_pad_settings_path atomically:YES encoding:NSUTF8StringEncoding error:&error])
        LOG("front end: could not save controller settings to %s: %s", g_pad_settings_path.fileSystemRepresentation,
            error.localizedDescription.UTF8String ?: "unknown error");
}

static void pad_settings_load(void) {
    g_pad_settings_path = pad_settings_path();
    g_face_layout = DM_FACE_AUTO;
    g_swap_ab = 0;
    g_start_fullscreen = 0; g_output_preset = 0; g_show_hints = 1;
    const char *e;
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
        else if ([key isEqualToString:@"hints"]) g_show_hints = value.intValue != 0;
    }
    e = getenv("DM_PAD_LAYOUT");
    if (e && !strcmp(e, "auto")) g_face_layout = DM_FACE_AUTO;
    else if (e && !strcmp(e, "position")) g_face_layout = DM_FACE_POSITION;
    else if (e && !strcmp(e, "label")) g_face_layout = DM_FACE_LABEL;
    e = getenv("DM_PAD_SWAP_AB");
    if (e) g_swap_ab = atoi(e) != 0;
    e = getenv("DM_FULLSCREEN");
    if (e) g_start_fullscreen = atoi(e) != 0;
    static const char *layout[] = {"auto", "position", "label"};
    LOG("front end: controller face buttons %s%s%s", layout[g_face_layout], g_swap_ab ? ", A/B swapped" : "",
        g_pad_settings_path ? "" : " (no DM_SAVE: settings are not kept)");
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
static void pad_read(GCExtendedGamepad *g) {
    int p = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)g.controller); if (p < 0) return;
    dm_haptics_apple_controller(p, g.controller);
    uint64_t m = pad_face_buttons(g, pad_face_swap_abxy(g.controller));
    #define K(btn, bit) if (btn.isPressed) m |= bit
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R); K(g.leftTrigger, DM_BTN_ZL); K(g.rightTrigger, DM_BTN_ZR);
    K(g.buttonMenu, DM_BTN_PLUS); K(g.buttonOptions, DM_BTN_MINUS); K(g.leftThumbstickButton, DM_BTN_LS); K(g.rightThumbstickButton, DM_BTN_RS);   // nil on pads without them: not pressed
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
    #undef K
    dm_pad_set_player(p, m, pad_axis(g.leftThumbstick.xAxis.value), pad_axis(g.leftThumbstick.yAxis.value), pad_axis(g.rightThumbstick.xAxis.value), pad_axis(g.rightThumbstick.yAxis.value));
}
static const char *pad_name(GCController *c) { return c.vendorName.UTF8String ?: "?"; }
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }
// Called at start and on every connect / disconnect (gone: the controller that was just removed, nil on a connect)
static void pads_changed(GCController *gone) {
    if (g_merge) {
        int any = 0;
        for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) {
            any = 1; c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
            pad_read(c.extendedGamepad);
        }
        if (!any) { dm_pad_set(0, 0, 0, 0, 0); dm_haptics_apple_controller(0, nil); }
        return;
    }
    NSArray<GCController *> *now = GCController.controllers; char nb[16];
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p] && (g_held[p] == gone || ![now containsObject:g_held[p]])) {
        npad_name(p, nb, sizeof nb); LOG("controller '%s' disconnected: player %d (npad %s) is free", pad_name(g_held[p]), p + 1, nb);
        g_held[p].extendedGamepad.valueChangedHandler = nil;
        dm_haptics_apple_controller(p, nil);
        pad_slot_free(&g_slots, (__bridge const void *)g_held[p]); g_held[p] = nil;
        dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
    }
    for (GCController *c in now) if (c.extendedGamepad && c != gone && pad_slot_of(&g_slots, (__bridge const void *)c) < 0) {
        int p = pad_slot_take(&g_slots, (__bridge const void *)c, pad_name(c));
        if (p < 0) { LOG("controller '%s' connected: all %d players are taken, not used", pad_name(c), DM_PLAYERS); continue; }
        g_held[p] = c;
        c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
        npad_name(p, nb, sizeof nb); LOG("controller '%s' connected: player %d (npad %s), face buttons by %s", pad_name(c), p + 1, nb,
                                         pad_face_swap_abxy(c) ? "Nintendo labels" : "Xbox / PlayStation positions");
        pad_read(c.extendedGamepad);
    }
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
}

// Mouse as a single-point touchscreen (front.h dm_touch_set): left button down / drag = finger 0, release = lifted. The content view takes the clicks,
// not a window-wide event monitor, so clicks on the title bar, the window buttons and the resize edges still reach AppKit.
@interface DmGameView : NSView @end
@implementation DmGameView
- (void)mouseDown:(NSEvent *)e { [self mouseDragged:e]; }
- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil]; NSSize b = self.bounds.size;
    double w = b.width, h = b.height;
    if (w <= 0 || h <= 0) return;
    if (w / h > 16.0 / 9.0) w = h * 16.0 / 9.0; else h = w * 9.0 / 16.0;   // the centered 16:9 picture the present fits into the view, as on the iPhone (DmGamepad.m)
    double x0 = (b.width - w) / 2, y0 = (b.height - h) / 2;
    dm_touch t = {0, (float)fmin(fmax((p.x - x0) / w, 0), 0.999), (float)fmin(fmax(1 - (p.y - y0) / h, 0), 0.999)};   // The AppKit view origin is at bottom left, the touchscreen origin is at top left
    dm_touch_set(&t, 1);
}
- (void)mouseUp:(NSEvent *)e { (void)e; dm_touch_set(NULL, 0); }
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
- (void)toggleHints:(id)sender { (void)sender; settings_changed(DmSettingHints, !g_show_hints); }
@end

static DmSettingsMenu *g_settings_menu_target;
static void install_controller_menu(NSMenu *bar) {
    g_settings_menu_target = [DmSettingsMenu new];
    NSMenu *controller = [[NSMenu alloc] initWithTitle:@"控制器"];
    const char *titles[] = {"自动（推荐）", "按位置（Xbox / PlayStation）", "按标签（Nintendo）"};
    for (int i = 0; i < 3; i++) {
        NSMenuItem *item = [controller addItemWithTitle:@(titles[i]) action:@selector(chooseFaceLayout:) keyEquivalent:@""];
        item.target = g_settings_menu_target;
        item.tag = i;
        g_face_items[i] = item;
    }
    [controller addItem:NSMenuItem.separatorItem];
    g_swap_item = [controller addItemWithTitle:@"交换 A / B" action:@selector(toggleSwapAB:) keyEquivalent:@""];
    g_swap_item.target = g_settings_menu_target;
    [controller addItemWithTitle:@"恢复控制器默认设置" action:@selector(resetController:) keyEquivalent:@""].target = g_settings_menu_target;
    [[bar addItemWithTitle:@"控制器" action:nil keyEquivalent:@""] setSubmenu:controller];

    NSMenu *view = [[NSMenu alloc] initWithTitle:@"显示"];
    g_fullscreen_item = [view addItemWithTitle:@"下次启动时全屏" action:@selector(toggleStartFullscreen:) keyEquivalent:@""];
    g_fullscreen_item.target = g_settings_menu_target;
    NSMenu *outputs = [[NSMenu alloc] initWithTitle:dm_settings_text(@"输出尺寸上限", @"Output size cap")];
    NSArray<NSString *> *names = @[dm_settings_text(@"自动（随窗口）", @"Automatic (window size)"), @"720p", @"1080p", @"1440p", @"2160p"];
    for (int i = 0; i < 5; i++) {
        NSMenuItem *item = [outputs addItemWithTitle:names[i] action:@selector(chooseOutput:) keyEquivalent:@""];
        item.tag = i; item.target = g_settings_menu_target; g_output_items[i] = item;
    }
    [[view addItemWithTitle:outputs.title action:nil keyEquivalent:@""] setSubmenu:outputs];
    g_hints_item = [view addItemWithTitle:dm_settings_text(@"快捷键提示", @"Shortcut hints") action:@selector(toggleHints:) keyEquivalent:@""];
    g_hints_item.target = g_settings_menu_target;
    [[bar addItemWithTitle:@"显示" action:nil keyEquivalent:@""] setSubmenu:view];
    pad_apply_settings();
}

static DmMacSettings settings_snapshot(void) {
    CGSize size = g_layer.drawableSize;
    NSUInteger connected = 0; for (GCController *pad in GCController.controllers) if (pad.extendedGamepad) connected++;
    return (DmMacSettings){.face_layout = g_face_layout, .swap_ab = g_swap_ab, .start_fullscreen = g_start_fullscreen,
        .output_preset = g_output_preset, .show_hints = g_show_hints, .fullscreen = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0,
        .output_width = (NSUInteger)size.width, .output_height = (NSUInteger)size.height, .controllers = connected};
}
static void settings_refresh(void) {
    pad_apply_settings();
    for (int i = 0; i < 5; i++) g_output_items[i].state = g_output_preset == i;
    g_hints_item.state = g_show_hints;
    [g_settings_panel refresh]; [g_shortcut_hud refresh];
}
static void settings_changed(DmMacSetting setting, NSInteger value) {
    switch (setting) {
        case DmSettingFaceLayout: if (value >= 0 && value < 3) g_face_layout = (int)value; break;
        case DmSettingSwapAB: g_swap_ab = value != 0; break;
        case DmSettingStartFullscreen: g_start_fullscreen = value != 0; break;
        case DmSettingOutput:
            if (value >= 0 && value < 5) g_output_preset = (int)value;
            [(DmGameView *)g_win.contentView fitDrawable]; break;
        case DmSettingHints: g_show_hints = value != 0; break;
        case DmSettingFullscreen:
            if (((g_win.styleMask & NSWindowStyleMaskFullScreen) != 0) != (value != 0)) [g_win toggleFullScreen:nil];
            break;
        case DmSettingResetController: g_face_layout = DM_FACE_AUTO; g_swap_ab = 0; break;
    }
    pad_settings_save(); settings_refresh();
    LOG("front end: settings layout=%d swap_ab=%d output=%d hints=%d start_fullscreen=%d", g_face_layout, g_swap_ab, g_output_preset, g_show_hints, g_start_fullscreen);
}

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

// Metal reads MTL_HUD_ENABLED when the process makes its first device, which can be before the window: set from DM_METAL_HUD before main (set in front_run, the HUD did not show)
__attribute__((constructor)) static void hud_env(void) { if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) setenv("MTL_HUD_ENABLED", "1", 0); }
void front_run(void (*guest)(void)) {
    // A game: tell macOS its work is latency-critical, so the system does not trade its frame time for power while the frame rate is capped
    static id activity; activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"game"];
    if (!getenv("DM_WINDOW")) { guest(); return; }
    @autoreleasepool {
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
        if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) { g_layer.developerHUDProperties = @{@"mode": @"default"}; LOG("front end: Metal performance HUD on (DM_METAL_HUD)"); }   // Apple's HUD over the game: frame rate, frame and GPU time, memory
        v.layer = g_layer; [(DmGameView *)v fitDrawable];
        [win center];
        [win makeKeyAndOrderFront:nil];
        g_screen_fps = (int)(win.screen ?: NSScreen.mainScreen).maximumFramesPerSecond;
        g_win = win;
        [NSApp activateIgnoringOtherApps:YES];
        pad_settings_load(); [(DmGameView *)v fitDrawable];
        g_settings_panel = [[DmSettingsPanel alloc] initWithParent:win snapshot:^{ return settings_snapshot(); }
            change:^(DmMacSetting setting, NSInteger value) { settings_changed(setting, value); }];
        g_shortcut_hud = [[DmShortcutHUD alloc] initWithSnapshot:^{ return settings_snapshot(); }
            openSettings:^{ [g_settings_panel showPanel]; }];
        g_shortcut_hud.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
        [v addSubview:g_shortcut_hud]; [g_shortcut_hud refresh];
        // Main menu: AppKit runs ⌘ shortcuts only through menu items, so ⌘Q quits and ⌃⌘F toggles fullscreen from here (AppKit shows the item as Globe-F and keeps ⌃⌘F as a hidden twin)
        NSMenu *bar = [NSMenu new], *app = [NSMenu new], *wm = [[NSMenu alloc] initWithTitle:@"Window"];
        [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
        [app addItemWithTitle:@"Quit " DM_GAME_NAME action:@selector(terminate:) keyEquivalent:@"q"];
        install_controller_menu(bar);
        NSMenuItem *preferences = [app insertItemWithTitle:dm_settings_text(@"设置…", @"Settings…") action:@selector(openSettings:) keyEquivalent:@"," atIndex:0];
        preferences.target = g_settings_menu_target;
        [app insertItem:NSMenuItem.separatorItem atIndex:1];
        settings_refresh();
        [[bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""] setSubmenu:wm];
        [wm addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        NSApp.mainMenu = bar;
        if (g_start_fullscreen) dispatch_async(dispatch_get_main_queue(), ^{ [win toggleFullScreen:nil]; });
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
                (e.keyCode == 122 || e.keyCode == 18 || e.keyCode == 20 || e.keyCode == 22)) {
                if (e.type == NSEventTypeKeyDown && !e.isARepeat) {
                    if (e.keyCode == 122) [g_settings_panel showPanel];
                    else if (e.keyCode == 18) settings_changed(DmSettingHints, !g_show_hints);
                    else if (e.keyCode == 20) settings_changed(DmSettingOutput, (g_output_preset + 1) % 5);
                    else settings_changed(DmSettingFaceLayout, (g_face_layout + 1) % 3);
                }
                return nil;
            }
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // Swallow it to avoid the system beep
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; exit(0); }];
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1)
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidChangeScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (win.screen) { g_screen_fps = (int)win.screen.maximumFramesPerSecond; g_layer.contentsScale = win.backingScaleFactor; } screen_match_apply(); }];
        // Entering and leaving fullscreen: the layer is layer-backed, so its frame (and with it the drawable) follows the
        // content view by itself; only the scale factor has to be carried over to the screen the window ended up on. The screen's rate is matched in fullscreen only (screen_match_apply)
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); }];
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_btn = 0; g_w = g_a = g_s = g_d = 0; push(); if (!win.miniaturized) dm_focus_set(2); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? @(title) : @"Input";
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        a.informativeText = info;
        [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"];
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        f.stringValue = initial ? @(initial) : @"";
        if (guide && *guide) f.placeholderString = @(guide);
        a.accessoryView = f; a.window.initialFirstResponder = f;
        r = [a runModal] == NSAlertFirstButtonReturn;
        text = f.stringValue;
    });
    if (!r) return 0;
    (void)max_chars;                              // Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
