/* The production Mac settings panel (key 1, runtime/settings_panel.h), its keyboard handling and the calls each row makes.
   No guest, save files, game data or GPU commands are used. */
#include "../games/totk/runtime/frontend.m"
#include "../games/totk/runtime/input.c"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/fps_overlay.h"
#include <assert.h>

void mfx_set(int mode);
int mfx_latch(void);
void mfxup_set(int on);
void mfxup_output_set(int on);
int mfxup_output_enabled(void);
void settings_test_output_init(void);
void settings_test_output_size(unsigned w, unsigned h);
void settings_test_aspect_init(double view);

void dm_gfx_set_flag(const char *name, int value) {
    if (!strcmp(name, "mfx_interp")) mfx_set(value);
    else if (!strcmp(name, "mfx_output")) mfxup_output_set(value);
    else if (!strcmp(name, "mfx_upscale")) mfxup_set(value);
    else abort();
}
/* runtime/gamepad.m's controller type and mouse mode without GameController: a DualSense is connected. */
static int fakePadType = GAMEPAD_TYPE_AUTO, fakePadDetected = GAMEPAD_TYPE_PLAYSTATION, fakeMouse;
int runtime_gamepad_type_set(int type) { return fakePadType = type; }
int runtime_gamepad_type(int *detected) { if (detected) *detected = fakePadDetected; return fakePadType; }
int runtime_mouse_set(int on) { return fakeMouse = !!on; }
int runtime_mouse_enabled(void) { return fakeMouse; }
void runtime_mouse_set_hook(void (*hook)(void)) { (void)hook; }
void runtime_mouse_move(double dx, double dy) { (void)dx; (void)dy; }
void runtime_mouse_button(int button, int down) { (void)button; (void)down; }
void runtime_mouse_release(void) {}
void runtime_gamepad_clear(void) {}
/* gfx/nvn/hle_nvn.c's frame lock (its pacing is checked by probe-gfx's present-interval probe). */
static int fakeLock;
void nvn_fps_lock_set(int fps) { fakeLock = fps == 30 || fps == 40 || fps == 60 ? fps : 0; }
int nvn_fps_lock(void) { return fakeLock; }
/* A window key is the user's input (runtime/input.c user_input): it stops a survey and an auto-load, neither here. */
void runtime_survey_cancel_user(void) {}
void runtime_autoload_user_input(void) {}
uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#define SIZE(w, h) ((uint64_t)(h) << 32 | (w))
static void tick(void) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]]; }
static void press(unsigned key) { runtime_input_key_synthetic(key, 1); runtime_input_key_synthetic(key, 0); tick(); }
static void key_event(unsigned short code, BOOL down) {
    NSEvent *e = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0
                                 timestamp:0 windowNumber:window.windowNumber context:nil characters:@"x"
               charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:code];
    if (down) [window.contentView keyDown:e]; else [window.contentView keyUp:e];
    tick();
}
static TOTKSettingRow *row(NSString *title) {
    for (TOTKSettingRow *r in settingsPanel.rows) if ([r.title hasPrefix:title]) return r;
    fprintf(stderr, "no row %s\n", title.UTF8String); abort();
}
static NSPopUpButton *menu(NSString *title) { return (NSPopUpButton *)row(title).control; }
static void pick(NSString *title, NSString *choice) {
    NSPopUpButton *p = menu(title);
    assert(p.enabled && [p indexOfItemWithTitle:choice] >= 0);
    [p selectItemWithTitle:choice]; [row(title) changed:p];
}
static void flip(NSString *title, BOOL on) {
    NSSwitch *s = (NSSwitch *)row(title).control;
    s.state = on ? NSControlStateValueOn : NSControlStateValueOff; [row(title) changed:s];
}
static void shows(NSString *title, NSString *choice) {
    [settingsPanel.timer fire];
    NSString *now = menu(title).titleOfSelectedItem;
    if (![now isEqualToString:choice]) { fprintf(stderr, "%s shows %s, expected %s\n", title.UTF8String, now.UTF8String, choice.UTF8String); abort(); }
}
/* The window's content: that height (or the most the screen's visible area holds), 16:9. */
static void window_is(CGFloat h) {
    NSRect room = [window contentRectForFrameRect:(window.screen ?: NSScreen.mainScreen).visibleFrame];
    CGFloat want = round(fmin(h, fmin(room.size.height, room.size.width * 9 / 16)));
    NSSize now = window.contentView.bounds.size;
    if (fabs(now.height - want) > 1 || fabs(now.width - round(want * 16 / 9)) > 1) {
        fprintf(stderr, "window %.0fx%.0f, expected height %.0f at 16:9\n", now.width, now.height, want); abort();
    }
}
static void shaped(double ratio) {
    NSSize now = window.contentView.bounds.size;
    if (fabs(now.width / now.height - ratio) > 0.01) { fprintf(stderr, "window %.0fx%.0f, expected ratio %.3f\n", now.width, now.height, ratio); abort(); }
}
static BOOL switched(NSString *title) { [settingsPanel.timer fire]; return ((NSSwitch *)row(title).control).state == NSControlStateValueOn; }

