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
void settings_test_aspect_frame(void);

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
int runtime_gamepad_battery_percent(void) { return -1; }
int runtime_mouse_set(int on) { return fakeMouse = !!on; }
int runtime_mouse_enabled(void) { return fakeMouse; }
void runtime_mouse_set_hook(void (*hook)(void)) { (void)hook; }
void runtime_mouse_move(double dx, double dy) { (void)dx; (void)dy; }
void runtime_mouse_button(int button, int down) { (void)button; (void)down; }
void runtime_mouse_release(void) {}
static int fakeSensitivity = 50, fakeSensitivityLocked;
int runtime_mouse_sensitivity(void) { return fakeSensitivity; }
int runtime_mouse_sensitivity_locked(void) { return fakeSensitivityLocked; }
void runtime_mouse_sensitivity_set(int sensitivity) { fakeSensitivity = sensitivity; }
void runtime_gamepad_clear(void) {}
static char savedName[64], savedWord[16];
void runtime_setting_save(const char *name, const char *word, const char *what) {
    (void)what; snprintf(savedName, sizeof savedName, "%s", name); snprintf(savedWord, sizeof savedWord, "%s", word);
    const char *dir = getenv("SETTINGS_TEST_DIR"); if (!dir) return;
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w"); assert(f); assert(fprintf(f, "%s\n", word) > 0); assert(!fclose(f));
}
int runtime_setting_word(const char *name, char *word, size_t size) {
    const char *dir = getenv("SETTINGS_TEST_DIR"); if (!dir) return 0;
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name); FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(word, 1, size - 1, f); word[n] = 0; fclose(f); return 1;
}
static int sharedSaves;   /* saves of a setting both games share (runtime_shared_setting_save) */
void runtime_shared_setting_save(const char *name, const char *word, const char *what) { sharedSaves++; runtime_setting_save(name, word, what); }
int runtime_shared_setting_word(const char *name, char *word, size_t size) { return runtime_setting_word(name, word, size); }
static char removedName[64];
void runtime_setting_remove(const char *name) { snprintf(removedName, sizeof removedName, "%s", name); }
/* runtime/gamepad.m's controller layout and mouse sensitivity, kept here. */
static uint16_t fakeMap[CONTROLLER_BUTTONS]; static uint8_t fakeModes[CONTROLLER_BUTTONS];
void runtime_controller_map_get(uint16_t *map, uint8_t *modes) { memcpy(map, fakeMap, sizeof fakeMap); memcpy(modes, fakeModes, sizeof fakeModes); }
void runtime_controller_map_set(const uint16_t *map, const uint8_t *modes) { memcpy(fakeMap, map, sizeof fakeMap); memcpy(fakeModes, modes, sizeof fakeModes); }
void runtime_controller_map_reset(void) { controller_map_default(fakeMap, fakeModes); }
static int fakeSettingsButton = -1;   /* gamepad.m's settings panel button (-1 L3 + R3) */
int runtime_gamepad_settings_button(void) { return fakeSettingsButton; }
void runtime_gamepad_settings_button_set(int physical) { fakeSettingsButton = physical < 0 ? -1 : physical; }
static int fakeSettingsShortcut = 1;   /* off while a controller button is being bound */
void runtime_gamepad_settings_enable(int on) { fakeSettingsShortcut = on; }
static int fakeSettingsCapture;
void runtime_gamepad_text_capture(int on) { fakeSettingsCapture = !!on; }
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
void nvn_fps_lock_set(int fps) { if (totk_fps_limit_valid(fps)) fakeLock = fps; }
int nvn_fps_lock(void) { return fakeLock; }
/* runtime/gamepad_feedback.m's DualSense bow trigger switch without a controller. */
static int fakeTriggers = 1, fakeTriggersLocked;
#ifndef GAME_BOTW160
static int fakeTriggersAvailable = 1;
#endif
int runtime_triggers_available(void) {
#ifdef GAME_BOTW160
    return 0;   /* This game version has no bow-state reader. */
#else
    return fakeTriggersAvailable;
#endif
}
int runtime_triggers_locked(void) { return fakeTriggersLocked; }
int runtime_triggers_choice(void) { return fakeTriggers; }
void runtime_triggers_choose(int on) { fakeTriggers = !!on; }
void runtime_triggers_release(void) {}
/* A window key is the user's input (runtime/input.c user_input): it stops a survey and an auto-load, neither here. */
void runtime_survey_cancel_user(void) {}
void runtime_autoload_user_input(void) {}
uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#define SIZE(w, h) ((uint64_t)(h) << 32 | (w))
static void tick(void) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]]; }
/* Key 1 toggles the panel on the main queue; a run loop pass may end before it is served (right after a key binding was
   captured), so two. */
static void press(unsigned key) { runtime_input_key_synthetic(key, 1); runtime_input_key_synthetic(key, 0); tick(); tick(); }
static void key_event(unsigned short code, BOOL down) {
    NSEvent *e = [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0
                                 timestamp:0 windowNumber:window.windowNumber context:nil characters:@"x"
               charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:code];
    if (down) [window.contentView keyDown:e]; else [window.contentView keyUp:e];
    tick();
}
static TOTKSettingRow *row(NSString *title) {
    for (TOTKSettingRow *r in settingsPanel.allRows) if ([r.title hasPrefix:title]) return r;
    fprintf(stderr, "no row %s\n", title.UTF8String); abort();
}
static NSPopUpButton *menu(NSString *title) { return (NSPopUpButton *)row(title).control; }
static void pick(NSString *title, NSString *choice) {
    NSPopUpButton *p = menu(title);
    assert(p.enabled && [p indexOfItemWithTitle:choice] >= 0);
    [p selectItemWithTitle:choice]; [row(title) changed:p]; settings_test_aspect_frame();
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
static NSEvent *key_down(unsigned short code, NSEventModifierFlags flags) {
    return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0 windowNumber:window.windowNumber
                             context:nil characters:@"x" charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:code];
}
static TOTKBindingButton *box(int action, int slot) {
    for (TOTKBindingButton *b in settingsPanel.bindingButtons) if (b.gameAction == action && b.slot == slot) return b;
    fprintf(stderr, "no box %d/%d\n", action, slot); abort();
}
static BOOL titled(NSButton *b, NSString *text) {
    if ([b.title isEqualToString:text]) return YES;
    fprintf(stderr, "box shows %s, expected %s\n", b.title.UTF8String, text.UTF8String); return NO;
}
static BOOL switched(NSString *title) { [settingsPanel.timer fire]; return ((NSSwitch *)row(title).control).state == NSControlStateValueOn; }

