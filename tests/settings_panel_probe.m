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
uint64_t settings_test_scene(void);
int settings_test_picture_restart(uint64_t *size);
int mfxup_enabled(void);

/* gfx/nvn/nvn_mtl.m's flags: vertical sync, and hle_nvn.c's adaptive interpolation (uncapped interpolation, its pacing
   checked by probe-gfx's present-interval probe). */
static int fakeVsync = 1;
void dm_gfx_set_flag(const char *name, int value) {
    if (!strcmp(name, "mfx_interp")) mfx_set(value);
    else if (!strcmp(name, "mfx_auto")) mfx_set(value ? MFX_MAX : MFX_OFF);
    else if (!strcmp(name, "vsync")) fakeVsync = !!value;
    else if (!strcmp(name, "mfx_output")) mfxup_output_set(value);
    else if (!strcmp(name, "mfx_upscale")) mfxup_set(value);
    else abort();
}
int dm_gfx_vsync(void) { return fakeVsync; }
/* controls.txt's controller layout (runtime/gamepad.m), set when the controller sheet applies. */
static int controllerLayouts;
void runtime_controller_layout_set(const ControlsLayout *layout) { (void)layout; controllerLayouts++; }
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
/* runtime/hid_feedback.c's and gamepad_feedback.m's rumble, strength and gyro switches without a controller. */
static int fakeFeedback[2] = {1, 1}, fakeFeedbackLocked[2], fakeFeedbackPresent[2] = {1, 1}, fakeGainLocked;
static float fakeGain = 1;
int runtime_feedback_locked(int gyro) { return fakeFeedbackLocked[gyro]; }
int runtime_feedback_present(int gyro) { return fakeFeedbackPresent[gyro]; }
int runtime_feedback_choice(int gyro) { return fakeFeedback[gyro]; }
void runtime_feedback_choose(int gyro, int on) { fakeFeedback[gyro] = !!on; }
int runtime_rumble_gain_locked(void) { return fakeGainLocked; }
float runtime_rumble_gain(void) { return fakeGain; }
void runtime_rumble_gain_set(float gain) { fakeGain = gain; }
/* gfx/nvn/hle_nvn.c's frame lock (its pacing is checked by probe-gfx's present-interval probe). */
static int fakeLock;
void nvn_fps_lock_set(int fps) { fakeLock = fps >= 20 && fps <= 240 ? fps : 0; }
int nvn_fps_lock(void) { return fakeLock; }
/* runtime/gamepad_feedback.m's DualSense bow trigger switch without a controller. */
static int fakeTriggers = 1, fakeTriggersLocked, fakeTriggersAvailable = 1;
int runtime_triggers_available(void) { return fakeTriggersAvailable; }
int runtime_triggers_locked(void) { return fakeTriggersLocked; }
int runtime_triggers_choice(void) { return fakeTriggers; }
void runtime_triggers_choose(int on) { fakeTriggers = !!on; }
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
    for (TOTKSettingRow *r in gameplaySheet.rows) if (gameplaySheet.panel && [r.title hasPrefix:title]) return r;
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
    [settingsPanel.timer fire]; [gameplaySheet refresh];
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
/* The gameplay sheet made again (its rows are made at each opening, as the panel's). */
static void gameplay(void) {
    if (gameplaySheet.panel) { [gameplaySheet done:nil]; tick(); }
    settings_open_controls(2); tick(); assert(gameplaySheet.panel && window.attachedSheet == gameplaySheet.panel);
}
static NSString *note(void) {   /* the line beside the interpolation switch */
    for (TOTKSettingRow *r in settingsPanel.rows) if (r.note) { [r refresh]; return ((NSTextField *)r.control).stringValue; }
    abort();
}
static BOOL switched(NSString *title) { [settingsPanel.timer fire]; [gameplaySheet refresh]; return ((NSSwitch *)row(title).control).state == NSControlStateValueOn; }

