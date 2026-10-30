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
#ifdef GAME_BOTW160
#define BOW_ROW 0   /* no bow row where the game has no bow state */
#else
#define BOW_ROW 1
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
/* The row of that title (exactly, else the first that starts with it: "Rumble (from the next launch)"), nil none. */
static TOTKSettingRow *find_row(NSString *title) {
    for (TOTKSettingRow *r in settingsPanel.allRows) if ([r.title isEqualToString:title]) return r;
    for (TOTKSettingRow *r in settingsPanel.allRows) if ([r.title hasPrefix:title]) return r;
    return nil;
}
static TOTKSettingRow *row(NSString *title) {
    TOTKSettingRow *r = find_row(title); if (r) return r;
    fprintf(stderr, "no row %s\n", title.UTF8String); abort();
}
/* A choice picked as the row's control does (its value applied, the panel refreshed). */
static void pick(NSString *title, NSString *choice) {
    TOTKSettingRow *r = row(title);
    NSUInteger i = [r.choices indexOfObject:choice];
    if (i == NSNotFound && r.firstTitle && [r.firstTitle() isEqualToString:choice]) i = 0;
    if (!r.enabled || i == NSNotFound) {
        fprintf(stderr, "%s: cannot pick %s (enabled %d, choices %s)\n", title.UTF8String, choice.UTF8String, r.enabled,
                [r.choices componentsJoinedByString:@" | "].UTF8String); abort();
    }
    [r apply:r.values[i].intValue]; settings_test_aspect_frame();
}
/* A switch: a click flips it (as the row does); already there, its value applied again. */
static void flip(NSString *title, BOOL on) {
    TOTKSettingRow *r = row(title);
    assert(r.enabled && !r.choices && !r.slider);
    if ((r.value != 0) != on) [r activate]; else [r apply:on];
    assert((r.value != 0) == on);
}
static void shows(NSString *title, NSString *choice) {
    [settingsPanel.timer fire];
    NSString *now = row(title).text;
    if (![now isEqualToString:choice]) { fprintf(stderr, "%s shows %s, expected %s\n", title.UTF8String, now.UTF8String, choice.UTF8String); abort(); }
}
static BOOL switched(NSString *title) { [settingsPanel.timer fire]; TOTKSettingRow *r = row(title); assert(!r.choices && !r.slider); return r.value != 0; }
/* The band (the row's view) of that row on its page. */
static TOTKRowBand *band(NSString *title) {
    TOTKSettingRow *r = row(title);
    for (TOTKRowBand *b in settingsPanel.bands) if (b.row == r) return b;
    fprintf(stderr, "no band %s\n", title.UTF8String); abort();
}
/* The one item that shows the focus (the white block); it is always the controller's. */
static NSInteger picked_at(void) {
    NSInteger at = -1, n = 0;
    for (NSUInteger i = 0; i < settingsPanel.controllerTargets.count; i++) if (settingsPanel.controllerTargets[i].focus) { at = (NSInteger)i; n++; }
    for (TOTKRowBand *b in settingsPanel.bands) if (b.focus && ![settingsPanel.controllerTargets containsObject:b]) n++;
    if (n != 1 || at != settingsPanel.controllerIndex) { fprintf(stderr, "%ld items focused, at %ld, controller at %ld\n", (long)n, (long)at, (long)settingsPanel.controllerIndex); abort(); }
    return at;
}
static NSString *focused_title(void);
static void pad(uint64_t buttons);
/* Down until that row is picked (at most once round the page). */
static void focus_down_to(NSString *title) {
    for (NSUInteger n = 0; n <= settingsPanel.controllerTargets.count; n++) { if ([focused_title() isEqualToString:title]) return; pad(1u << 15); }
    fprintf(stderr, "down never reaches %s\n", title.UTF8String); abort();
}
static NSString *focused_title(void) {
    TOTKFocusView *v = settingsPanel.controllerTargets[(NSUInteger)picked_at()];
    assert([v isKindOfClass:TOTKRowBand.class]); return ((TOTKRowBand *)v).row.title;
}
/* One controller press: released (it arms), pressed, released; each a second apart so no repeat is due. */
static double padTime = 1000;
static void pad(uint64_t buttons) {
    RuntimeGamepadState s = {.connected = 1};
    [settingsPanel controllerPad:s time:padTime active:YES]; padTime += 1;
    s.buttons = buttons; [settingsPanel controllerPad:s time:padTime active:YES]; padTime += 1;
    s.buttons = 0; [settingsPanel controllerPad:s time:padTime active:YES]; padTime += 1;
}
enum { PAD_A = 1u << 0, PAD_B = 1u << 1, PAD_Y = 1u << 3, PAD_L1 = 1u << 6, PAD_R1 = 1u << 7,
       PAD_LEFT = 1u << 12, PAD_UP = 1u << 13, PAD_RIGHT = 1u << 14, PAD_DOWN = 1u << 15 };
