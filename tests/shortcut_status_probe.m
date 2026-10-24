/* Production AppKit panel, keyboard dispatch, mode state and output helpers.
   No guest, save files, game window or GPU commands are used. */
#include "../games/totk/runtime/frontend.m"
#include "../games/totk/runtime/input.c"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/fps_overlay.h"
#include <assert.h>

void mfx_set(int mode);
int mfx_latch(void);
void mfxup_set(int on);
void mfxup_output_set(int on);
void shortcut_test_output_init(void);
void shortcut_test_output_size(unsigned w, unsigned h);

void dm_gfx_set_flag(const char *name, int value) {
    if (!strcmp(name, "mfx_interp")) mfx_set(value);
    else if (!strcmp(name, "mfx_output")) mfxup_output_set(value);
    else if (!strcmp(name, "mfx_upscale")) mfxup_set(value);
    else abort();
}
void shortcut_test_aspect_init(double view);
/* runtime/gamepad.m's key 6 state without GameController (gamepad_layout.h's real cycle): a DualSense is connected. */
static int fakePadType = GAMEPAD_TYPE_AUTO, fakePadDetected = GAMEPAD_TYPE_PLAYSTATION;
int runtime_gamepad_type_cycle(void) { return fakePadType = gamepad_type_next(fakePadType); }
int runtime_gamepad_type_set(int type) { return fakePadType = type; }
int runtime_gamepad_type(int *detected) { if (detected) *detected = fakePadDetected; return fakePadType; }
/* runtime/gamepad.m's mouse mode (key 7) without GameController; the window never has the keyboard here. */
static int fakeMouse;
int runtime_mouse_toggle(void) { return fakeMouse = !fakeMouse; }
int runtime_mouse_enabled(void) { return fakeMouse; }
void runtime_mouse_set_hook(void (*hook)(void)) { (void)hook; }
void runtime_mouse_move(double dx, double dy) { (void)dx; (void)dy; }
void runtime_mouse_button(int button, int down) { (void)button; (void)down; }
void runtime_mouse_release(void) {}
static void press(unsigned key) {
    runtime_input_key_synthetic(key, 1);
    runtime_input_key_synthetic(key, 0);
}
static void tick(void) {
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    [((TOTKShortcutHelp *)shortcutHelp).timer fire];
}
/* The Settings menu's item for a choice (tag kind << 8 | value), its submenus refreshed as when it opens. */
static NSMenuItem *setting(NSMenu *menu, int kind, int value) {
    [settings menuNeedsUpdate:menu];
    for (NSMenuItem *item in menu.itemArray) {
        if (item.submenu) { NSMenuItem *found = setting(item.submenu, kind, value); if (found) return found; }
        else if (item.tag == (kind << 8 | value)) return item;
    }
    return nil;
}
static NSMenu *settingsMenu;
static void choose(int kind, int value) { NSMenuItem *item = setting(settingsMenu, kind, value); assert(item && item.enabled); [settings choose:item]; }
static int checked(int kind, int value) { return setting(settingsMenu, kind, value).state == NSControlStateValueOn; }
static void expect(TOTKShortcutHelp *panel, NSString *text) {
    if (![panel.label.stringValue containsString:text]) {
        fprintf(stderr, "expected %s in:\n%s\n", text.UTF8String, panel.label.stringValue.UTF8String);
        abort();
    }
}