static void persistence_probe(const char *action) {
#ifdef GAME_BOTW190
    const int outputMode = OUTPUT_MODE_METALFX;   /* 1.9.0 has no native 2K canvas mode. */
    NSString *outputChoice = @"1080p + MetalFX upscaling";
#else
    const int outputMode = OUTPUT_MODE_NATIVE;
    NSString *outputChoice = @"2K (2560×1440)";
#endif
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView = [[TOTKInputView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    if (!strcmp(action, "save")) {
        settings_test_output_init(); settings_test_aspect_init(16.0 / 9); settingsPanel = [TOTKSettingsPanel new];
        pick(@"Overlay", @"FPS + playtime"); pick(@"Frame interpolation", @"40+40");
        pick(@"Resolution", outputChoice); pick(@"Aspect ratio", @"21:9"); flip(@"Edge smoothing", YES);
        assert(fakeLock == 0 && mfx_requested() == MFX_40); return;
    }
    settings_restore_begin();
    if (!strcmp(action, "corrupt")) {
        assert(settingsPendingOutput == -1 && settingsPendingAspect == 0 && fakeLock == 0 && mfx_requested() == MFX_OFF);
        assert(fps_overlay_hud_mode() == HUD_MODE_ALL && !taa_enabled()); return;
    }
    assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    if (!strcmp(action, "override")) {
        assert(settingsPendingOutput == -1 && settingsPendingAspect == 0 && mfx_requested() == MFX_MAX && !taa_enabled()); return;
    }
    if (!strcmp(action, "lock")) { assert(fakeLock == 60 && mfx_requested() == MFX_OFF); return; }
    assert(fakeLock == 0 && mfx_requested() == MFX_40 && taa_enabled());
    assert(settingsPendingOutput == outputMode && settingsPendingAspect > 2.3f);
    settings_restore_ready(); assert(settingsPendingOutput == outputMode && settingsPendingAspect > 2.3f);
    settings_test_output_init(); settings_test_aspect_init(16.0 / 9); settings_restore_ready();
    assert(settingsPendingOutput == -1 && settingsPendingAspect == 0);
    assert(runtime_output_mode() == outputMode && fabsf(runtime_aspect_ratio() - 21.0f / 9) < 0.001f);
}

/* Only row models and settings functions: no NSApplication, window or GPU. */
static TOTKSettingRow *frame_row(NSString *title) {
    for (TOTKSettingRow *r in settings_rows()) if ([r.title isEqualToString:title]) return r;
    abort();
}
static void frame_probe(void) {
    assert(getenv("SETTINGS_TEST_DIR"));
    unsetenv("TOTK_MFX");
    TOTKSettingRow *lock=frame_row(@"Frame lock"), *interp=frame_row(@"Frame interpolation (MetalFX)");
    assert(([lock.values isEqualToArray:@[@0,@30,@40,@60,@72,@90,@120]] && lock.set));
    assert(fakeLock==0); settings_restore_begin(); assert(fakeLock==0);
    const int rates[]={0,30,40,60,72,90,120};
    for(unsigned i=0;i<sizeof rates/sizeof *rates;i++){
        int fps=rates[i]; char expected[32];
        if(fps)snprintf(expected,sizeof expected,"lock-%d",fps);else strcpy(expected,"off");
        mfx_set(MFX_OFF); lock.set(fps);
        assert(fakeLock==fps && mfx_requested()==MFX_OFF);
        assert(!strcmp(savedName,"graphics-frame.txt") && !strcmp(savedWord,expected));
        fakeLock=0; settings_restore_begin(); assert(fakeLock==fps && mfx_requested()==MFX_OFF);
    }
    lock.set(72); interp.set(MFX_40);
    assert(fakeLock==0 && mfx_requested()==MFX_40 && !strcmp(savedWord,"interp-2"));
    mfx_set(MFX_OFF); settings_restore_begin(); assert(fakeLock==0 && mfx_requested()==MFX_40);
    lock.set(90); assert(mfx_requested()==MFX_OFF && !strcmp(savedWord,"lock-90"));
    interp.set(MFX_30); interp.set(MFX_OFF);
    assert(fakeLock==0 && !strcmp(savedWord,"off"));
    lock.set(120);
    for(unsigned i=0;i<2;i++){
        setenv("TOTK_MFX",i?"interpmax":"0",1);fakeLock=0;mfx_set(i?MFX_MAX:MFX_OFF);
        settings_restore_begin();
        assert(fakeLock==0 && mfx_requested()==(i?MFX_MAX:MFX_OFF));
        assert(!frame_row(@"Frame lock").set);
        char word[32]; assert(runtime_setting_word("graphics-frame.txt",word,sizeof word) && !strcmp(word,"lock-120\n"));
    }
    unsetenv("TOTK_MFX");mfx_set(MFX_OFF);
    const char *bad[]={"lock-0","lock-73","lock--1","lock-90junk","lock-120 trailing","invalid"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){
        runtime_setting_save("graphics-frame.txt",bad[i],"test");fakeLock=0;settings_restore_begin();assert(fakeLock==0);
    }
    runtime_setting_save("graphics-frame.txt","lock-72  ","test");settings_restore_begin();assert(fakeLock==72);
    runtime_input_key_synthetic(25,1);runtime_input_key_synthetic(25,0);assert(fakeLock==72); /* key 9 is unassigned */
    NSString *path=[@(getenv("SETTINGS_TEST_DIR")) stringByAppendingPathComponent:@"graphics-frame.txt"];
    assert([NSFileManager.defaultManager removeItemAtPath:path error:nil]);fakeLock=0;settings_restore_begin();assert(fakeLock==0);
    unsetenv("SETTINGS_TEST_DIR");lock.set(90); /* no directory: no player file is written */
    puts("pass: frame lock choices, shared persistence, interpolation, overrides, invalid/deleted files, key 9 inert");
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc==2 && !strcmp(argv[1],"frame")) { frame_probe(); return 0; }
    [NSApplication sharedApplication];
    controller_map_default(fakeMap, fakeModes); setenv("TOTK_UI_LANG", "en", 1); unsetenv("TOTK_SOURCE_COMMIT");
    if (argc == 2) { persistence_probe(argv[1]); puts("pass: settings persistence"); return 0; }
    unsetenv("TOTK_MFX"); /* priority/disabled row covered by the headless frame probe */
    setenv("TOTK_OUTPUT_RES", "2560x1440", 1);   /* the native size the resolution row names */
    settings_test_output_init();
    // In-memory fixture only: do not start the timer or touch a player's profile.
    playtime=[[TOTKPlaytime alloc] initWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView = [[TOTKInputView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    if (hud_compact()) { TOTKHudBox *box = [[TOTKHudBox alloc] initWithFrame:NSZeroRect]; [window.contentView addSubview:box]; hudBox = box; }

    /* Key 1 opens it; a held key acts once. */
    runtime_input_key_synthetic(18, 1); runtime_input_key_synthetic(18, 1); tick(); runtime_input_key_synthetic(18, 0); tick();
    assert(settingsOpen && fakeSettingsCapture && settingsPanel && !settingsPanel.hidden && settingsPanel.superview == window.contentView);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    assert(settingsPanel.allRows.count == 14);

    /* FPS graph: a switch (here off at start; frontend.m turns it on when nothing is saved), kept in fps-graph.txt
       that both games share (user 2026-10-29). */
    assert(!switched(@"FPS graph"));
    int shared0 = sharedSaves;
    flip(@"FPS graph", YES); assert(fpsGraphEnabled && !strcmp(savedName, "fps-graph.txt") && !strcmp(savedWord, "on") && sharedSaves == shared0 + 1);
    flip(@"FPS graph", NO); assert(!fpsGraphEnabled && !strcmp(savedWord, "off") && sharedSaves == shared0 + 2);

    /* Overlay: all -> FPS + playtime -> off; the corner box follows. */
    hud_publish(HUD_FPS, "FPS 60  2560x1440", 0); hud_publish(HUD_SHADER, "New shaders 3 / total 9", 0);
    shows(@"Overlay", @"All (FPS, shaders, load, playtime)");
    pick(@"Overlay", @"FPS + playtime"); assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    if (hudBox) { TOTKHudBox *b = (TOTKHudBox *)hudBox; assert(!b.hidden && [b.label.stringValue isEqualToString:@"FPS 60  2560x1440\nPlaytime  0 h 00 m"]); }
    pick(@"Overlay", @"Off"); assert(fps_overlay_hud_mode() == HUD_MODE_OFF && (!hudBox || hudBox.hidden));
    pick(@"Overlay", @"All (FPS, shaders, load, playtime)");
    if (hudBox) assert(!hudBox.hidden && [((TOTKHudBox *)hudBox).label.stringValue containsString:@"New shaders"]);

    if (hudBox) assert([((TOTKHudBox *)hudBox).label.stringValue containsString:@"Playtime  0 h 00 m"]);

    /* Frame lock: off -> 30 -> 60 -> off. */
    shows(@"Frame lock", @"Off");
    pick(@"Frame lock", @"30 fps"); assert(fakeLock == 30);
    pick(@"Frame lock", @"40 fps"); assert(fakeLock == 40);
    pick(@"Frame lock", @"60 fps"); assert(fakeLock == 60);
    pick(@"Frame lock", @"72 fps"); assert(fakeLock == 72);
    pick(@"Frame lock", @"90 fps"); assert(fakeLock == 90);
    pick(@"Frame lock", @"120 fps"); assert(fakeLock == 120);
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
    assert(menu(@"Frame interpolation").numberOfItems == 5 && menu(@"Frame lock").numberOfItems == 7);
    pick(@"Frame interpolation", @"Off"); mfx_latch(); assert(!mfx_enabled());

    /* Resolution: the player picks; 1080p without MetalFX at the start. */
    mfxup_output_set(0); mfxup_set(0); shows(@"Resolution", @"1080p");
    BOOL nativeResolution = !!(runtime_output_modes() & (1 << OUTPUT_MODE_NATIVE));
    if (nativeResolution) {
    assert(menu(@"Resolution").numberOfItems == 5 && [menu(@"Resolution").itemTitles.firstObject isEqualToString:@"720p"]);
    pick(@"Resolution", @"720p"); assert(runtime_output_mode() == OUTPUT_MODE_720P && runtime_output_size_current() == SIZE(1280, 720));
    window_is(720);
    } else {
        assert(menu(@"Resolution").numberOfItems == 2);
        assert([menu(@"Resolution") indexOfItemWithTitle:@"720p"] < 0);
        assert(!runtime_output_mode_set(OUTPUT_MODE_720P) && !runtime_output_mode_set(OUTPUT_MODE_NATIVE));
    }
    pick(@"Resolution", @"1080p + MetalFX upscaling");
    assert(runtime_output_mode() == OUTPUT_MODE_METALFX && mfxup_output_enabled() && runtime_output_size_current() == SIZE(1920, 1080));
    if (nativeResolution) {
    pick(@"Resolution", @"2K (2560×1440)");
    assert(runtime_output_mode() == OUTPUT_MODE_NATIVE && !mfxup_output_enabled() && runtime_output_size_current() == SIZE(2560, 1440));
    window_is(1440);   /* the window takes the picked size at once */
    }
    pick(@"Resolution", @"1080p");
    assert(runtime_output_mode() == OUTPUT_MODE_OFF && !mfxup_output_enabled() && runtime_output_size_current() == SIZE(1920, 1080));
    window_is(1080);
    if (nativeResolution) {
        settings_test_output_size(2560, 1440); shows(@"Resolution", @"2K (2560×1440)");   /* set elsewhere: shown */
        settings_test_output_size(1920, 1080);
    }

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
    if (nativeResolution) {
    pick(@"Resolution", @"2K + MetalFX upscaling (4K screens)");
    assert(runtime_output_mode() == OUTPUT_MODE_NATIVE_METALFX && runtime_output_size_current() == SIZE(2560, 1440) && mfxup_output_enabled());
    window_is(2160);
    pick(@"Resolution", @"2K (2560×1440)"); assert(runtime_output_mode() == OUTPUT_MODE_NATIVE && !mfxup_output_enabled());
    window_is(1440);
    } else assert([menu(@"Resolution") indexOfItemWithTitle:@"2K + MetalFX upscaling (4K screens)"] < 0);
    pick(@"Resolution", @"1080p"); assert(runtime_output_size_current() == SIZE(1920, 1080)); window_is(1080);

    /* Switches. */
    assert(!switched(@"Edge smoothing")); flip(@"Edge smoothing", YES); assert(taa_enabled() && switched(@"Edge smoothing"));
    flip(@"Edge smoothing", NO); assert(!taa_enabled());
    assert(!switched(@"Mouse")); flip(@"Mouse", YES); assert(fakeMouse && switched(@"Mouse")); flip(@"Mouse", NO); assert(!fakeMouse);

    /* The DualSense bow trigger: a switch; greyed out while TOTK_DUALSENSE_TRIGGERS or a test session decides, and on a
       game version without the bow's state (rows are made at each opening). */
#ifdef GAME_BOTW160
    shows(@"DualSense bow trigger", @"Not in this version");
    assert(!menu(@"DualSense bow trigger").enabled);
#else
    assert(switched(@"DualSense bow trigger")); flip(@"DualSense bow trigger", NO); assert(!fakeTriggers && !switched(@"DualSense bow trigger"));
    flip(@"DualSense bow trigger", YES); assert(fakeTriggers);
    press(18); fakeTriggersLocked = 1; fakeTriggers = 0; press(18); assert(settingsOpen);
    assert(!menu(@"DualSense bow trigger").enabled && [menu(@"DualSense bow trigger").titleOfSelectedItem isEqualToString:@"Off"]);
    press(18); fakeTriggersLocked = 0; fakeTriggersAvailable = 0; press(18);
    assert(!menu(@"DualSense bow trigger").enabled && [menu(@"DualSense bow trigger").titleOfSelectedItem isEqualToString:@"Not in this version"]);
    press(18); fakeTriggersAvailable = 1; fakeTriggers = 1; press(18); assert(settingsOpen && switched(@"DualSense bow trigger"));
#endif

    /* Controller type: auto names the controller it sees. */
    shows(@"Controller type", @"Auto (PlayStation)");
    pick(@"Controller type", @"Switch"); assert(fakePadType == GAMEPAD_TYPE_SWITCH);
    pick(@"Controller type", @"Auto (PlayStation)"); assert(fakePadType == GAMEPAD_TYPE_AUTO);
    fakePadDetected = -1; shows(@"Controller type", @"Auto (no controller)");

    /* Rumble, its strength and gyro aiming. */
    assert(switched(@"Rumble")); flip(@"Rumble", NO); assert(!fakeFeedback[0] && !switched(@"Rumble")); flip(@"Rumble", YES); assert(fakeFeedback[0]);
    shows(@"Rumble strength", @"1× (default)");
    pick(@"Rumble strength", @"2×"); assert(fakeGain == 2); shows(@"Rumble strength", @"2×");
    pick(@"Rumble strength", @"0.5×"); assert(fakeGain == 0.5f); pick(@"Rumble strength", @"1× (default)"); assert(fakeGain == 1);
    assert(switched(@"Gyro aiming")); flip(@"Gyro aiming", NO); assert(!fakeFeedback[1]); flip(@"Gyro aiming", YES); assert(fakeFeedback[1]);
    /* Rows are made at each opening: a variable that decides (greyed out), a device the run started without (the next
       launch), a strength the file holds that is not a choice. */
    press(18); fakeFeedbackLocked[1] = 1; fakeFeedbackPresent[0] = 0; fakeGain = 2.5f; press(18); assert(settingsOpen);
    assert(!menu(@"Gyro aiming").enabled && [menu(@"Gyro aiming").titleOfSelectedItem isEqualToString:@"On"]);
    (void)row(@"Rumble (from the next launch)"); flip(@"Rumble (from the next launch)", NO); assert(!fakeFeedback[0]);
    shows(@"Rumble strength", @"2.5×"); pick(@"Rumble strength", @"3×"); assert(fakeGain == 3);
    press(18); fakeGainLocked = 1; press(18); assert(!menu(@"Rumble strength").enabled && [menu(@"Rumble strength").titleOfSelectedItem isEqualToString:@"3×"]);
    press(18); fakeFeedbackLocked[1] = 0; fakeFeedbackPresent[0] = 1; fakeFeedback[0] = 1; fakeGainLocked = 0; fakeGain = 1; press(18); assert(settingsOpen);
    assert(switched(@"Rumble") && menu(@"Rumble strength").enabled);
    /* A short window (32:9 on a laptop screen): the rows close up, so the title, the close button and the first row stay in it. */
    NSSize was = window.contentView.bounds.size; press(18); [window setContentSize:NSMakeSize(1728, 486)]; press(18); assert(settingsOpen);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    press(18); [window setContentSize:was]; press(18); assert(settingsOpen);

    /* While it is open the game gets no keys (only key 1 and Esc act); after it closes, keys reach the game again, through
       the player's layout (E confirms: A). */
    key_event(14, YES); assert(!held[14] && !heldOut.buttons); key_event(14, NO);
    key_event(18, YES); key_event(18, NO); assert(!settingsOpen);
    key_event(14, YES); assert(held[14] && heldOut.buttons == 1); key_event(14, NO); assert(!held[14] && !heldOut.buttons);
    /* Keys 2-7 are no shortcuts any more. */
    int interp = mfx_requested(); float shape = runtime_aspect_ratio();
    for (unsigned k = 19; k <= 26; k++) if (k != 24 && k != 25) press(k);
    assert(mfx_requested() == interp && runtime_aspect_ratio() == shape && !settingsOpen && !taa_enabled() && !fakeMouse);

    /* Pages: Display, Input and Key bindings; the page shown last opens again. */
    press(18); assert(settingsOpen);
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY];
    assert(settingsPanel.rows.count == 7 && [settingsPanel.rows[0].title isEqualToString:@"Overlay"]);
    /* The display page's bottom right: contribute shaders and star the project, after the rows for a controller; the
       version stays clear of them; a result shows for a moment where the version is. */
    NSButton *contribute = settingsPanel.displayButtons[1], *starButton = settingsPanel.displayButtons[0];
    assert([contribute.title isEqualToString:@"Contribute shaders and pipelines"] && [starButton.title isEqualToString:@"⭐ Star the project"]);
    assert(!contribute.hidden && !starButton.hidden && NSMaxX(contribute.frame) < NSMinX(starButton.frame) && NSMaxX(starButton.frame) <= settingsPanel.frame.size.width - 14);
    assert(NSMaxX(settingsPanel.versionLabel.frame) <= NSMinX(contribute.frame) && contribute.frame.origin.y < 34 && starButton.frame.origin.y < 34);
    assert(settingsPanel.controllerTargets.count == 9 && settingsPanel.controllerTargets[7] == contribute && settingsPanel.controllerTargets[8] == starButton);
    NSString *versionText = settingsPanel.versionLabel.stringValue;
    [settingsPanel note:@"Thank you!"]; assert([settingsPanel.versionLabel.stringValue isEqualToString:@"Thank you!"]);
    settingsPanel.versionLabel.stringValue = versionText; settingsPanel.versionLabel.frame = NSMakeRect(18, 9, 230, 16);
    [settingsPanel selectPage:SETTINGS_PAGE_INPUT];
    assert(contribute.hidden && starButton.hidden);   /* the first page only */
    assert(settingsPanel.rows.count == 7 && !settingsPanel.pages[SETTINGS_PAGE_INPUT].hidden && settingsPanel.pages[SETTINGS_PAGE_DISPLAY].hidden);
    assert(settingsPanel.tabs.selectedSegment == SETTINGS_PAGE_INPUT);
    press(18); press(18); assert(settingsOpen && settingsPanel.page == SETTINGS_PAGE_INPUT);

    /* Mouse sensitivity: a slider from 1 to 100, at once; greyed out while TOTK_MOUSE_SENSITIVITY decides. */
    NSSlider *slider = (NSSlider *)row(@"Mouse sensitivity").control;
    assert([slider isKindOfClass:NSSlider.class] && slider.minValue == 1 && slider.maxValue == 100 && slider.intValue == 50);
    slider.intValue = 80; [row(@"Mouse sensitivity") changed:slider];
    assert(fakeSensitivity == 80 && [row(@"Mouse sensitivity").valueLabel.stringValue isEqualToString:@"80"]);
    fakeSensitivity = 30; [settingsPanel.timer fire]; assert(slider.intValue == 30);
    press(18); fakeSensitivityLocked = 1; press(18);
    assert(!menu(@"Mouse sensitivity").enabled && [menu(@"Mouse sensitivity").titleOfSelectedItem isEqualToString:@"30"]);
    press(18); fakeSensitivityLocked = 0; fakeSensitivity = 50; press(18); assert(settingsOpen);

    /* The bottom line: the version in the bottom-left corner (not in the overlay box), no key hints, no buttons on the
       settings pages, a small margin under the last row. */
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY];
    assert([settingsPanel.versionLabel.stringValue isEqualToString:@"Ver. " TOTK_GAME_VERSION]);
    press(18); setenv("TOTK_SOURCE_COMMIT", "abc1234", 1); press(18); assert(settingsOpen);
    assert([settingsPanel.versionLabel.stringValue isEqualToString:@"Ver. " TOTK_GAME_VERSION " · build abc1234"]);
    /* The overlay keeps its build line too (user 2026-10-25; after PR #1210). */
    if (hudBox) { [(TOTKHudBox *)hudBox refresh]; assert([((TOTKHudBox *)hudBox).label.stringValue containsString:@"abc1234"]); }
    NSRect lastBand = [settingsPanel.rows.lastObject.control.superview convertRect:settingsPanel.rows.lastObject.control.superview.bounds toView:settingsPanel];
    NSRect version = settingsPanel.versionLabel.frame;
    assert(NSMinX(version) <= 20 && NSMinY(version) >= 6 && NSMaxY(version) <= NSMinY(lastBand) - 6 && NSMinY(lastBand) - NSMaxY(version) <= 14);
    for (NSView *view in settingsPanel.subviews)
        if ([view isKindOfClass:NSButton.class] && NSMaxY(view.frame) <= NSMinY(lastBand))   /* below the rows only the display page's two buttons */
            assert(view.hidden || (settingsPanel.page == SETTINGS_PAGE_DISPLAY && [settingsPanel.displayButtons containsObject:(NSButton *)view]));
    for (NSView *view in settingsPanel.subviews)
        if ([view isKindOfClass:NSTextField.class] && view != settingsPanel.versionLabel) assert(NSMinY(view.frame) > NSMinY(lastBand));

    /* Key bindings: the default layout; a key, a modifier, a mouse button and a controller button bound (an input in use
       swaps places), key 1 cancels, Command keys pass, a right-click clears, the two resets. */
    g_settings_bindings_list = YES; [settingsPanel selectPage:SETTINGS_PAGE_BINDINGS]; tick();
    assert(!settingsPanel.drawings && settingsPanel.frame.size.width == SETTINGS_W);
    assert(settingsPanel.rows.count == 0 && settingsPanel.controllerTargets.count == 20 && !settingsPanel.resetButtons[0].hidden && !settingsPanel.resetButtons[1].hidden &&
           [settingsPanel.listToggle.title isEqualToString:@"Show drawings"] && settingsPanel.listToggle.enabled);
    fakePadDetected = -1; fakePadType = GAMEPAD_TYPE_AUTO; [settingsPanel refreshBindings];
    assert(titled(box(KEYMAP_X, 0), @"Space") && titled(box(KEYMAP_X, 1), @"—") && titled(box(KEYMAP_X, 2), @"Y"));   /* Xbox names: Switch X sits where Xbox Y is */
    assert(titled(box(KEYMAP_Y, 0), @"Left click") && titled(box(KEYMAP_ZL, 1), @"Wheel click") && titled(box(KEYMAP_A, 0), @"E") && titled(box(KEYMAP_A, 1), @"Return"));
    assert(titled(box(KEYMAP_MOVE_UP, 0), @"W") && titled(box(KEYMAP_MOVE_UP, 2), @"Left stick ↑") && !box(KEYMAP_MOVE_UP, 2).enabled);
    fakePadType = GAMEPAD_TYPE_PLAYSTATION; [settingsPanel refreshBindings];
    assert(titled(box(KEYMAP_A, 2), @"○") && [settingsPanel.padHeader.stringValue isEqualToString:@"Controller (PlayStation)"]);
    fakePadType = GAMEPAD_TYPE_AUTO;
    /* The settings panel's own row (user 2026-10-29): key 1 fixed; on a controller L3 + R3, or the button picked (the
       touchpad here), cleared back to L3 + R3 by a right-click and by the controller reset; a button a game action takes
       back leaves the panel on L3 + R3. */
    [settingsPanel refreshBindings];
    assert(titled(box(SETTINGS_BIND_PANEL, 0), @"1") && !box(SETTINGS_BIND_PANEL, 0).enabled && !box(SETTINGS_BIND_PANEL, 1).enabled);
    assert(titled(box(SETTINGS_BIND_PANEL, 2), @"LS + RS") && box(SETTINGS_BIND_PANEL, 2).enabled);
    [settingsPanel beginCapture:box(SETTINGS_BIND_PANEL, 2)]; assert(!fakeSettingsShortcut); [settingsPanel bindPad:16];
    assert(fakeSettingsShortcut && fakeSettingsButton == 16 && !settingsPanel.capturing && titled(box(SETTINGS_BIND_PANEL, 2), @"Touchpad"));
    [settingsPanel clearBinding:box(SETTINGS_BIND_PANEL, 2)]; assert(fakeSettingsButton == -1 && titled(box(SETTINGS_BIND_PANEL, 2), @"LS + RS"));
    [settingsPanel beginCapture:box(SETTINGS_BIND_PANEL, 2)]; [settingsPanel bindPad:10]; assert(fakeSettingsButton == 10);
    [settingsPanel beginCapture:box(KEYMAP_PLUS, 2)]; [settingsPanel bindPad:10]; assert(fakeSettingsButton == -1);
    [settingsPanel beginCapture:box(SETTINGS_BIND_PANEL, 2)]; [settingsPanel bindPad:16]; [settingsPanel resetController:nil]; assert(fakeSettingsButton == -1);
    [settingsPanel beginCapture:box(KEYMAP_X, 0)]; assert(settingsPanel.capturing && titled(box(KEYMAP_X, 0), @"Press a key…"));
    assert([settingsPanel captured:key_down(12, 0)] == nil);   /* Q locked on: it jumps now, lock on takes Space */
    KeyMap keysNow; runtime_keymap_get(&keysNow);
    assert(keysNow.slot[KEYMAP_X][0] == 12 && keysNow.slot[KEYMAP_ZL][0] == 49 && !settingsPanel.capturing && !strcmp(savedName, "keyboard.txt"));
    assert(titled(box(KEYMAP_X, 0), @"Q") && titled(box(KEYMAP_ZL, 0), @"Space"));
    press(18); assert(!settingsOpen); key_event(12, YES); assert(heldOut.buttons == 1u << KEYMAP_X); key_event(12, NO); press(18); assert(settingsOpen);
    [settingsPanel beginCapture:box(KEYMAP_B, 1)];
    NSEvent *command = key_down(12, NSEventModifierFlagCommand); assert([settingsPanel captured:command] == command && settingsPanel.capturing);
    NSEvent *rightShift = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:NSEventModifierFlagShift timestamp:0
                                        windowNumber:window.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:60];
    assert([settingsPanel captured:rightShift] == nil); runtime_keymap_get(&keysNow); assert(keysNow.slot[KEYMAP_B][1] == 60);
    [settingsPanel beginCapture:box(KEYMAP_A, 1)]; assert([settingsPanel captured:key_down(18, 0)] == nil && !settingsPanel.capturing);   /* key 1 cancels */
    runtime_keymap_get(&keysNow); assert(keysNow.slot[KEYMAP_A][1] == 36);
    [settingsPanel beginCapture:box(KEYMAP_Y, 1)];
    NSEvent *right = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:window.windowNumber
                                         context:nil eventNumber:0 clickCount:1 pressure:1];
    assert([settingsPanel captured:right] == nil); runtime_keymap_get(&keysNow);
    assert(keysNow.slot[KEYMAP_Y][1] == KEYMAP_MOUSE_FIRST + 1 && keysNow.slot[KEYMAP_ZR][0] == 5);   /* the bow takes G */
    [settingsPanel clearBinding:box(KEYMAP_Y, 1)]; runtime_keymap_get(&keysNow); assert(keysNow.slot[KEYMAP_Y][1] == KEYMAP_NONE && titled(box(KEYMAP_Y, 1), @"—"));
    /* A controller button: through the panel's controller input, L1 now jumps and the ability takes the top button. */
    [settingsPanel beginCapture:box(KEYMAP_X, 2)]; assert(titled(box(KEYMAP_X, 2), @"Press a button…"));
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:100 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1u << 6} time:100.1 active:YES];
    assert(fakeMap[6] == 1u << KEYMAP_X && fakeMap[3] == 1u << KEYMAP_L && !settingsPanel.capturing && titled(box(KEYMAP_X, 2), @"LB") && titled(box(KEYMAP_L, 2), @"Y"));
    [settingsPanel clearBinding:box(KEYMAP_X, 2)]; assert(!fakeMap[6] && titled(box(KEYMAP_X, 2), @"—"));
    [settingsPanel.resetButtons[0] performClick:nil]; runtime_keymap_get(&keysNow);
    KeyMap defaults; keymap_default(&defaults); assert(!memcmp(&keysNow, &defaults, sizeof defaults) && !strcmp(removedName, "keyboard.txt"));
    [settingsPanel.resetButtons[1] performClick:nil];
    uint16_t defaultMap[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaultMap, defaultModes);
    assert(!memcmp(fakeMap, defaultMap, sizeof defaultMap) && titled(box(KEYMAP_X, 2), @"Y"));
    /* With a controller: nothing is highlighted until it moves (PR #1210); the first A only shows where it is, the
       next on a row waits for the new button; R1 / L1 change the page. */
    for (NSView *v in settingsPanel.controllerTargets) assert(v.layer.borderWidth == 0);
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:101 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1} time:101.1 active:YES];
    assert(!settingsPanel.capturing && settingsPanel.controllerTargets[0].layer.borderWidth == 2);
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:101.2 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1} time:101.3 active:YES];
    assert(settingsPanel.capturing && settingsPanel.capturing.slot == 2);
    settings_pointer_used(); assert(settingsPanel.controllerTargets[0].layer.borderWidth == 0);   /* a click: the highlight goes */
    [settingsPanel endCapture];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:102 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1u << 7} time:102.1 active:YES];
    assert(settingsPanel.page == SETTINGS_PAGE_DISPLAY);
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:103 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1u << 6} time:103.1 active:YES];
    assert(settingsPanel.page == SETTINGS_PAGE_BINDINGS);
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY];

    /* Key bindings as drawings (PR #1210): the panel made wider for them; the keyboard and mouse, or the controller,
       with what each key or button does; a click's menu of actions; the actions nothing drives; a controller moves through
       the controller's buttons and changes them; a window too small for them gets the list. */
    press(18); [window setContentSize:NSMakeSize(1280, 720)]; g_settings_bindings_list = NO; g_settings_bindings_device = 0; press(18); assert(settingsOpen);
    for (NSView *v in settingsPanel.controllerTargets) assert(v.layer.borderWidth == 0);   /* nothing looks picked at the opening */
    [settingsPanel selectPage:SETTINGS_PAGE_BINDINGS]; tick();
    assert(settingsPanel.drawings && settingsPanel.page == SETTINGS_PAGE_BINDINGS && settingsPanel.frame.size.width == SETTINGS_WIDE_W);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame) && settingsPanel.tabs.selectedSegment == SETTINGS_PAGE_BINDINGS);
    assert(!settingsPanel.keyboardDiagram.hidden && settingsPanel.padDiagram.hidden && !settingsPanel.resetButtons[0].hidden && settingsPanel.resetButtons[1].hidden);
    assert([settingsPanel.listToggle.title isEqualToString:@"Show as list"] && [settingsPanel.drawingsHelp.stringValue hasPrefix:@"Click a key"]);
    TOTKKeyboardDiagram *kb = settingsPanel.keyboardDiagram;
    NSRect eKey = [kb rectOfInput:14], oneKey = [kb rectOfInput:18], wheel = [kb rectOfInput:KEYMAP_MOUSE_FIRST + 2], leftClick = [kb rectOfInput:KEYMAP_MOUSE_FIRST];
    assert([kb inputAt:NSMakePoint(NSMidX(eKey), NSMidY(eKey)) rect:NULL] == 14);
    assert([kb inputAt:NSMakePoint(NSMidX(oneKey), NSMidY(oneKey)) rect:NULL] == -1);   /* key 1 opens the panel: not changeable */
    assert([kb inputAt:NSMakePoint(NSMidX(wheel), NSMidY(wheel)) rect:NULL] == KEYMAP_MOUSE_FIRST + 2);
    assert([kb inputAt:NSMakePoint(NSMinX(leftClick) + 8, NSMidY(leftClick)) rect:NULL] == KEYMAP_MOUSE_FIRST);
    NSMenu *actions = [settingsPanel menuForInput:14 pad:NO]; NSMenuItem *ticked = nil, *jump = nil;
    for (NSMenuItem *i in actions.itemArray) { if (i.state == NSControlStateValueOn) ticked = i; if (i.action && i.tag == KEYMAP_X) jump = i; }
    assert([actions.itemArray.firstObject.title isEqualToString:@"E"] && ticked && ticked.tag == KEYMAP_A && jump && [actions.itemArray.lastObject.title isEqualToString:@"None (unused)"]);
    [settingsPanel menuPicked:jump]; runtime_keymap_get(&keysNow);   /* E jumps now (with Space); confirm keeps Return */
    assert(keysNow.slot[KEYMAP_X][0] == 49 && keysNow.slot[KEYMAP_X][1] == 14 && keysNow.slot[KEYMAP_A][0] == 36 && keysNow.slot[KEYMAP_A][1] == KEYMAP_NONE);
    [settingsPanel assignInput:36 action:-1]; assert([diagram_missing(0) isEqualToString:@"No key: Confirm / Interact"]);
    [settingsPanel.resetButtons[0] performClick:nil]; assert(diagram_missing(0) == nil);
    settingsPanel.deviceTabs.selectedSegment = 1; [settingsPanel deviceChanged:settingsPanel.deviceTabs];
    assert(settingsPanel.keyboardDiagram.hidden && !settingsPanel.padDiagram.hidden && settingsPanel.resetButtons[0].hidden && !settingsPanel.resetButtons[1].hidden);
    TOTKControllerDiagram *pd = settingsPanel.padDiagram; NSRect north = [pd rectOfPhysical:3];
    assert([pd physicalAt:NSMakePoint(NSMidX(north), NSMidY(north)) rect:NULL] == 3 && [pd order].count == 16 && [pd order][0].intValue == 8);
    actions = [settingsPanel menuForInput:3 pad:YES]; int choices = 0; ticked = nil;
    for (NSMenuItem *i in actions.itemArray) if (i.action) { choices++; assert(i.tag < KEYMAP_SWITCH_BUTTONS || i.tag == SETTINGS_BIND_PANEL); if (i.state == NSControlStateValueOn) ticked = i; }
    assert(choices == 18 && ticked.tag == KEYMAP_X);   /* 16 buttons, the settings panel and none: no stick directions */
    for (NSMenuItem *i in actions.itemArray) if (i.tag == SETTINGS_BIND_PANEL) [settingsPanel menuPicked:i];
    assert(fakeSettingsButton == 3); [settingsPanel assignPad:3 action:KEYMAP_X]; assert(fakeSettingsButton == -1 && fakeMap[3] == 1u << KEYMAP_X);
    [settingsPanel assignPad:3 action:KEYMAP_ZR]; assert(fakeMap[3] == 1u << KEYMAP_ZR && fakeMap[9] == 1u << KEYMAP_ZR && [diagram_missing(1) isEqualToString:@"No button: Jump"]);
    assert(settingsPanel.controllerTargets.count == 19 && pd.selected == -1);
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:200 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1u << 15} time:200.1 active:YES];   /* shows where it is */
    assert(settingsPanel.controllerIndex == 0 && settingsPanel.deviceTabs.layer.borderWidth == 2 && pd.selected == -1);
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:201 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1u << 15} time:201.1 active:YES];
    assert(settingsPanel.controllerIndex == 1 && pd.selected == 8 && settingsPanel.deviceTabs.layer.borderWidth == 0);   /* LT */
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:202 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1u << 14} time:202.1 active:YES];
    assert(fakeMap[8] == 1u << KEYMAP_ZR);   /* right: the next action, after lock on the bow */
    settings_pointer_used(); assert(pd.selected == -1);
    [settingsPanel.resetButtons[1] performClick:nil]; assert(!memcmp(fakeMap, defaultMap, sizeof defaultMap) && diagram_missing(1) == nil);
    [settingsPanel.listToggle performClick:nil]; tick(); assert(g_settings_bindings_list && !settingsPanel.drawings && settingsPanel.page == SETTINGS_PAGE_BINDINGS);
    [settingsPanel.listToggle performClick:nil]; tick(); assert(!g_settings_bindings_list && settingsPanel.drawings);
    [settingsPanel selectPage:SETTINGS_PAGE_INPUT]; tick(); assert(!settingsPanel.drawings && settingsPanel.frame.size.width == SETTINGS_W);
    press(18); [window setContentSize:NSMakeSize(900, 600)]; g_settings_page = SETTINGS_PAGE_BINDINGS; press(18);
    assert(settingsOpen && !settingsPanel.drawings && settingsPanel.frame.size.width == SETTINGS_W && !settingsPanel.listToggle.enabled);   /* too small: the list */
    press(18); [window setContentSize:NSMakeSize(1280, 720)]; g_settings_page = SETTINGS_PAGE_DISPLAY; press(18); assert(settingsOpen);

    press(18); assert(!settingsOpen);

    /* Chinese. */
    setenv("TOTK_UI_LANG", "zh", 1); press(18); assert(settingsOpen);
    shows(@"分辨率", @"1080p"); shows(@"手柄类型", @"自动（未接手柄）");