static void dump(NSView *view, const char *path, NSString *suffix) {
    NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds]; [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[@(path) stringByAppendingString:suffix] atomically:YES]);
}
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
    assert(settingsPanel.rows.count == 8);   /* overlay, limit | vsync, resolution | window mode, aspect, MetalFX, controls */

    /* Overlay: all -> FPS only -> off; the corner box follows. */
    hud_publish(HUD_FPS, "FPS 60  2560x1440", 0); hud_publish(HUD_SHADER, "New shaders 3 / total 9", 0);
    shows(@"Overlay", @"All (FPS, shaders, load)");
    pick(@"Overlay", @"FPS only"); assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    if (hudBox) { TOTKHudBox *b = (TOTKHudBox *)hudBox; assert(!b.hidden && [b.label.stringValue isEqualToString:@"FPS 60  2560x1440"]); }
    pick(@"Overlay", @"Off"); assert(fps_overlay_hud_mode() == HUD_MODE_OFF && (!hudBox || hudBox.hidden));
    pick(@"Overlay", @"All (FPS, shaders, load)");
    if (hudBox) assert(!hudBox.hidden && [((TOTKHudBox *)hudBox).label.stringValue containsString:@"New shaders"]);

    /* Frame rate limit: off / 30 / 40 / 60 and a rate typed in (20..240), shown as the last choice. */
    shows(@"Frame rate limit", @"Off");
    pick(@"Frame rate limit", @"30 fps"); assert(fakeLock == 30);
    pick(@"Frame rate limit", @"40 fps"); assert(fakeLock == 40);
    pick(@"Frame rate limit", @"60 fps"); assert(fakeLock == 60);
    pick(@"Frame rate limit", @"Off"); assert(fakeLock == 0);
    assert(menu(@"Frame rate limit").numberOfItems == 5);
    assert(settings_fps_parse(@"72") == 72 && settings_fps_parse(@"20") == 20 && settings_fps_parse(@"240") == 240);
    assert(!settings_fps_parse(@"19") && !settings_fps_parse(@"241") && !settings_fps_parse(@"60.5") && !settings_fps_parse(@"") && !settings_fps_parse(@"6O"));
    nvn_fps_lock_set(72); shows(@"Frame rate limit", @"Custom (72 fps)");
    nvn_fps_lock_set(60); shows(@"Frame rate limit", @"60 fps");
    assert([[menu(@"Frame rate limit") itemAtIndex:4].title isEqualToString:@"Custom…"]); nvn_fps_lock_set(0);

    /* Vertical sync: a switch, on at the start (TOTK_MFX's interpolation off first: it keeps vsync on). */
    mfx_set(MFX_OFF); mfx_latch();
    assert(switched(@"Vertical sync") && menu(@"Vertical sync").enabled);
    flip(@"Vertical sync", NO); assert(!fakeVsync && !switched(@"Vertical sync")); flip(@"Vertical sync", YES); assert(fakeVsync);

    /* The MetalFX section: closed at first; opening it makes the panel again with its two lines. */
    assert([((NSButton *)row(@"MetalFX settings").control).title isEqualToString:@"Show ▸"]);
    [row(@"MetalFX settings") changed:row(@"MetalFX settings").control]; tick();
    assert(settingsOpen && settingsPanel.rows.count == 12 && NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    assert([((NSButton *)row(@"MetalFX settings").control).title isEqualToString:@"Hide ▾"]);

    /* Frame interpolation: one switch, adaptive. Without a limit at half the display's rate; with one at the limit
       (N + N); vertical sync stays on (greyed out) while it runs. The limit no longer turns it off. */
    mfx_set(MFX_OFF); mfx_latch(); assert(!switched(@"Frame interpolation"));
    assert([note() hasPrefix:@"Off"]);
    flip(@"Frame interpolation", YES); assert(mfx_requested() == MFX_MAX && switched(@"Frame interpolation"));
    assert([note() hasPrefix:@"Adaptive: up to "]);
    flip(@"Vertical sync", NO); assert(!fakeVsync);   /* stays asked for, shown on while interpolating */
    assert(switched(@"Vertical sync") && !menu(@"Vertical sync").enabled);
    pick(@"Frame rate limit", @"40 fps"); assert(fakeLock == 40 && mfx_requested() == MFX_MAX);
    assert([note() hasPrefix:@"At the limit: 40 + 40 = 80 fps"]);
    nvn_fps_lock_set(45); assert([note() hasPrefix:@"At the limit: 45 + 45 = 90 fps"]);
    assert([note() hasSuffix:@"screen)"] == (90 > settings_display_hz()));   /* more than the screen shows: said so */
    nvn_fps_lock_set(240); assert(([note() hasSuffix:[NSString stringWithFormat:@"(above the %d Hz screen)", settings_display_hz()]]));
    nvn_fps_lock_set(45);
    flip(@"Frame interpolation", NO); assert(mfx_requested() == MFX_OFF && fakeLock == 45);
    assert(!switched(@"Vertical sync") && menu(@"Vertical sync").enabled);
    flip(@"Vertical sync", YES); pick(@"Frame rate limit", @"Off"); mfx_latch();

    /* Resolution: 720p / 900p / 1080p / 1440p up to the swapchain; 1080p at the start; the window takes the size. */
    mfxup_output_set(0); mfxup_set(0); shows(@"Resolution", @"1080p");
    assert(menu(@"Resolution").numberOfItems == 4 && [menu(@"Resolution").itemTitles isEqualToArray:(@[@"720p", @"900p", @"1080p", @"1440p"])]);
    pick(@"Resolution", @"720p"); assert(runtime_output_size_current() == SIZE(1280, 720)); window_is(720);
    pick(@"Resolution", @"900p"); assert(runtime_output_size_current() == SIZE(1600, 900)); window_is(900);
    pick(@"Resolution", @"1440p"); assert(runtime_output_size_current() == SIZE(2560, 1440) && !mfxup_output_enabled()); window_is(1440);
    pick(@"Resolution", @"1080p"); assert(runtime_output_size_current() == SIZE(1920, 1080)); window_is(1080);
    settings_test_output_size(2560, 1440); shows(@"Resolution", @"1440p");   /* set elsewhere: shown */
    settings_test_output_size(1920, 1080);

    /* MetalFX spatial presets (1.4.3): the scene below the canvas, MetalFX rebuilding it; kept for the next start.
       Anti-aliasing only with a preset on. Elsewhere the preset is fixed off and anti-aliasing stays free. */
    if (runtime_picture_presets()) {
        char kept[] = "/tmp/totk-picture-XXXXXX"; int fd = mkstemp(kept); assert(fd >= 0); close(fd); setenv("TOTK_PICTURE_MODE", kept, 1);
        shows(@"MetalFX upscaling", @"Off"); assert(!menu(@"Anti-aliasing").enabled && !taa_enabled());
        pick(@"MetalFX upscaling", @"Balanced (~59%)");
        assert(runtime_picture_preset() == PICTURE_PRESET_BALANCED && mfxup_enabled() && !mfxup_output_enabled() && settings_test_scene() == SIZE(1130, 636));
        assert(menu(@"Anti-aliasing").enabled);
        pick(@"Anti-aliasing", @"MetalFX TAA"); assert(taa_enabled());
        pick(@"Resolution", @"900p"); assert(runtime_output_size_current() == SIZE(1600, 900) && settings_test_scene() == SIZE(942, 530));
        uint64_t size = 0; assert(settings_test_picture_restart(&size) == PICTURE_PRESET_BALANCED && size == SIZE(1600, 900));
        pick(@"MetalFX upscaling", @"Quality (~67%)"); assert(settings_test_scene() == SIZE(1066, 600));
        pick(@"MetalFX upscaling", @"Performance (50%)"); assert(settings_test_scene() == SIZE(800, 450));
        pick(@"MetalFX upscaling", @"Off"); assert(!runtime_picture_preset() && !mfxup_enabled() && !settings_test_scene() && !taa_enabled());
        assert(!menu(@"Anti-aliasing").enabled);
        assert(settings_test_picture_restart(&size) == PICTURE_PRESET_OFF && size == SIZE(1600, 900));
        pick(@"Resolution", @"1080p"); assert(settings_test_picture_restart(&size) == PICTURE_PRESET_OFF && size == SIZE(1920, 1080));
        unlink(kept); unsetenv("TOTK_PICTURE_MODE");
    } else {
        assert(!menu(@"MetalFX upscaling").enabled && [menu(@"MetalFX upscaling").titleOfSelectedItem isEqualToString:@"Off (1.4.3 only)"]);
        assert(menu(@"Anti-aliasing").enabled);
        pick(@"Anti-aliasing", @"MetalFX TAA"); assert(taa_enabled()); pick(@"Anti-aliasing", @"FXAA (the game's)"); assert(!taa_enabled());
    }

    /* Window mode: borderless fills the screen without the title bar; a window again comes back to its frame. */
    shows(@"Window mode", @"Window");
    NSRect framed = window.frame;
    pick(@"Window mode", @"Borderless"); assert(windowBorderless && !(window.styleMask & NSWindowStyleMaskTitled));
    assert(NSEqualRects(window.frame, (window.screen ?: NSScreen.mainScreen).frame)); shows(@"Window mode", @"Borderless");
    pick(@"Window mode", @"Window"); assert(!windowBorderless && (window.styleMask & NSWindowStyleMaskTitled) && NSEqualRects(window.frame, framed));

    /* Aspect ratio: fixed at 16:9 while the game cannot change it; offered once its code is in (the next opening). */
    assert(!menu(@"Aspect ratio").enabled && [menu(@"Aspect ratio").titleOfSelectedItem isEqualToString:@"16:9"]);
    settings_test_aspect_init(1.6);
    key_event(53, YES); key_event(53, NO); assert(!settingsOpen && settingsPanel.hidden);   /* Esc closes it */
    press(18); assert(settingsOpen && settingsPanel.rows.count == 12);   /* the section stays open for this run */
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
    pick(@"Resolution", @"1440p"); window_is(1440);
    pick(@"Resolution", @"1080p"); assert(runtime_output_size_current() == SIZE(1920, 1080)); window_is(1080);

    /* The controls line: the gameplay sheet holds main's controller switches. */
    gameplay();
    assert(!switched(@"Mouse")); flip(@"Mouse", YES); assert(fakeMouse && switched(@"Mouse")); flip(@"Mouse", NO); assert(!fakeMouse);

    /* The DualSense bow trigger: a switch; greyed out while TOTK_DUALSENSE_TRIGGERS or a test session decides, and on a
       game version without the bow's state (rows are made at each opening). */
    assert(switched(@"DualSense bow trigger")); flip(@"DualSense bow trigger", NO); assert(!fakeTriggers && !switched(@"DualSense bow trigger"));
    flip(@"DualSense bow trigger", YES); assert(fakeTriggers);
    fakeTriggersLocked = 1; fakeTriggers = 0; gameplay();
    assert(!menu(@"DualSense bow trigger").enabled && [menu(@"DualSense bow trigger").titleOfSelectedItem isEqualToString:@"Off"]);
    fakeTriggersLocked = 0; fakeTriggersAvailable = 0; gameplay();
    assert(!menu(@"DualSense bow trigger").enabled && [menu(@"DualSense bow trigger").titleOfSelectedItem isEqualToString:@"Not in this version"]);
    fakeTriggersAvailable = 1; fakeTriggers = 1; gameplay(); assert(switched(@"DualSense bow trigger"));

    /* Rumble, its strength and gyro aiming. */
    assert(switched(@"Rumble")); flip(@"Rumble", NO); assert(!fakeFeedback[0] && !switched(@"Rumble")); flip(@"Rumble", YES); assert(fakeFeedback[0]);
    shows(@"Rumble strength", @"1× (default)");
    pick(@"Rumble strength", @"2×"); assert(fakeGain == 2); shows(@"Rumble strength", @"2×");
    pick(@"Rumble strength", @"0.5×"); assert(fakeGain == 0.5f); pick(@"Rumble strength", @"1× (default)"); assert(fakeGain == 1);
    assert(switched(@"Gyro aiming")); flip(@"Gyro aiming", NO); assert(!fakeFeedback[1]); flip(@"Gyro aiming", YES); assert(fakeFeedback[1]);
    /* Rows are made at each opening: a variable that decides (greyed out), a device the run started without (the next
       launch), a strength the file holds that is not a choice. */
    fakeFeedbackLocked[1] = 1; fakeFeedbackPresent[0] = 0; fakeGain = 2.5f; gameplay();
    assert(!menu(@"Gyro aiming").enabled && [menu(@"Gyro aiming").titleOfSelectedItem isEqualToString:@"On"]);
    (void)row(@"Rumble (from the next launch)"); flip(@"Rumble (from the next launch)", NO); assert(!fakeFeedback[0]);
    shows(@"Rumble strength", @"2.5×"); pick(@"Rumble strength", @"3×"); assert(fakeGain == 3);
    fakeGainLocked = 1; gameplay(); assert(!menu(@"Rumble strength").enabled && [menu(@"Rumble strength").titleOfSelectedItem isEqualToString:@"3×"]);
    fakeFeedbackLocked[1] = 0; fakeFeedbackPresent[0] = 1; fakeFeedback[0] = 1; fakeGainLocked = 0; fakeGain = 1; gameplay();
    assert(switched(@"Rumble") && menu(@"Rumble strength").enabled);
    if (argc == 2) {
        NSView *sheet = gameplaySheet.panel.contentView; NSBitmapImageRep *rep = [sheet bitmapImageRepForCachingDisplayInRect:sheet.bounds];
        [sheet cacheDisplayInRect:sheet.bounds toBitmapImageRep:rep];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[@(argv[1]) stringByAppendingString:@".gameplay.png"] atomically:YES]);
    }
    [gameplaySheet done:nil]; tick(); assert(!gameplaySheet.panel && !window.attachedSheet && settingsOpen);

    /* The controller sheet: the controller type (auto names the controller it sees), set on Apply with controls.txt. */
    char folder[] = "/tmp/totk-controls-XXXXXX"; assert(mkdtemp(folder));
    NSString *controls = [@(folder) stringByAppendingPathComponent:@"controls.txt"]; setenv("TOTK_CONTROLS", controls.fileSystemRepresentation, 1);
    settings_open_controls(0); tick(); assert(controlsEditor.panel && controlsEditor.padType);
    if (argc == 2) {
        dump(controlsEditor.panel.contentView, argv[1], @".controller.png");
        if (controlsEditor.pages.count > 1) { for (NSScrollView *page in controlsEditor.pages) page.hidden = page != controlsEditor.pages.lastObject;
            dump(controlsEditor.panel.contentView, argv[1], @".actions.png"); }
    }
    assert([controlsEditor.padType.titleOfSelectedItem isEqualToString:@"Auto (PlayStation)"]);
    [controlsEditor.padType selectItemWithTitle:@"Switch"]; [controlsEditor apply:nil]; tick();
    assert(fakePadType == GAMEPAD_TYPE_SWITCH && controllerLayouts == 1 && !controlsEditor.panel && !window.attachedSheet);
    settings_open_controls(0); tick(); assert([controlsEditor.padType.titleOfSelectedItem isEqualToString:@"Switch"]);
    [controlsEditor.padType selectItemAtIndex:0]; [controlsEditor cancel:nil]; tick(); assert(fakePadType == GAMEPAD_TYPE_SWITCH);   /* Cancel keeps it */
    settings_open_controls(0); tick(); [controlsEditor.padType selectItemAtIndex:0]; [controlsEditor apply:nil]; tick(); assert(fakePadType == GAMEPAD_TYPE_AUTO);
    /* The keyboard sheet: keys and the mouse's buttons, no controller type. */
    /* Its player actions page (where the version has them): untouched, Apply writes no player-actions.txt. */
    NSString *actions = [@(folder) stringByAppendingPathComponent:@"player-actions.txt"];
    assert(![[NSFileManager defaultManager] fileExistsAtPath:actions]);
    settings_open_controls(1); tick(); assert(controlsEditor.panel && !controlsEditor.padType && [controlsEditor.panel.title isEqualToString:@"Keyboard and Mouse"]);
    if (argc == 2) dump(controlsEditor.panel.contentView, argv[1], @".keyboard.png");
    [controlsEditor cancel:nil]; tick();
    [[NSFileManager defaultManager] removeItemAtPath:@(folder) error:nil]; unsetenv("TOTK_CONTROLS");
    fakePadDetected = -1;

    /* A short window (32:9 on a laptop screen): the lines close up, so the title, the close button and the first line stay in it. */
    NSSize was = window.contentView.bounds.size; press(18); [window setContentSize:NSMakeSize(1728, 486)]; press(18); assert(settingsOpen);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    press(18); [window setContentSize:was]; press(18); assert(settingsOpen);

    /* While it is open the game gets no keys (only key 1 and Esc act); after it closes, keys reach the game again. */
    key_event(38, YES); assert(!keys[38]); key_event(38, NO);
    key_event(18, YES); key_event(18, NO); assert(!settingsOpen);
    key_event(38, YES); assert(keys[38]); key_event(38, NO); assert(!keys[38]);
    /* Keys 2-7 are no shortcuts any more. */
    int interp = mfx_requested(); float shape = runtime_aspect_ratio();
    for (unsigned k = 19; k <= 26; k++) if (k != 24 && k != 25) press(k);
    assert(mfx_requested() == interp && runtime_aspect_ratio() == shape && !settingsOpen && !taa_enabled() && !fakeMouse && fakeVsync);

    /* The contribute button with no contribution file (TOTK_SHADER_CONTRIB unset): says so, sends nothing. */
    press(18); assert(settingsOpen);
    unsetenv("TOTK_SHADER_CONTRIB"); [settingsPanel contribute:[NSButton new]];
    assert([settingsPanel.hint.stringValue hasPrefix:@"Nothing to contribute"]);
    press(18); assert(!settingsOpen);

    /* Chinese. */
    setenv("TOTK_UI_LANG", "zh", 1); press(18); assert(settingsOpen);
    shows(@"分辨率", @"1080p"); shows(@"帧率上限", @"关"); shows(@"窗口模式", @"窗口");
    flip(@"补帧（自适应）", YES); assert(mfx_requested() == MFX_MAX && [note() hasPrefix:@"自适应：最高 "]);
    nvn_fps_lock_set(50); assert([note() hasPrefix:@"按帧率上限补：50 + 50 = 100 帧"]); shows(@"帧率上限", @"自定义（50 帧）");
    flip(@"补帧（自适应）", NO); nvn_fps_lock_set(0);
    gameplay(); assert(switched(@"DualSense 拉弓扳机")); shows(@"震动强度", @"1×（默认）"); assert(switched(@"震动") && switched(@"体感瞄准"));
    [gameplaySheet done:nil]; tick();

    if (argc == 2) {
        NSBitmapImageRep *rep = [settingsPanel bitmapImageRepForCachingDisplayInRect:settingsPanel.bounds];
        [settingsPanel cacheDisplayInRect:settingsPanel.bounds toBitmapImageRep:rep];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES]);
    }
    [settingsPanel.timer invalidate];
    puts("pass: settings panel (key 1 / Esc, overlay, frame rate limit and custom, vertical sync, the MetalFX section, adaptive interpolation, resolution 720p / 900p / 1080p / 1440p and window size, MetalFX presets and anti-aliasing, window mode, aspect 16:9 / 16:10 / 21:9 / 32:9, the gameplay sheet, the controller and keyboard sheets, rumble / rumble strength / gyro aiming, game keys blocked while open, keys 2-7 inert, Chinese)");
    return 0;
}}