int main(int argc, const char **argv) { @autoreleasepool {
    [NSApplication sharedApplication];
    shortcut_test_output_init();
    int initial = mfx_enabled();
    assert(initial == ((!getenv("TOTK_MFX") || !strcmp(getenv("TOTK_MFX"), "0")) ? MFX_OFF : MFX_HALF));
    NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 900, 540)];
    TOTKShortcutHelp *panel = [[TOTKShortcutHelp alloc] initWithFrame:NSZeroRect];
    [host addSubview:panel]; shortcutHelp = panel;
    assert(panel.timer && panel.timer.valid);
    assert(panel.timer.timeInterval == 0.5);
    [panel refresh];
    expect(panel, initial ? @"half refresh rate" : @"off");
    expect(panel, mfxup_output_enabled() ? @"MetalFX 2K" : @"Picture: off");

    /* A request is not the current frame's interpolation mode until latched. */
    mfx_set(MFX_OFF); mfx_latch();
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.6]];
    expect(panel, @"Interpolation: off");
    press(19); tick(); expect(panel, @"Interpolation: off");
    mfx_latch(); tick(); expect(panel, @"Interpolation: 30+30");
    runtime_input_key_synthetic(19, 1); runtime_input_key_synthetic(19, 1);
    runtime_input_key_synthetic(19, 0); mfx_latch(); tick();
    expect(panel, @"Interpolation: half refresh rate");
    press(19); mfx_latch(); tick(); expect(panel, @"Interpolation: unlimited");
    press(19); mfx_latch(); tick(); expect(panel, @"Interpolation: off");

    mfxup_output_set(0); tick(); expect(panel, @"Picture: off");
    press(20); tick(); expect(panel, @"Picture: MetalFX 2K");
    press(20); tick(); expect(panel, @"Picture: native 2K");
    press(20); tick(); expect(panel, @"Picture: off");
    expect(panel, @"Edge smoothing (MetalFX Temporal): off");
    press(23); tick(); expect(panel, @"Edge smoothing (MetalFX Temporal): on");
    press(23); tick(); expect(panel, @"Edge smoothing (MetalFX Temporal): off");
    /* Key 6: auto (what it takes the controller for) -> Xbox -> PlayStation -> Switch -> auto. */
    expect(panel, @"Controller type: auto (PlayStation) (current; press to cycle)");
    press(22); tick(); expect(panel, @"Controller type: Xbox (current");
    runtime_input_key_synthetic(22, 1); runtime_input_key_synthetic(22, 1); runtime_input_key_synthetic(22, 0); tick();
    expect(panel, @"Controller type: PlayStation (current");   /* a held key acts once */
    press(22); tick(); expect(panel, @"Controller type: Switch (current");
    press(22); tick(); expect(panel, @"Controller type: auto (PlayStation)");
    fakePadDetected = -1; tick(); expect(panel, @"Controller type: auto (no controller)");
    /* Key 7: mouse mode off -> on -> off. */
    expect(panel, @"7  Mouse: off (current; press to switch)");
    press(26); tick(); expect(panel, @"Mouse: on (camera, buttons; Command-Tab frees the pointer)");
    press(26); tick(); expect(panel, @"Mouse: off");
    press(26);   /* on for the Chinese check below */
    expect(panel, @"Aspect ratio: not available in this game");   /* runtime_aspect_step -1 (2026-10-23: was shown as 16:9) */
    shortcut_test_aspect_init(1.6);   /* key 4 becomes available; a 16:10 window */
    press(21); tick(); expect(panel, @"Aspect ratio: fit to window 1.60:1");
    press(21); tick(); expect(panel, @"Aspect ratio: 21:9");
    press(21); tick(); expect(panel, @"Aspect ratio: 16:9");
    /* Startup/diagnostic paths can set the canvas without a key press. */
    shortcut_test_output_size(3840, 2160); tick(); expect(panel, @"Picture: native 3840×2160");
    shortcut_test_output_size(2560, 1440);
    mfx_set(MFX_HALF); mfx_latch();
    setenv("TOTK_UI_LANG", "zh", 1); tick();
    expect(panel, @"补帧：当前 刷新率一半"); expect(panel, @"画质：当前 原生 2K");
    expect(panel, @"6  手柄类型：当前 自动（未接手柄）（按键切换）");
    expect(panel, @"7  鼠标操作：当前 开（视角、按键，⌘Tab 释放指针）（按键切换）");
    fakePadDetected = GAMEPAD_TYPE_SWITCH; tick(); expect(panel, @"手柄类型：当前 自动（Switch）");

    if (hud_compact()) {
        assert(panel.frame.size.width < host.bounds.size.width - 20);
        assert(panel.frame.size.height < host.bounds.size.height - 20);
        assert(panel.frame.origin.x >= 0 && panel.frame.origin.y >= 0);
        assert(NSWidth(panel.label.frame) >= panel.textSize.width);
    } else {
        NSSize fit = [panel.label sizeThatFits:panel.label.bounds.size];
        assert(fit.height <= panel.label.bounds.size.height);
    }
    /* Key 1: all -> minimal (FPS only) -> off -> all. */
    hud_publish(HUD_FPS, "FPS 60  2560x1440", 0); hud_publish(HUD_SHADER, "New shaders 3 / total 9", 0);
    press(18); tick(); assert(fps_overlay_hints_enabled() && !fps_overlay_full_hud());
    if (hud_compact()) {
        assert(!panel.hidden); expect(panel, @"FPS 60  2560x1440");
        assert(![panel.label.stringValue containsString:@"New shaders"] && ![panel.label.stringValue containsString:@"快捷键"]);
    } else assert(panel.hidden);   // corners: the FPS corner stays in the picture, the shortcut box goes
    press(18); tick(); assert(panel.hidden && !fps_overlay_hints_enabled());
    mfx_set(MFX_MAX); mfx_latch(); tick();
    press(18); tick(); assert(!panel.hidden && fps_overlay_full_hud());
    expect(panel, @"补帧：当前 不限帧");
    hud_publish(0, "60 FPS (30 + 30)", 0); tick();
    if (hud_compact()) expect(panel, @"60 FPS (30 + 30)");

    /* The Settings menu: each choice does what its key does, and the current one is checked. */
    setenv("TOTK_UI_LANG", "en", 1);
    NSMenu *bar = [NSMenu new]; front_settings_add(bar);
    settingsMenu = bar.itemArray.lastObject.submenu;
    assert([settingsMenu.title isEqualToString:@"Settings"]);
    assert(checked(SET_HUD, HUD_MODE_ALL) && !checked(SET_HUD, HUD_MODE_OFF));
    choose(SET_HUD, HUD_MODE_OFF); tick(); assert(fps_overlay_hud_mode() == HUD_MODE_OFF && panel.hidden && checked(SET_HUD, HUD_MODE_OFF));
    choose(SET_HUD, HUD_MODE_MINIMAL); tick(); assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    choose(SET_HUD, HUD_MODE_ALL); choose(SET_HUD, HUD_MODE_ALL); tick();
    assert(fps_overlay_hud_mode() == HUD_MODE_ALL && !panel.hidden && checked(SET_HUD, HUD_MODE_ALL));
    choose(SET_INTERP, MFX_30); mfx_latch(); tick();
    assert(checked(SET_INTERP, MFX_30) && !checked(SET_INTERP, MFX_MAX)); expect(panel, @"Interpolation: 30+30");
    choose(SET_INTERP, MFX_OFF); mfx_latch(); tick(); expect(panel, @"Interpolation: off");
    assert(runtime_output_mode_now() == PICTURE_NATIVE_2K && checked(SET_PICTURE, PICTURE_NATIVE_2K));
    choose(SET_PICTURE, PICTURE_METALFX_2K); tick(); expect(panel, @"Picture: MetalFX 2K");
    assert(runtime_output_size_current() == (1920 | (uint64_t)1080 << 32) && checked(SET_PICTURE, PICTURE_METALFX_2K));
    choose(SET_PICTURE, PICTURE_NATIVE_2K); tick(); expect(panel, @"Picture: native 2K");
    assert(!mfxup_output_enabled() && checked(SET_PICTURE, PICTURE_NATIVE_2K));
    choose(SET_PICTURE, PICTURE_OFF); tick(); expect(panel, @"Picture: off");
    assert(runtime_output_size_current() == (1920 | (uint64_t)1080 << 32) && checked(SET_PICTURE, PICTURE_OFF));
    choose(SET_TAA, 0); tick(); assert(taa_enabled() && checked(SET_TAA, 0)); expect(panel, @"Edge smoothing (MetalFX Temporal): on");
    choose(SET_TAA, 0); tick(); assert(!taa_enabled() && !checked(SET_TAA, 0));
    fakePadDetected = GAMEPAD_TYPE_PLAYSTATION;
    choose(SET_PAD, GAMEPAD_TYPE_SWITCH); tick(); assert(checked(SET_PAD, GAMEPAD_TYPE_SWITCH) && !checked(SET_PAD, GAMEPAD_TYPE_AUTO));
    expect(panel, @"Controller type: Switch (current");
    choose(SET_PAD, GAMEPAD_TYPE_AUTO); tick(); assert(checked(SET_PAD, GAMEPAD_TYPE_AUTO)); expect(panel, @"Controller type: auto (PlayStation)");
    int mouse = runtime_mouse_enabled();
    choose(SET_MOUSE, 0); tick(); assert(runtime_mouse_enabled() == !mouse && checked(SET_MOUSE, 0) == !mouse);
    choose(SET_MOUSE, 0); tick(); assert(runtime_mouse_enabled() == mouse && checked(SET_MOUSE, 0) == mouse);
    assert(checked(SET_ASPECT, 0) && setting(settingsMenu, SET_ASPECT, 2).enabled);   // key 4 made available above
    choose(SET_ASPECT, 2); tick(); assert(runtime_aspect_step() == 2 && checked(SET_ASPECT, 2)); expect(panel, @"Aspect ratio: 21:9");
    choose(SET_ASPECT, 1); tick(); assert(checked(SET_ASPECT, 1)); expect(panel, @"Aspect ratio: fit to window 1.60:1");
    choose(SET_ASPECT, 0); tick(); assert(runtime_aspect_now() == 0 && checked(SET_ASPECT, 0)); expect(panel, @"Aspect ratio: 16:9");
    /* Not started from the app (no TOTK_PREFS_APP): the start options are shown but cannot be saved. */
    for (int i = 0; i < START_OPTIONS; i++) assert(setting(settingsMenu, SET_START, i) && !setting(settingsMenu, SET_START, i).enabled);

    if (argc == 2) {
        NSBitmapImageRep *rep = [panel bitmapImageRepForCachingDisplayInRect:panel.bounds];
        [panel cacheDisplayInRect:panel.bounds toBitmapImageRep:rep];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
                writeToFile:@(argv[1]) atomically:YES]);
    }
    [panel.timer invalidate];
    puts("pass: current modes, keyboard cycling, frame latch, localization, both layouts, key 1 all/minimal/off, key 6 controller type, key 7 mouse and the Settings menu");
    return 0;
}}