/* A hint at the bottom right shows this glyph or key cap. */
static BOOL hint_in(NSView *v, NSString *text) {
    if ([v isKindOfClass:NSTextField.class] && [((NSTextField *)v).stringValue isEqualToString:text]) return YES;
    for (NSView *s in v.subviews) if (hint_in(s, text)) return YES;
    return NO;
}
static BOOL hint(NSString *text) { return hint_in(settingsPanel.hints, text); }
static NSEvent *mouse(NSEventType type) {
    return [NSEvent mouseEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:window.windowNumber
                               context:nil eventNumber:0 clickCount:1 pressure:1];
}
static BOOL panel_size(CGFloat w, CGFloat h) {
    NSSize s = settingsPanel.frame.size; if (s.width == w && s.height == h) return YES;
    fprintf(stderr, "panel %.0fx%.0f, expected %.0fx%.0f\n", s.width, s.height, w, h); return NO;
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
static TOTKKeyCard *card(int action) {
    for (TOTKKeyCard *c in settingsPanel.cards) if (c.gameAction == action) return c;
    fprintf(stderr, "no card %d\n", action); abort();
}
static TOTKPadLabel *label(int physical) {
    for (TOTKPadLabel *l in settingsPanel.padLabels) if (l.physical == physical) return l;
    fprintf(stderr, "no label %d\n", physical); abort();
}
static BOOL labelled(int physical, NSString *text) {
    NSString *now = label(physical).text; if ([now isEqualToString:text]) return YES;
    fprintf(stderr, "label %d shows %s, expected %s\n", physical, now.UTF8String, text.UTF8String); return NO;
}
/* A controller label shows what its button drives now. */
static NSString *drives(int physical) { return settings_action_name(controller_map_target(fakeMap[physical])); }

static void persistence_probe(const char *action) {
#ifdef GAME_BOTW190
    const int outputMode = OUTPUT_MODE_METALFX;   /* 1.9.0 has no native 2K canvas mode. */
    NSString *outputChoice = @"1080p + MetalFX upscaling";
#else
    const int outputMode = OUTPUT_MODE_NATIVE;
    NSString *outputChoice = @"2K";
#endif
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView = [[TOTKInputView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    g_settings_bindings_device = 0;
    if (!strcmp(action, "save")) {
        settings_test_output_init(); settings_test_aspect_init(16.0 / 9); settingsPanel = [TOTKSettingsPanel new];
        pick(@"Overlay", @"FPS + playtime"); pick(@"Frame interpolation", @"40 → 80");
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
    TOTKSettingRow *lock=frame_row(@"Frame rate cap"), *interp=frame_row(@"Frame interpolation");
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
        assert(!frame_row(@"Frame rate cap").set);
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
    g_settings_bindings_device = 0;   /* the input method: keyboard and mouse (no controller file, none connected) */
    settings_test_output_init();
    // In-memory fixture only: do not start the timer or touch a player's profile.
    playtime=[[TOTKPlaytime alloc] initWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView = [[TOTKInputView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    if (hud_compact()) { TOTKHudBox *box = [[TOTKHudBox alloc] initWithFrame:NSZeroRect]; [window.contentView addSubview:box]; hudBox = box; }

    /* Key 1 opens it; a held key acts once. One size (the photo's, the window has room), the display page, its first row
       picked; the rows read their values again twice a second. */
    runtime_input_key_synthetic(18, 1); runtime_input_key_synthetic(18, 1); tick(); runtime_input_key_synthetic(18, 0); tick();
    assert(settingsOpen && fakeSettingsCapture && settingsPanel && !settingsPanel.hidden && settingsPanel.superview == window.contentView);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    assert(settingsPanel.allRows.count == 13 + BOW_ROW);
    assert(panel_size(SETTINGS_WIDE_W, SETTINGS_WIDE_H) && settingsPanel.page == SETTINGS_PAGE_DISPLAY);
    assert(picked_at() == 0 && [focused_title() isEqualToString:@"Resolution"]);
    assert(settingsPanel.timer.valid && settingsPanel.timer.timeInterval == 0.5);

    /* FPS graph: a switch (here off at start; frontend.m turns it on when nothing is saved), kept in fps-graph.txt
       that both games share (user 2026-10-29). */
    assert(!switched(@"FPS graph"));
    int shared0 = sharedSaves;
    flip(@"FPS graph", YES); assert(fpsGraphEnabled && !strcmp(savedName, "fps-graph.txt") && !strcmp(savedWord, "on") && sharedSaves == shared0 + 1);
    flip(@"FPS graph", NO); assert(!fpsGraphEnabled && !strcmp(savedWord, "off") && sharedSaves == shared0 + 2);

    /* Overlay: all -> FPS + playtime -> off; the corner box follows. */
    hud_publish(HUD_FPS, "FPS 60  2560x1440", 0); hud_publish(HUD_SHADER, "New shaders 3 / total 9", 0);
    assert([row(@"Overlay").choices isEqualToArray:(@[@"All", @"FPS + playtime", @"Off"])]);
    shows(@"Overlay", @"All");
    pick(@"Overlay", @"FPS + playtime"); assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    if (hudBox) { TOTKHudBox *b = (TOTKHudBox *)hudBox; assert(!b.hidden && [b.label.stringValue isEqualToString:@"FPS 60  2560x1440\nPlaytime  0 h 00 m"]); }
    pick(@"Overlay", @"Off"); assert(fps_overlay_hud_mode() == HUD_MODE_OFF && (!hudBox || hudBox.hidden));
    pick(@"Overlay", @"All");
    if (hudBox) assert(!hudBox.hidden && [((TOTKHudBox *)hudBox).label.stringValue containsString:@"New shaders"]);

    if (hudBox) assert([((TOTKHudBox *)hudBox).label.stringValue containsString:@"Playtime  0 h 00 m"]);

    /* Frame rate cap: unlimited -> 30 -> ... -> 120 -> unlimited. */
    shows(@"Frame rate cap", @"Unlimited");
    pick(@"Frame rate cap", @"30"); assert(fakeLock == 30);
    pick(@"Frame rate cap", @"40"); assert(fakeLock == 40);
    pick(@"Frame rate cap", @"60"); assert(fakeLock == 60);
    pick(@"Frame rate cap", @"72"); assert(fakeLock == 72);
    pick(@"Frame rate cap", @"90"); assert(fakeLock == 90);
    pick(@"Frame rate cap", @"120"); assert(fakeLock == 120);
    pick(@"Frame rate cap", @"Unlimited"); assert(fakeLock == 0);
    nvn_fps_lock_set(60); shows(@"Frame rate cap", @"60"); nvn_fps_lock_set(0);
    /* Frame lock and interpolation exclude each other: picking one turns the other off, both rows show it at once. */
    mfx_set(MFX_30); mfx_latch(); [settingsPanel refresh];
    pick(@"Frame rate cap", @"60");
    assert(fakeLock == 60 && mfx_requested() == MFX_OFF && [row(@"Frame interpolation").text isEqualToString:@"Off"]);
    pick(@"Frame interpolation", @"30 → 60");
    assert(fakeLock == 0 && mfx_requested() == MFX_30 && [row(@"Frame rate cap").text isEqualToString:@"Unlimited"]);
    pick(@"Frame interpolation", @"Off"); assert(fakeLock == 0);   /* interpolation off leaves the lock off */
    pick(@"Frame rate cap", @"30"); pick(@"Frame rate cap", @"Unlimited"); assert(mfx_requested() == MFX_OFF);

    /* Frame interpolation: the pick shows at once (the game takes it at its next frame); a change elsewhere shows too. */
    mfx_set(MFX_OFF); mfx_latch(); shows(@"Frame interpolation", @"Off");
    pick(@"Frame interpolation", @"30 → 60"); assert(mfx_requested() == MFX_30); shows(@"Frame interpolation", @"30 → 60");
    mfx_latch(); assert(mfx_enabled() == MFX_30);
    mfx_set(MFX_MAX); shows(@"Frame interpolation", @"Unlimited");
    pick(@"Frame interpolation", @"40 → 80"); assert(mfx_requested() == MFX_40);
    pick(@"Frame interpolation", @"50 → 100"); assert(mfx_requested() == MFX_50);
    assert(row(@"Frame interpolation").choices.count == 5 && row(@"Frame rate cap").choices.count == 7);
    pick(@"Frame interpolation", @"Off"); mfx_latch(); assert(!mfx_enabled());

    /* Resolution: the player picks; 1080p without MetalFX at the start. */
    mfxup_output_set(0); mfxup_set(0); shows(@"Resolution", @"1080p");
    BOOL nativeResolution = !!(runtime_output_modes() & (1 << OUTPUT_MODE_NATIVE));
    if (nativeResolution) {
    assert(row(@"Resolution").choices.count == 5 && [row(@"Resolution").choices.firstObject isEqualToString:@"720p"]);
    pick(@"Resolution", @"720p"); assert(runtime_output_mode() == OUTPUT_MODE_720P && runtime_output_size_current() == SIZE(1280, 720));
    window_is(720);
    } else {
        assert(row(@"Resolution").choices.count == 2);
        assert(![row(@"Resolution").choices containsObject:@"720p"]);
        assert(!runtime_output_mode_set(OUTPUT_MODE_720P) && !runtime_output_mode_set(OUTPUT_MODE_NATIVE));
    }
    pick(@"Resolution", @"1080p + MetalFX upscaling");
    assert(runtime_output_mode() == OUTPUT_MODE_METALFX && mfxup_output_enabled() && runtime_output_size_current() == SIZE(1920, 1080));
    if (nativeResolution) {
    pick(@"Resolution", @"2K");
    assert(runtime_output_mode() == OUTPUT_MODE_NATIVE && !mfxup_output_enabled() && runtime_output_size_current() == SIZE(2560, 1440));
    window_is(1440);   /* the window takes the picked size at once */
    }
    pick(@"Resolution", @"1080p");
    assert(runtime_output_mode() == OUTPUT_MODE_OFF && !mfxup_output_enabled() && runtime_output_size_current() == SIZE(1920, 1080));
    window_is(1080);
    if (nativeResolution) {
        settings_test_output_size(2560, 1440); shows(@"Resolution", @"2K");   /* set elsewhere: shown */
        settings_test_output_size(1920, 1080);
    }

    /* Aspect ratio: fixed at 16:9 while the game cannot change it (greyed out: a controller skips it); offered once its
       code is in (the next opening). */
    assert(!row(@"Aspect ratio").enabled && [row(@"Aspect ratio").text isEqualToString:@"16:9"]);
    assert([focused_title() isEqualToString:@"Resolution"]);
    pad(PAD_DOWN); assert([focused_title() isEqualToString:@"Frame rate cap"]);
    pad(PAD_UP); assert([focused_title() isEqualToString:@"Resolution"]);
    settings_test_aspect_init(1.6);
    key_event(53, YES); key_event(53, NO); assert(!settingsOpen && settingsPanel.hidden);   /* Esc closes it */
    press(18); assert(settingsOpen);
    /* 16:9, 16:10, 21:9, 32:9 (no fitting the window any more); the window takes each shape at once. */
    assert(row(@"Aspect ratio").enabled && [row(@"Aspect ratio").choices isEqualToArray:(@[@"16:9", @"16:10", @"21:9", @"32:9"])]);
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
    pick(@"Resolution", @"2K"); assert(runtime_output_mode() == OUTPUT_MODE_NATIVE && !mfxup_output_enabled());
    window_is(1440);
    } else assert(![row(@"Resolution").choices containsObject:@"2K + MetalFX upscaling (4K screens)"]);
    pick(@"Resolution", @"1080p"); assert(runtime_output_size_current() == SIZE(1920, 1080)); window_is(1080);

    /* Switches. */
    assert(!switched(@"Edge smoothing")); flip(@"Edge smoothing", YES); assert(taa_enabled() && switched(@"Edge smoothing"));
    flip(@"Edge smoothing", NO); assert(!taa_enabled());
    assert(!switched(@"Mouse camera")); flip(@"Mouse camera", YES); assert(fakeMouse && switched(@"Mouse camera")); flip(@"Mouse camera", NO); assert(!fakeMouse);

    /* A controller on the display page: up / down move the picked row; A flips a switch but leaves a choice; left / right
       step a choice (round at the ends). A click on a row picks it and steps it as A would. */
    focus_down_to(@"Edge smoothing");
    assert(settingsPanel.controllerIndex == 4);
    pad(PAD_A); assert(taa_enabled()); pad(PAD_A); assert(!taa_enabled());
    pad(PAD_DOWN); assert([focused_title() isEqualToString:@"Overlay"]);
    pad(PAD_A); assert(fps_overlay_hud_mode() == HUD_MODE_ALL);
    pad(PAD_RIGHT); assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    pad(PAD_LEFT); assert(fps_overlay_hud_mode() == HUD_MODE_ALL);
    pad(PAD_LEFT); assert(fps_overlay_hud_mode() == HUD_MODE_OFF); pad(PAD_RIGHT); assert(fps_overlay_hud_mode() == HUD_MODE_ALL);
    pad(PAD_DOWN); pad(PAD_DOWN); assert([focused_title() isEqualToString:@"Resolution"]);   /* past the last row: the first */
    pad(PAD_DOWN); pad(PAD_DOWN); assert([focused_title() isEqualToString:@"Frame rate cap"]);
    pad(PAD_LEFT); assert(fakeLock == 120); pad(PAD_RIGHT); assert(fakeLock == 0);
    [band(@"Overlay") mouseDown:mouse(NSEventTypeLeftMouseDown)];
    assert([focused_title() isEqualToString:@"Overlay"] && fps_overlay_hud_mode() == HUD_MODE_MINIMAL);
    [band(@"Edge smoothing") mouseDown:mouse(NSEventTypeLeftMouseDown)];
    assert([focused_title() isEqualToString:@"Edge smoothing"] && taa_enabled());
    flip(@"Edge smoothing", NO); pick(@"Overlay", @"All");

    /* The DualSense bow trigger: a switch; greyed out while TOTK_DUALSENSE_TRIGGERS or a test session decides, and on a
       game version without the bow's state (rows are made at each opening). */
#ifdef GAME_BOTW160
    assert(!find_row(@"DualSense bow trigger"));   /* no row where the game has no bow state */
#else
    assert(switched(@"DualSense bow trigger")); flip(@"DualSense bow trigger", NO); assert(!fakeTriggers && !switched(@"DualSense bow trigger"));
    flip(@"DualSense bow trigger", YES); assert(fakeTriggers);
    press(18); fakeTriggersLocked = 1; fakeTriggers = 0; press(18); assert(settingsOpen);
    assert(!row(@"DualSense bow trigger").enabled && [row(@"DualSense bow trigger").text isEqualToString:@"Off"]);
    press(18); fakeTriggersLocked = 0; fakeTriggersAvailable = 0; press(18);
    assert(!find_row(@"DualSense bow trigger"));
    press(18); fakeTriggersAvailable = 1; fakeTriggers = 1; press(18); assert(settingsOpen && switched(@"DualSense bow trigger"));
#endif

    /* Controller type: auto names the controller it sees. */
    shows(@"Controller type", @"Auto (PlayStation)");
    pick(@"Controller type", @"Switch"); assert(fakePadType == GAMEPAD_TYPE_SWITCH);
    pick(@"Controller type", @"Auto (PlayStation)"); assert(fakePadType == GAMEPAD_TYPE_AUTO);
    fakePadDetected = -1; shows(@"Controller type", @"Auto (no controller)");

    /* Rumble: one row of bars, none lit = off, then 0.5x to 4x; a level turns rumble on. Gyro aiming: a switch. */
    TOTKSettingRow *rumble = row(@"Rumble");
    assert(rumble.bars && rumble.enabled && !find_row(@"Rumble strength"));
    assert([rumble.choices isEqualToArray:(@[@"Off", @"0.5×", @"1×", @"2×", @"3×", @"4×"])]);
    shows(@"Rumble", @"1×");
    pick(@"Rumble", @"Off"); assert(!fakeFeedback[0] && fakeGain == 1); shows(@"Rumble", @"Off");
    pick(@"Rumble", @"2×"); assert(fakeFeedback[0] && fakeGain == 2); shows(@"Rumble", @"2×");
    pick(@"Rumble", @"0.5×"); assert(fakeGain == 0.5f);
    [rumble stepBy:-1]; assert(!fakeFeedback[0]); [rumble stepBy:-1]; assert(!fakeFeedback[0] && rumble.index == 0);   /* bars stop at the ends */
    pick(@"Rumble", @"4×"); [rumble stepBy:1]; assert(fakeGain == 4 && fakeFeedback[0]);
    [rumble activate]; assert(!fakeFeedback[0]);   /* a click past the last bar: off */
    [rumble activate]; assert(fakeFeedback[0] && fakeGain == 0.5f);
    pick(@"Rumble", @"1×"); assert(fakeFeedback[0] && fakeGain == 1);
    assert(switched(@"Gyro aiming")); flip(@"Gyro aiming", NO); assert(!fakeFeedback[1]); flip(@"Gyro aiming", YES); assert(fakeFeedback[1]);
    /* Rows are made at each opening: a variable that decides (greyed out), a device the run started without (the next
       launch), a strength the file holds that is not a bar (the nearest lit). */
    press(18); fakeFeedbackLocked[1] = 1; fakeFeedbackPresent[0] = 0; fakeGain = 2.5f; press(18); assert(settingsOpen);
    assert(!row(@"Gyro aiming").enabled && [row(@"Gyro aiming").text isEqualToString:@"On"]);
    rumble = row(@"Rumble (from the next launch)"); assert(rumble.bars && rumble.enabled);
    shows(@"Rumble (from the next launch)", @"2×");
    pick(@"Rumble (from the next launch)", @"Off"); assert(!fakeFeedback[0]);
    pick(@"Rumble (from the next launch)", @"3×"); assert(fakeFeedback[0] && fakeGain == 3);
    /* TOTK_RUMBLE (or a test session) decides rumble: the switch greyed out, and the strength row as before, with the
       strength the file holds as one more choice. */
    press(18); fakeFeedbackLocked[1] = 0; fakeFeedbackLocked[0] = 1; fakeFeedbackPresent[0] = 1; fakeGain = 2.5f; press(18); assert(settingsOpen);
    assert(settingsPanel.allRows.count == 14 + BOW_ROW && !row(@"Rumble").enabled && !row(@"Rumble").bars && [row(@"Rumble").text isEqualToString:@"On"]);
    assert(row(@"Rumble strength").enabled && row(@"Rumble strength").choices.count == 6);
    shows(@"Rumble strength", @"2.5×"); pick(@"Rumble strength", @"3×"); assert(fakeGain == 3); shows(@"Rumble strength", @"3×");
    pick(@"Rumble strength", @"1× (default)"); assert(fakeGain == 1); pick(@"Rumble strength", @"3×");
    /* TOTK_RUMBLE_GAIN decides the strength: the rumble switch, the strength greyed out. */
    press(18); fakeFeedbackLocked[0] = 0; fakeGainLocked = 1; press(18); assert(settingsOpen);
    assert(!row(@"Rumble strength").enabled && [row(@"Rumble strength").text isEqualToString:@"3×"]);
    assert(!row(@"Rumble").bars && switched(@"Rumble")); flip(@"Rumble", NO); assert(!fakeFeedback[0]); flip(@"Rumble", YES); assert(fakeFeedback[0]);
    press(18); fakeGainLocked = 0; fakeGain = 1; press(18); assert(settingsOpen);
    assert(settingsPanel.allRows.count == 13 + BOW_ROW && row(@"Rumble").bars && row(@"Rumble").enabled && !find_row(@"Rumble strength"));
    /* A short window (32:9 on a laptop screen): the smaller panel, the rows close up, so the tabs and every row stay in it,
       above the bottom line. */
    NSSize was = window.contentView.bounds.size; press(18); [window setContentSize:NSMakeSize(1728, 486)]; press(18); assert(settingsOpen);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame) && settingsPanel.frame.size.width == SETTINGS_W);
    for (TOTKRowBand *b in settingsPanel.bands) assert(NSMinY(b.frame) >= 40 && NSMaxY(b.frame) <= NSMinY(settingsPanel.tabs.frame));
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

    /* Pages: Display, Input and Key bindings, one size for all; the page shown last opens again. */
    [window setContentSize:NSMakeSize(1280, 720)]; press(18); assert(settingsOpen);
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY];
    assert(panel_size(SETTINGS_WIDE_W, SETTINGS_WIDE_H));
    assert(settingsPanel.rows.count == 7 && [settingsPanel.rows[0].title isEqualToString:@"Resolution"]);
    assert(settingsPanel.controllerTargets.count == 7 && picked_at() == 0);
    [settingsPanel selectPage:SETTINGS_PAGE_INPUT];
    assert(settingsPanel.rows.count == 6 + BOW_ROW && [settingsPanel.rows[0].title isEqualToString:@"Input method"]);
    assert(!settingsPanel.pages[SETTINGS_PAGE_INPUT].hidden && settingsPanel.pages[SETTINGS_PAGE_DISPLAY].hidden);
    assert(settingsPanel.tabs.selectedSegment == SETTINGS_PAGE_INPUT && panel_size(SETTINGS_WIDE_W, SETTINGS_WIDE_H) && picked_at() == 0);
    press(18); press(18); assert(settingsOpen && settingsPanel.page == SETTINGS_PAGE_INPUT);

    /* The input method (the input page's first row): the key bindings page and the hints follow it. With keyboard and
       mouse the hints are keys; with the controller its buttons, and mouse sensitivity greyed out (a controller skips it). */
    shows(@"Input method", @"Keyboard & mouse");
    assert(!settingsPanel.usingPad && hint(@"Esc") && hint(@"↩") && !hint(@"B"));
    assert(row(@"Mouse sensitivity").enabled);
    pick(@"Input method", @"Controller");
    assert(g_settings_bindings_device == 1 && settings_keys_device() == 1 && !strcmp(savedName, "settings-keys-device.txt") && !strcmp(savedWord, "1"));
    assert(settingsPanel.usingPad && hint(@"A") && hint(@"B") && !hint(@"Esc"));
    assert(!row(@"Mouse sensitivity").enabled);
    focus_down_to(@"Mouse camera");
    pad(PAD_DOWN); assert([focused_title() isEqualToString:@"Input method"]);   /* the greyed-out row is skipped */
    [settingsPanel focusTarget:band(@"Mouse sensitivity")]; assert([focused_title() isEqualToString:@"Input method"]);
    [band(@"Mouse sensitivity") mouseDown:mouse(NSEventTypeLeftMouseDown)]; assert([focused_title() isEqualToString:@"Input method"] && fakeSensitivity == 50);
    [row(@"Mouse sensitivity") stepBy:1]; assert(fakeSensitivity == 50);
    pad(PAD_RIGHT); assert(settings_keys_device() == 0 && !settingsPanel.usingPad && hint(@"Esc"));   /* right: the other choice */
    assert(row(@"Mouse sensitivity").enabled);
    focus_down_to(@"Mouse camera");
    pad(PAD_DOWN); assert([focused_title() isEqualToString:@"Mouse sensitivity"]);
    assert(!settingsPanel.usingPad && hint(@"Esc") && !hint(@"B"));   /* the controller used last: still the keys */

    /* Mouse sensitivity: a slider from 1 to 100, at once (left / right 5); greyed out while TOTK_MOUSE_SENSITIVITY decides. */
    TOTKSettingRow *sensitivity = row(@"Mouse sensitivity");
    assert(sensitivity.slider && sensitivity.sliderMin == 1 && sensitivity.sliderMax == 100 && sensitivity.value == 50);
    [sensitivity apply:80]; assert(fakeSensitivity == 80 && sensitivity.value == 80);
    pad(PAD_RIGHT); assert(fakeSensitivity == 85); pad(PAD_LEFT); pad(PAD_LEFT); assert(fakeSensitivity == 75);
    [sensitivity apply:98]; pad(PAD_RIGHT); assert(fakeSensitivity == 100); pad(PAD_A); assert(fakeSensitivity == 100);
    fakeSensitivity = 30; [settingsPanel.timer fire]; assert(sensitivity.value == 30);
    press(18); fakeSensitivityLocked = 1; press(18);
    assert(!row(@"Mouse sensitivity").enabled && [row(@"Mouse sensitivity").text isEqualToString:@"30"]);
    press(18); fakeSensitivityLocked = 0; fakeSensitivity = 50; press(18); assert(settingsOpen);

    /* Key bindings, keyboard and mouse: a card per action with its first key; a click (or A) waits for the new key (one in
       use swaps places), key 1 and Esc cancel, Command keys pass, a right-click clears, Y / Delete restores the default. */
    [settingsPanel selectPage:SETTINGS_PAGE_BINDINGS]; tick();
    assert(!settingsPanel.drawings && !settingsPanel.cardsPage.hidden && (!settingsPanel.padPage || settingsPanel.padPage.hidden));
    assert(panel_size(SETTINGS_WIDE_W, SETTINGS_WIDE_H) && settingsPanel.tabs.selectedSegment == SETTINGS_PAGE_BINDINGS);
    assert(settingsPanel.rows.count == 0 && settingsPanel.cards.count == 24 && settingsPanel.controllerTargets.count == 24 && picked_at() == 0);
    assert(settingsPanel.cards[0].gameAction == KEYMAP_Y && hint(@"⌫") && hint(@"↩") && hint(@"Esc"));
    for (TOTKKeyCard *c in settingsPanel.cards) assert(c.gameAction != SETTINGS_BIND_PANEL && !c.waiting);   /* key 1 is fixed */
    fakePadDetected = -1; fakePadType = GAMEPAD_TYPE_AUTO; [settingsPanel refreshBindings];
    KeyMap keysNow; runtime_keymap_get(&keysNow);
    assert(keysNow.slot[KEYMAP_X][0] == 49 && [settings_cap_text(49) isEqualToString:@"Space"] && [settings_action_name(KEYMAP_X) isEqualToString:@"Jump"]);
    assert(keysNow.slot[KEYMAP_Y][0] == KEYMAP_MOUSE_FIRST && keysNow.slot[KEYMAP_A][0] == 14 && [settings_cap_text(14) isEqualToString:@"E"]);
    assert(keysNow.slot[KEYMAP_MOVE_UP][0] == 13 && [settings_cap_text(13) isEqualToString:@"W"]);
    [settingsPanel beginCapture:card(KEYMAP_X)];
    assert(settingsPanel.capturing == card(KEYMAP_X) && card(KEYMAP_X).waiting && card(KEYMAP_X).focus && hint(@"Esc") && !hint(@"↩"));
    assert([settingsPanel captured:key_down(12, 0)] == nil);   /* Q locked on: it jumps now, lock on takes Space */
    runtime_keymap_get(&keysNow);
    assert(keysNow.slot[KEYMAP_X][0] == 12 && keysNow.slot[KEYMAP_ZL][0] == 49 && !settingsPanel.capturing && !strcmp(savedName, "keyboard.txt"));
    assert(!card(KEYMAP_X).waiting && hint(@"↩"));
    press(18); assert(!settingsOpen); key_event(12, YES); assert(heldOut.buttons == 1u << KEYMAP_X); key_event(12, NO); press(18); assert(settingsOpen);
    assert(settingsPanel.page == SETTINGS_PAGE_BINDINGS && !settingsPanel.drawings);
    [settingsPanel beginCapture:card(KEYMAP_B)];
    NSEvent *command = key_down(12, NSEventModifierFlagCommand); assert([settingsPanel captured:command] == command && settingsPanel.capturing);
    NSEvent *rightShift = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:NSEventModifierFlagShift timestamp:0
                                        windowNumber:window.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:60];
    assert([settingsPanel captured:rightShift] == nil); runtime_keymap_get(&keysNow); assert(keysNow.slot[KEYMAP_B][0] == 60);
    [settingsPanel beginCapture:card(KEYMAP_A)]; assert([settingsPanel captured:key_down(18, 0)] == nil && !settingsPanel.capturing);   /* key 1 cancels */
    [settingsPanel beginCapture:card(KEYMAP_A)]; assert([settingsPanel captured:key_down(53, 0)] == nil && !settingsPanel.capturing);   /* so does Esc */
    runtime_keymap_get(&keysNow); assert(keysNow.slot[KEYMAP_A][0] == 14);
    [card(KEYMAP_Y) mouseDown:mouse(NSEventTypeLeftMouseDown)]; assert(settingsPanel.capturing == card(KEYMAP_Y) && card(KEYMAP_Y).focus);   /* a click */
    assert([settingsPanel captured:mouse(NSEventTypeRightMouseDown)] == nil); runtime_keymap_get(&keysNow);
    assert(keysNow.slot[KEYMAP_Y][0] == KEYMAP_MOUSE_FIRST + 1 && keysNow.slot[KEYMAP_ZR][0] == KEYMAP_MOUSE_FIRST);   /* the bow takes the left click */
    [card(KEYMAP_Y) rightMouseDown:mouse(NSEventTypeRightMouseDown)]; runtime_keymap_get(&keysNow);
    assert(keysNow.slot[KEYMAP_Y][0] == KEYMAP_NONE && !settingsPanel.capturing);
    /* A controller on the cards: down a row (two cards), right the next card, A waits for a key, B cancels the wait (the
       panel stays), Y the default layout, R1 / L1 the pages. */
    [settingsPanel selectPage:SETTINGS_PAGE_BINDINGS]; assert(picked_at() == 0);
    pad(PAD_DOWN); assert(picked_at() == 2); pad(PAD_RIGHT); assert(picked_at() == 3); pad(PAD_UP); assert(picked_at() == 1);
    pad(PAD_A); assert(settingsPanel.capturing == settingsPanel.cards[1] && settingsPanel.cards[1].waiting);
    pad(PAD_B); assert(settingsOpen && !settingsPanel.capturing && !settingsPanel.cards[1].waiting);
    pad(PAD_Y); runtime_keymap_get(&keysNow);
    KeyMap defaults; keymap_default(&defaults); assert(!memcmp(&keysNow, &defaults, sizeof defaults) && !strcmp(removedName, "keyboard.txt"));
    pad(PAD_R1); assert(settingsPanel.page == SETTINGS_PAGE_DISPLAY);
    pad(PAD_L1); assert(settingsPanel.page == SETTINGS_PAGE_BINDINGS);

    /* Key bindings, controller (the input method; the window has room): the controller photo in the same panel, a label
       per button with what it drives (the two paddles not set); A on a label waits for the new button (a banner says for
       what), which swaps with it (its mode too; the settings panel's button moves with it); B cancels; Y the default. */
    g_settings_bindings_device = 1; [settingsPanel refresh];   /* the input method changed: the photo */
    assert(settingsPanel.drawings && settingsPanel.page == SETTINGS_PAGE_BINDINGS && !settingsPanel.padPage.hidden && settingsPanel.cardsPage.hidden);
    assert(panel_size(SETTINGS_WIDE_W, SETTINGS_WIDE_H) && NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    assert(settingsPanel.padLabels.count == 14 && settingsPanel.controllerTargets.count == 12 && picked_at() == 0);
    assert(settingsPanel.padLabels[12].physical == -1 && settingsPanel.padLabels[13].physical == -1 && !settingsPanel.padLabels[12].text);
    assert(settingsPanel.usingPad && hint(@"Y") && hint(@"A") && hint(@"B") && settingsPanel.banner.hidden);
    for (NSUInteger i = 0; i < 12; i++) { TOTKPadLabel *l = settingsPanel.padLabels[i]; assert(labelled(l.physical, drives(l.physical))); }
    assert(labelled(3, @"Jump") && labelled(8, @"Lock on / Shield") && settingsPanel.padLabels[0].physical == 8);   /* Switch X sits north */
    fakeModes[8] = CONTROLLER_TOGGLE;
    pad(PAD_A); assert(settingsPanel.padCapturing == label(8) && label(8).waiting && !fakeSettingsShortcut);
    assert(!settingsPanel.banner.hidden && [settingsPanel.bannerText.stringValue isEqualToString:@"Press the new button for “Lock on / Shield”"]);
    assert(labelled(8, @"Lock on / Shield") && hint(@"B") && !hint(@"A"));
    pad(PAD_L1);   /* L1 now locks on, ZL takes the ability */
    assert(fakeMap[6] == 1u << KEYMAP_ZL && fakeMap[8] == 1u << KEYMAP_L && fakeModes[6] == CONTROLLER_TOGGLE && fakeModes[8] == CONTROLLER_HOLD);
    assert(!settingsPanel.padCapturing && !label(8).waiting && fakeSettingsShortcut && settingsPanel.banner.hidden);
    assert(labelled(6, @"Lock on / Shield") && labelled(8, drives(8)) && hint(@"A"));
    pad(PAD_A); assert(settingsPanel.padCapturing == label(8)); pad(PAD_B);   /* B: no new button */
    assert(settingsOpen && !settingsPanel.padCapturing && fakeMap[8] == 1u << KEYMAP_L && fakeSettingsShortcut && settingsPanel.banner.hidden);
    pad(PAD_DOWN); assert(picked_at() == 1 && settingsPanel.controllerTargets[1] == label(6));
    [settingsPanel beginPadCapture:settingsPanel.padLabels[12]]; assert(!settingsPanel.padCapturing);   /* a paddle: nothing to change */
    /* The settings panel's own button (user 2026-10-29): it moves with a swap either way. */
    runtime_gamepad_settings_button_set(10); [settingsPanel refreshBindings]; assert(labelled(10, @"Settings panel"));
    [label(10) mouseDown:mouse(NSEventTypeLeftMouseDown)];   /* a click */
    assert(settingsPanel.padCapturing == label(10) && label(10).focus && [settingsPanel.bannerText.stringValue isEqualToString:@"Press the new button for “Settings panel”"]);
    [settingsPanel bindPad:11]; assert(fakeSettingsButton == 11 && labelled(11, @"Settings panel") && labelled(10, @"Map"));
    [settingsPanel beginPadCapture:label(3)]; [settingsPanel bindPad:11]; assert(fakeSettingsButton == 3 && labelled(3, @"Settings panel") && labelled(11, @"Jump"));
    pad(PAD_Y);
    uint16_t defaultMap[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaultMap, defaultModes);
    assert(!memcmp(fakeMap, defaultMap, sizeof defaultMap) && !memcmp(fakeModes, defaultModes, sizeof defaultModes) && fakeSettingsButton == -1);
    assert(labelled(3, @"Jump") && labelled(10, drives(10)));
    runtime_keymap_get(&keysNow); assert(!memcmp(&keysNow, &defaults, sizeof defaults));   /* the keyboard's layout untouched */
    pad(PAD_R1); assert(settingsPanel.page == SETTINGS_PAGE_DISPLAY && panel_size(SETTINGS_WIDE_W, SETTINGS_WIDE_H));
    pad(PAD_L1); assert(settingsPanel.page == SETTINGS_PAGE_BINDINGS && settingsPanel.drawings);
    /* A window too small for the photo: the smaller panel, the cards even with the controller. */
    press(18); [window setContentSize:NSMakeSize(1728, 486)]; press(18);
    assert(settingsOpen && settingsPanel.page == SETTINGS_PAGE_BINDINGS && !settingsPanel.drawings && !settingsPanel.padPage && !settingsPanel.cardsPage.hidden);
    assert(settingsPanel.frame.size.width == SETTINGS_W && settingsPanel.controllerTargets.count == 24 && settingsPanel.padLabels.count == 0);
    press(18); [window setContentSize:NSMakeSize(1280, 720)]; g_settings_bindings_device = 0; g_settings_page = SETTINGS_PAGE_DISPLAY; press(18); assert(settingsOpen);

    /* The overlay keeps its build line (user 2026-10-25; after PR #1210). */
    setenv("TOTK_SOURCE_COMMIT", "abc1234", 1);
    if (hudBox) { [(TOTKHudBox *)hudBox refresh]; assert([((TOTKHudBox *)hudBox).label.stringValue containsString:@"abc1234"]); }

    press(18); assert(!settingsOpen);

    /* Chinese. */
    setenv("TOTK_UI_LANG", "zh", 1); press(18); assert(settingsOpen);
    shows(@"分辨率", @"1080p"); shows(@"手柄类型", @"自动（未接手柄）");
#ifdef GAME_BOTW160
    assert(!find_row(@"DualSense 拉弓扳机"));
#else
    assert(switched(@"DualSense 拉弓扳机"));
#endif
    fakePadDetected = GAMEPAD_TYPE_SWITCH; shows(@"手柄类型", @"自动（Switch）");
    pick(@"补帧", @"50 → 100"); assert(mfx_requested() == MFX_50);
    assert(row(@"震动").bars); shows(@"震动", @"1×"); assert(switched(@"体感瞄准"));

    if (argc == 2) {
        NSBitmapImageRep *rep = [settingsPanel bitmapImageRepForCachingDisplayInRect:settingsPanel.bounds];
        [settingsPanel cacheDisplayInRect:settingsPanel.bounds toBitmapImageRep:rep];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES]);
    }
    [settingsPanel.timer invalidate];
    puts("pass: settings panel (key 1 / Esc, one size, focus, overlay, frame lock, interpolation, resolution 1080p / MetalFX / 2K / 4K and window size, aspect 16:9 / 16:10 / 21:9 / 32:9, switches, controller navigation, controller type, rumble bars / locked rumble / gyro aiming, input method hints, sensitivity, key cards, controller photo swap, game keys blocked while open, keys 2-7 inert, Chinese)");
    return 0;
}}