int main(int argc, const char **argv) { @autoreleasepool {
    [NSApplication sharedApplication];
    setenv("TOTK_OUTPUT_RES", "2560x1440", 1);   /* the native size the resolution row names */
    settings_test_output_init();
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView = [[TOTKInputView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    if (hud_compact()) { TOTKHudBox *box = [[TOTKHudBox alloc] initWithFrame:NSZeroRect]; [window.contentView addSubview:box]; hudBox = box; }

    /* Key 1 opens it; a held key acts once. */
    runtime_input_key_synthetic(18, 1); runtime_input_key_synthetic(18, 1); tick(); runtime_input_key_synthetic(18, 0); tick();
    assert(settingsOpen && settingsPanel && !settingsPanel.hidden && settingsPanel.superview == window.contentView);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    assert(settingsPanel.rows.count == 8);

    /* Overlay: all -> FPS only -> off; the corner box follows. */
    hud_publish(HUD_FPS, "FPS 60  2560x1440", 0); hud_publish(HUD_SHADER, "New shaders 3 / total 9", 0);
    shows(@"Overlay", @"All (FPS, shaders, load)");
    pick(@"Overlay", @"FPS only"); assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    if (hudBox) { TOTKHudBox *b = (TOTKHudBox *)hudBox; assert(!b.hidden && [b.label.stringValue isEqualToString:@"FPS 60  2560x1440"]); }
    pick(@"Overlay", @"Off"); assert(fps_overlay_hud_mode() == HUD_MODE_OFF && (!hudBox || hudBox.hidden));
    pick(@"Overlay", @"All (FPS, shaders, load)");
    if (hudBox) assert(!hudBox.hidden && [((TOTKHudBox *)hudBox).label.stringValue containsString:@"New shaders"]);

    /* Frame lock: off -> 30 -> 60 -> off. */
    shows(@"Frame lock", @"Off");
    pick(@"Frame lock", @"30 fps"); assert(fakeLock == 30);
    pick(@"Frame lock", @"40 fps"); assert(fakeLock == 40);
    pick(@"Frame lock", @"60 fps"); assert(fakeLock == 60);
    pick(@"Frame lock", @"Off"); assert(fakeLock == 0);
    nvn_fps_lock_set(60); shows(@"Frame lock", @"60 fps"); nvn_fps_lock_set(0);
    /* Frame lock and interpolation exclude each other: picking one turns the other off, both rows show it at once. */
    mfx_set(MFX_30); mfx_latch(); [settingsPanel refresh];
    pick(@"Frame lock", @"60 fps");
    assert(fakeLock == 60 && mfx_requested() == MFX_OFF && [menu(@"Frame interpolation").titleOfSelectedItem isEqualToString:@"Off"]);
    pick(@"Frame interpolation", @"30+30");
    assert(fakeLock == 0 && mfx_requested() == MFX_30 && [menu(@"Frame lock").titleOfSelectedItem isEqualToString:@"Off"]);
    pick(@"Frame interpolation", @"Off"); assert(fakeLock == 0);   /* interpolation off leaves the lock off */
    pick(@"Frame lock", @"30 fps"); pick(@"Frame lock", @"Off"); assert(mfx_requested() == MFX_OFF);

    /* Frame interpolation: the pick shows at once (the game takes it at its next frame); a change elsewhere shows too. */
    mfx_set(MFX_OFF); mfx_latch(); shows(@"Frame interpolation", @"Off");
    pick(@"Frame interpolation", @"30+30"); assert(mfx_requested() == MFX_30); shows(@"Frame interpolation", @"30+30");
    mfx_latch(); assert(mfx_enabled() == MFX_30);
    mfx_set(MFX_MAX); shows(@"Frame interpolation", @"Unlimited");
    pick(@"Frame interpolation", @"40+40"); assert(mfx_requested() == MFX_40);
    pick(@"Frame interpolation", @"50+50"); assert(mfx_requested() == MFX_50);
    assert(menu(@"Frame interpolation").numberOfItems == 5 && menu(@"Frame lock").numberOfItems == 4);
    pick(@"Frame interpolation", @"Off"); mfx_latch(); assert(!mfx_enabled());

    /* Resolution: the player picks; 1080p without MetalFX at the start. */
    mfxup_output_set(0); mfxup_set(0); shows(@"Resolution", @"1080p");
    assert(menu(@"Resolution").numberOfItems == 5 && [menu(@"Resolution").itemTitles.firstObject isEqualToString:@"720p"]);
    pick(@"Resolution", @"720p"); assert(runtime_output_mode() == OUTPUT_MODE_720P && runtime_output_size_current() == SIZE(1280, 720));
    window_is(720);
    pick(@"Resolution", @"1080p + MetalFX upscaling");
    assert(runtime_output_mode() == OUTPUT_MODE_METALFX && mfxup_output_enabled() && runtime_output_size_current() == SIZE(1920, 1080));
    pick(@"Resolution", @"2K (2560×1440)");
    assert(runtime_output_mode() == OUTPUT_MODE_NATIVE && !mfxup_output_enabled() && runtime_output_size_current() == SIZE(2560, 1440));
    window_is(1440);   /* the window takes the picked size at once */
    pick(@"Resolution", @"1080p");
    assert(runtime_output_mode() == OUTPUT_MODE_OFF && !mfxup_output_enabled() && runtime_output_size_current() == SIZE(1920, 1080));
    window_is(1080);
    settings_test_output_size(2560, 1440); shows(@"Resolution", @"2K (2560×1440)");   /* set elsewhere: shown */
    settings_test_output_size(1920, 1080);

    /* Aspect ratio: fixed at 16:9 while the game cannot change it; offered once its code is in (the next opening). */
    assert(!menu(@"Aspect ratio").enabled && [menu(@"Aspect ratio").titleOfSelectedItem isEqualToString:@"16:9"]);
    settings_test_aspect_init(1.6);
    key_event(53, YES); key_event(53, NO); assert(!settingsOpen && settingsPanel.hidden);   /* Esc closes it */
    press(18); assert(settingsOpen);
    /* 16:9, 16:10, 21:9, 32:9 (no fitting the window any more); the window takes each shape at once. */
    assert(menu(@"Aspect ratio").enabled && menu(@"Aspect ratio").numberOfItems == 4 && [menu(@"Aspect ratio") indexOfItemWithTitle:@"Fit to window"] < 0);
    pick(@"Aspect ratio", @"16:10"); assert(fabsf(runtime_aspect_ratio() - 1.6f) < 0.001f && fabsf(runtime_aspect_now() - 1.6f) < 0.001f);
    tick(); shaped(16.0 / 10);
    pick(@"Aspect ratio", @"32:9"); assert(fabsf(runtime_aspect_now() - 32.0f / 9) < 0.001f);
    tick(); shaped(32.0 / 9);
    pick(@"Aspect ratio", @"21:9"); assert(fabsf(runtime_aspect_now() - 21.0f / 9) < 0.001f);
    tick(); shaped(21.0 / 9);
    pick(@"Aspect ratio", @"16:9"); assert(runtime_aspect_now() == 0 && fabsf(runtime_aspect_ratio() - 16.0f / 9) < 0.001f);
    tick(); shaped(16.0 / 9);
    /* 2K + MetalFX for a 4K screen: the 2K canvas, the MetalFX output stage on, a 3840x2160 window (as far as the screen holds). */
    pick(@"Resolution", @"2K + MetalFX upscaling (4K screens)");
    assert(runtime_output_mode() == OUTPUT_MODE_NATIVE_METALFX && runtime_output_size_current() == SIZE(2560, 1440) && mfxup_output_enabled());
    window_is(2160);
    pick(@"Resolution", @"2K (2560×1440)"); assert(runtime_output_mode() == OUTPUT_MODE_NATIVE && !mfxup_output_enabled());
    window_is(1440);
    pick(@"Resolution", @"1080p"); assert(runtime_output_size_current() == SIZE(1920, 1080)); window_is(1080);

    /* Switches. */
    assert(!switched(@"Edge smoothing")); flip(@"Edge smoothing", YES); assert(taa_enabled() && switched(@"Edge smoothing"));
    flip(@"Edge smoothing", NO); assert(!taa_enabled());
    assert(!switched(@"Mouse")); flip(@"Mouse", YES); assert(fakeMouse && switched(@"Mouse")); flip(@"Mouse", NO); assert(!fakeMouse);

    /* Controller type: auto names the controller it sees. */
    shows(@"Controller type", @"Auto (PlayStation)");
    pick(@"Controller type", @"Switch"); assert(fakePadType == GAMEPAD_TYPE_SWITCH);
    pick(@"Controller type", @"Auto (PlayStation)"); assert(fakePadType == GAMEPAD_TYPE_AUTO);
    fakePadDetected = -1; shows(@"Controller type", @"Auto (no controller)");

    /* While it is open the game gets no keys (only key 1 and Esc act); after it closes, keys reach the game again. */
    key_event(38, YES); assert(!keys[38]); key_event(38, NO);
    key_event(18, YES); key_event(18, NO); assert(!settingsOpen);
    key_event(38, YES); assert(keys[38]); key_event(38, NO); assert(!keys[38]);
    /* Keys 2-7 are no shortcuts any more. */
    int interp = mfx_requested(); float shape = runtime_aspect_ratio();
    for (unsigned k = 19; k <= 26; k++) if (k != 24 && k != 25) press(k);
    assert(mfx_requested() == interp && runtime_aspect_ratio() == shape && !settingsOpen && !taa_enabled() && !fakeMouse);

    /* The contribute button with no contribution file (TOTK_SHADER_CONTRIB unset): says so, sends nothing. */
    press(18); assert(settingsOpen);
    unsetenv("TOTK_SHADER_CONTRIB"); [settingsPanel contribute:[NSButton new]];
    assert([settingsPanel.hint.stringValue hasPrefix:@"Nothing to contribute"]);
    press(18); assert(!settingsOpen);

    /* Chinese. */
    setenv("TOTK_UI_LANG", "zh", 1); press(18); assert(settingsOpen);
    shows(@"分辨率", @"1080p"); shows(@"手柄类型", @"自动（未接手柄）");
    fakePadDetected = GAMEPAD_TYPE_SWITCH; shows(@"手柄类型", @"自动（Switch）");
    pick(@"补帧", @"50+50"); assert(mfx_requested() == MFX_50);

    if (argc == 2) {
        NSBitmapImageRep *rep = [settingsPanel bitmapImageRepForCachingDisplayInRect:settingsPanel.bounds];
        [settingsPanel cacheDisplayInRect:settingsPanel.bounds toBitmapImageRep:rep];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES]);
    }
    [settingsPanel.timer invalidate];
    puts("pass: settings panel (key 1 / Esc, overlay, frame lock, interpolation, resolution 1080p / MetalFX / 2K / 4K and window size, aspect 16:9 / 16:10 / 21:9 / 32:9, aspect ratio, switches, controller type, game keys blocked while open, keys 2-7 inert, Chinese)");
    return 0;
}}