#ifdef GAME_BOTW160
    shows(@"DualSense 拉弓扳机", @"这个版本没有"); assert(!menu(@"DualSense 拉弓扳机").enabled);
#else
    assert(switched(@"DualSense 拉弓扳机"));
#endif
    fakePadDetected = GAMEPAD_TYPE_SWITCH; shows(@"手柄类型", @"自动（Switch）");
    pick(@"补帧", @"50+50"); assert(mfx_requested() == MFX_50);
    shows(@"震动强度", @"1×（默认）"); assert(switched(@"震动") && switched(@"体感瞄准"));

    if (argc == 2) {
        NSBitmapImageRep *rep = [settingsPanel bitmapImageRepForCachingDisplayInRect:settingsPanel.bounds];
        [settingsPanel cacheDisplayInRect:settingsPanel.bounds toBitmapImageRep:rep];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES]);
    }
    [settingsPanel.timer invalidate];
    puts("pass: settings panel (key 1 / Esc, overlay, frame lock, interpolation, resolution 1080p / MetalFX / 2K / 4K and window size, aspect 16:9 / 16:10 / 21:9 / 32:9, aspect ratio, switches, controller type, rumble / rumble strength / gyro aiming, game keys blocked while open, keys 2-7 inert, Chinese)");
    return 0;
}}
