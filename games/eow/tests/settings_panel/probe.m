/* The production Mac settings panel of Echoes of Wisdom (key 1: shared/runtime/settings_panel.h with runtime/settings_game.h),
   its keyboard handling and the rumble switch (runtime/hid_feedback.c), as games/acnh/tests/settings_panel/probe.m does for
   Animal Crossing. No guest, save files, game data or GPU commands are used; tests/test_eow_settings_panel.py builds and
   runs it. Its own folder (SETTINGS_TEST_DIR) holds the setting files.
   Modes: none, the panel; "restore", the files the first run left read back at the start of a run. */
#include "../../runtime/frontend.m"
#include "../../runtime/input.c"
#include <assert.h>

uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "runtime_fail: %s\n", message); abort(); }
/* runtime/gamepad.m's setting files, in SETTINGS_TEST_DIR. */
static char savedName[64], savedWord[16], removedName[64];
static void setting_file(const char *name, char *path, size_t size) { snprintf(path, size, "%s/%s", getenv("SETTINGS_TEST_DIR"), name); }
void runtime_setting_save(const char *name, const char *word, const char *what) {
    (void)what; snprintf(savedName, sizeof savedName, "%s", name); snprintf(savedWord, sizeof savedWord, "%s", word);
    char path[4096]; setting_file(name, path, sizeof path);
    FILE *f = fopen(path, "w"); assert(f); assert(fprintf(f, "%s\n", word) > 0); assert(!fclose(f));
}
int runtime_setting_word(const char *name, char *word, size_t size) {
    char path[4096]; setting_file(name, path, sizeof path); FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(word, 1, size - 1, f); word[n] = 0; fclose(f); return 1;
}
void runtime_setting_remove(const char *name) {
    snprintf(removedName, sizeof removedName, "%s", name);
    char path[4096]; setting_file(name, path, sizeof path); unlink(path);
}
static NSString *setting_text(const char *name) {
    char path[4096]; setting_file(name, path, sizeof path);
    return [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
}
/* runtime/gamepad.m without GameController: a DualSense is connected. */
static int fakePadType = GAMEPAD_TYPE_AUTO, fakePadDetected = GAMEPAD_TYPE_PLAYSTATION;
int runtime_gamepad_type_set(int type) { return fakePadType = type; }
int runtime_gamepad_type(int *detected) { if (detected) *detected = fakePadDetected; return fakePadType; }
int runtime_gamepad_battery_percent(void) { return -1; }
int runtime_gamepad_battery_left_percent(void) { return -1; }
void runtime_gamepad_clear(void) {}
void runtime_gamepad_text_read(RuntimeGamepadState *out) { *out = (RuntimeGamepadState){0}; }
static int fakeSettingsCapture, fakeSettingsShortcut = 1, fakeSettingsButton = -1;
void runtime_gamepad_text_capture(int on) { fakeSettingsCapture = !!on; }
void runtime_gamepad_settings_enable(int on) { fakeSettingsShortcut = on; }
int runtime_gamepad_settings_button(void) { return fakeSettingsButton; }
void runtime_gamepad_settings_button_set(int physical) { fakeSettingsButton = physical < 0 ? -1 : physical; }
static uint16_t fakeMap[CONTROLLER_BUTTONS]; static uint8_t fakeModes[CONTROLLER_BUTTONS];
void runtime_controller_map_get(uint16_t *map, uint8_t *modes) { memcpy(map, fakeMap, sizeof fakeMap); memcpy(modes, fakeModes, sizeof fakeModes); }
void runtime_controller_map_set(const uint16_t *map, const uint8_t *modes) { memcpy(fakeMap, map, sizeof fakeMap); memcpy(fakeModes, modes, sizeof fakeModes); }
void runtime_controller_map_reset(void) { controller_map_default(fakeMap, fakeModes); }
/* gamepad.m's screenshot button (held 3 s; -1 none) and the capture button's request (input.c's P). */
static int fakeShotButton = CONTROLLER_SCREENSHOT_DEFAULT, capturePresses;
int runtime_gamepad_screenshot_button(void) { return fakeShotButton; }
void runtime_gamepad_screenshot_button_set(int physical) { fakeShotButton = physical < 0 ? -1 : physical; }
void runtime_capture_press(void) { capturePresses++; }
/* gamepad.m's DSU client (dsu_client.h), its settings kept here. */
static RuntimeDSUSettings fakeDSU = {.endpoint = "127.0.0.1:26760"}; static int dsuApplies;
void runtime_dsu_settings(RuntimeDSUSettings *out) { *out = fakeDSU; }
void runtime_dsu_status(RuntimeDSUStatus *out) { *out = (RuntimeDSUStatus){0}; }
int runtime_dsu_apply(const RuntimeDSUSettings *settings) { fakeDSU = *settings; dsuApplies++; return 0; }
/* The 排行榜 page's view of the game's pacing and picture (hle_nvn.c, nvn_mtl.m). */
void nvn_board_pacing(int *lock, int *interp) { *lock = 0; *interp = 0; }
void mtl_board_view(int *ow, int *oh, int *sw, int *sh, int *mfx) { *ow = 1920; *oh = 1080; *sw = 1920; *sh = 1080; *mfx = 0; }
int mtl_display_hz(void) { return 60; }
/* hid_feedback.c's rumble turned off while the game has its devices: the motors are stopped. */
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) { (void)side; (void)al; (void)fl; (void)ah; (void)fh; }
/* A window key is the user's input (input.c user_input): it stops a survey and an auto-load, neither here. */
void runtime_survey_cancel_user(void) {}
void runtime_autoload_user_input(void) {}

static void tick(void) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]]; }
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
    if (!p.enabled || [p indexOfItemWithTitle:choice] < 0) { fprintf(stderr, "%s has no choice %s\n", title.UTF8String, choice.UTF8String); abort(); }
    [p selectItemWithTitle:choice]; [row(title) changed:p];
}
static void flip(NSString *title, BOOL on) {
    NSSwitch *s = (NSSwitch *)row(title).control;
    s.state = on ? NSControlStateValueOn : NSControlStateValueOff; [row(title) changed:s];
}
static BOOL switched(NSString *title) { [settingsPanel.timer fire]; return ((NSSwitch *)row(title).control).state == NSControlStateValueOn; }
static void shows(NSString *title, NSString *choice) {
    [settingsPanel.timer fire];
    NSString *now = menu(title).titleOfSelectedItem;
    if (![now isEqualToString:choice]) { fprintf(stderr, "%s shows %s, expected %s\n", title.UTF8String, now.UTF8String, choice.UTF8String); abort(); }
}
static NSArray<NSString *> *choices(NSString *title) { return menu(title).itemTitles; }
static NSArray<NSString *> *titles(NSInteger page) {
    NSMutableArray *out = [NSMutableArray array];
    for (TOTKSettingRow *r in settingsPanel.allRows) if (r.page == page) [out addObject:r.title];
    return out;
}
static BOOL same_titles(NSInteger page, NSArray *want) {
    if ([titles(page) isEqualToArray:want]) return YES;
    fprintf(stderr, "page %ld rows: %s\n", (long)page, [titles(page) componentsJoinedByString:@" | "].UTF8String); return NO;
}
static TOTKBindingButton *box(int action, int slot) {
    for (TOTKBindingButton *b in settingsPanel.bindingButtons) if (b.gameAction == action && b.slot == slot) return b;
    fprintf(stderr, "no box %d/%d\n", action, slot); abort();
}
static BOOL titled(NSButton *b, NSString *text) {
    if ([b.title isEqualToString:text]) return YES;
    fprintf(stderr, "box shows %s, expected %s\n", b.title.UTF8String, text.UTF8String); return NO;
}
static int homeOpens; static NSURL *homeOpened;
static void probe_home_open(NSURL *url) { homeOpens++; homeOpened = url; }

static void open_window(void) {
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView = [[TOTKInputView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
}

/* "restore": what the panel saved is in force from the start of the next run (settings_restore_start). */
static void restore_probe(void) {
    assert(fps_overlay_hud_mode() == HUD_MODE_ALL && !mfxup_enabled() && !mfxup_output_enabled());   /* before: the defaults */
    settings_restore_start();
    assert(fps_overlay_hud_mode() == HUD_MODE_FULL);   /* FPS full information is kept (graphics-overlay.txt 3) */
    assert(mfxup_enabled() && mfxup_output_enabled());   /* the picture: MetalFX upscaling (graphics-output.txt 1) */
    /* The key bindings the first run changed: keyboard.txt read back on the first key (input.c keymap_load_locked). */
    KeyMap m; runtime_keymap_get(&m); assert(m.slot[KEYMAP_A][0] == 46 && m.slot[KEYMAP_A][1] == KEYMAP_NONE);   /* M */
}

int main(int argc, const char **argv) { @autoreleasepool {
    [NSApplication sharedApplication];
    assert(getenv("SETTINGS_TEST_DIR"));
    controller_map_default(fakeMap, fakeModes); setenv("TOTK_UI_LANG", "en", 1); unsetenv("TOTK_SOURCE_COMMIT");
    setenv("TOTK_BOARD_URL", "http://127.0.0.1:9/v1/board", 1);   /* the 排行榜 page reads no real board in a test */
    setenv("TOTK_SHOT_URL", "http://127.0.0.1:9/v1/shots", 1);    /* nor the 截图分享 page real pictures */
    setenv("TOTK_NAME_URL", "http://127.0.0.1:9/v1/name", 1);     /* nor a real nickname */
    setenv("TOTK_STATS_URL", "http://127.0.0.1:9", 1);            /* and 访问主页 names a local site */
    unsetenv("TOTK_BOARD_DIR");                                   /* this run takes part in neither */
    if (argc == 2 && !strcmp(argv[1], "restore")) { restore_probe(); puts("pass: settings restore"); return 0; }

    playtime = [[TOTKPlaytime alloc] initWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
    open_window();
    if (hud_compact()) { TOTKHudBox *b = [[TOTKHudBox alloc] initWithFrame:NSZeroRect]; [window.contentView addSubview:b]; hudBox = b; }

    /* Key 1 opens it (it toggled the reminder box before); a held key acts once. */
    runtime_input_key_synthetic(18, 1); runtime_input_key_synthetic(18, 1); tick(); runtime_input_key_synthetic(18, 0); tick();
    assert(settingsOpen && fakeSettingsCapture && settingsPanel && !settingsPanel.hidden && settingsPanel.superview == window.contentView);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    /* The Zelda games' opacity (0.9): this game sets no SETTINGS_PANEL_ALPHA of its own. */
    assert(fabs(CGColorGetAlpha(settingsPanel.layer.backgroundColor) - 0.9) < 1e-6);

    /* The tabs (2026-11-17, as the Zelda games, without a User page, the shader contribution or VR). */
    assert(settings_pages() == 6 && settingsPanel.tabs.segmentCount == 6);
    { NSArray *labels = @[@"Display", @"Input", @"Effects", @"Key bindings", @"Performance", @"Screenshots"];
      for (NSInteger t = 0; t < 6; t++) assert([[settingsPanel.tabs labelForSegment:t] isEqualToString:labels[(NSUInteger)t]]); }
    assert(!settings_page_present(SETTINGS_PAGE_USERS) && !settings_page_present(SETTINGS_PAGE_CONTRIB) && !settings_page_present(SETTINGS_PAGE_VR));
    assert(settings_tab_page(2) == SETTINGS_PAGE_EFFECTS && settings_tab_page(3) == SETTINGS_PAGE_BINDINGS &&
           settings_tab_page(4) == SETTINGS_PAGE_BOARD && settings_tab_page(5) == SETTINGS_PAGE_SHOTS);

    /* Echoes of Wisdom's rows: Display 4, Input 6 (controller type, rumble, the 4 DSU rows), Effects 1 (the picture). */
    { NSArray *display = @[@"Overlay", @"FPS graph", @"Frame rate", @"Frame interpolation (MetalFX)"];
      assert(same_titles(SETTINGS_PAGE_DISPLAY, display));
      NSArray *input = @[@"Controller type", @"Rumble", @"DSU input", @"DSU server", @"DSU controller slot", @"DSU status"];
      assert(same_titles(SETTINGS_PAGE_INPUT, input));
      assert(same_titles(SETTINGS_PAGE_EFFECTS, @[@"Picture"]));
      assert(settingsPanel.allRows.count == 11); }

    /* The overlay: the Zelda games' four choices; FPS + playtime and the corner box. */
    assert([choices(@"Overlay") isEqualToArray:(@[@"Off", @"Bare FPS", @"FPS + playtime", @"All", @"FPS and frame debug info"])]);   /* user 2026-11-18 */
    shows(@"Overlay", @"All");
    hud_publish(HUD_FPS, "FPS 60  1920x1080", 0);
    pick(@"Overlay", @"FPS + playtime"); assert(fps_overlay_hud_mode() == HUD_MODE_MINIMAL && !strcmp(savedWord, "2"));
    if (hudBox) { TOTKHudBox *b = (TOTKHudBox *)hudBox; assert(!b.hidden && [b.label.stringValue hasPrefix:@"FPS 60  1920x1080\nSession"] && [b.label.stringValue containsString:@"Settings: key 1"]); }
    pick(@"Overlay", @"Off"); assert(fps_overlay_hud_mode() == HUD_MODE_OFF && (!hudBox || hudBox.hidden));
    pick(@"Overlay", @"FPS and frame debug info"); assert(fps_overlay_hud_mode() == HUD_MODE_FULL && !strcmp(savedWord, "3"));
    assert(hud_line_shown(HUD_HITCHES) && hud_line_shown(HUD_HITCH0));   /* the long frames hle_nvn.c notes (FPS full information) */
    flip(@"FPS graph", YES); assert(fpsGraphEnabled && [setting_text("fps-graph.txt") isEqualToString:@"on\n"]); flip(@"FPS graph", NO); assert(!fpsGraphEnabled);

    /* The frame rate and frame interpolation: one value each, greyed out (nothing this game can change). */
    assert(!menu(@"Frame rate").enabled && [menu(@"Frame rate").titleOfSelectedItem isEqualToString:@"60 fps (the game's own pace)"]);
    assert(!menu(@"Frame interpolation").enabled && [menu(@"Frame interpolation").titleOfSelectedItem isEqualToString:@"Not available in this game yet"]);

    /* Input: the controller type, the rumble switch (hid_feedback.c's, rumble.txt; on by default, TOTK_RUMBLE=0 turns it
       off) and the DSU rows. */
    shows(@"Controller type", @"Auto (PlayStation)"); pick(@"Controller type", @"Switch"); assert(fakePadType == GAMEPAD_TYPE_SWITCH);
    pick(@"Controller type", @"Auto (PlayStation)");
    assert(switched(@"Rumble")); flip(@"Rumble", NO); assert(!runtime_feedback_choice(0) && [setting_text("rumble.txt") isEqualToString:@"off\n"]);
    flip(@"Rumble", YES); assert(runtime_feedback_choice(0) && [setting_text("rumble.txt") isEqualToString:@"on\n"]);
    assert(!switched(@"DSU input")); flip(@"DSU input", YES); assert(fakeDSU.enabled && dsuApplies == 1); flip(@"DSU input", NO); assert(!fakeDSU.enabled);
    pick(@"DSU controller slot", @"2"); assert(fakeDSU.slot == 2); pick(@"DSU controller slot", @"0");
    [settingsPanel.timer fire]; assert([row(@"DSU status").textLabel.stringValue isEqualToString:@"DSU · Off"]);

    /* Effects: the picture (MetalFX upscaling of the present) and Reset to defaults, which gives the plain scaling back. */
    [settingsPanel selectPage:SETTINGS_PAGE_EFFECTS];
    NSButton *resetEffects = settingsPanel.effectsButtons.firstObject;
    assert(resetEffects && !resetEffects.hidden && [resetEffects.title isEqualToString:@"Reset to defaults"] &&
           settingsPanel.controllerTargets.count == 3 && settingsPanel.controllerTargets[1] == resetEffects);
    shows(@"Picture", @"1080p");
    pick(@"Picture", @"1080p + MetalFX upscaling (large window, full screen)");
    assert(mfxup_enabled() && mfxup_output_enabled() && [setting_text("graphics-output.txt") isEqualToString:@"1\n"]);
    removedName[0] = 0; [resetEffects performClick:nil]; tick(); tick();
    assert(settingsOpen && settingsPanel.page == SETTINGS_PAGE_EFFECTS);   /* made again so the row says what is in force */
    assert(!mfxup_enabled() && !mfxup_output_enabled() && !strcmp(removedName, "graphics-output.txt") && !setting_text("graphics-output.txt"));
    shows(@"Picture", @"1080p");
    pick(@"Picture", @"1080p + MetalFX upscaling (large window, full screen)");   /* kept for the "restore" run */

    /* Key bindings: this game's layout (the fixed one it had: J is A, K and Space B), the screenshot row (P, and the touchpad
       held 3 s); another button takes the hold, a right-click clears it, the controller reset gives the touchpad back. P
       takes a picture once the panel is closed and drives nothing in the game. */
    g_settings_bindings_list = YES; [settingsPanel selectPage:SETTINGS_PAGE_BINDINGS]; tick();
    assert(!settingsPanel.drawings && settingsPanel.page == SETTINGS_PAGE_BINDINGS);
    fakePadType = GAMEPAD_TYPE_AUTO; [settingsPanel refreshBindings];
    assert(titled(box(KEYMAP_SCREENSHOT, 0), @"P") && titled(box(KEYMAP_SCREENSHOT, 1), @"—") && titled(box(KEYMAP_SCREENSHOT, 2), @"Touchpad held 3 s"));
    assert(titled(box(KEYMAP_A, 0), @"J") && titled(box(KEYMAP_A, 1), @"—") && titled(box(KEYMAP_B, 0), @"K") && titled(box(KEYMAP_B, 1), @"Space"));
    assert(titled(box(SETTINGS_BIND_PANEL, 0), @"1"));
    [settingsPanel beginCapture:box(KEYMAP_SCREENSHOT, 2)]; [settingsPanel bindPad:11]; assert(fakeShotButton == 11);
    [settingsPanel clearBinding:box(KEYMAP_SCREENSHOT, 2)]; assert(fakeShotButton == -1 && titled(box(KEYMAP_SCREENSHOT, 2), @"—"));
    [settingsPanel resetController:nil]; assert(fakeShotButton == CONTROLLER_SCREENSHOT_DEFAULT);
    /* A key binding is in force at once and saved: M takes A from J (keyboard.txt, read back by the "restore" run). */
    { KeyMap m; runtime_keymap_get(&m); assert(keymap_bind(&m, KEYMAP_A, 0, 46)); runtime_keymap_set(&m);
      assert([setting_text("keyboard.txt") containsString:@"\na = m\n"]); [settingsPanel refreshBindings]; assert(titled(box(KEYMAP_A, 0), @"M")); }
    press(18); assert(!settingsOpen);
    key_event(35, YES); assert(capturePresses == 1 && !heldOut.buttons && !heldOut.lx && !heldOut.ly); key_event(35, NO);
    key_event(46, YES); assert(heldOut.buttons == 1); key_event(46, NO);   /* M is A now */
    key_event(38, YES); assert(!heldOut.buttons); key_event(38, NO);     /* J drives nothing any more */
    key_event(49, YES); assert(heldOut.buttons == 2); key_event(49, NO);   /* Space is still B */
    key_event(13, YES); assert(heldOut.ly == 32767 && !heldOut.lx); key_event(13, NO);   /* W moves forward */
    /* Keys 2-4 drive nothing (they were shortcuts before the panel). */
    for (unsigned short k = 19; k <= 21; k++) { key_event(k, YES); assert(!heldOut.buttons && !settingsOpen); key_event(k, NO); }
    press(18); assert(settingsOpen && settingsPanel.page == SETTINGS_PAGE_BINDINGS);
    /* The drawings: the screenshot is one of the controller buttons' actions. */
    { NSMenu *actions = [settingsPanel menuForInput:3 pad:YES]; BOOL shot = NO;
      for (NSMenuItem *i in actions.itemArray) if (i.action && i.tag == KEYMAP_SCREENSHOT) shot = [i.title isEqualToString:@"Screenshot (hold 3 s)"];
      assert(shot); }

    /* 性能排行榜 and 截图分享: a run without the launcher's folder takes part in neither, and their switches stay off;
       the 截屏 button is the capture button's own request. 访问主页 names this game. */
    [settingsPanel selectPage:SETTINGS_PAGE_BOARD];
    { TOTKBoardPage *board = (TOTKBoardPage *)settingsPanel.pages[SETTINGS_PAGE_BOARD];
      assert([board isKindOfClass:TOTKBoardPage.class] && !board.hidden && !board.toggle.enabled && [board.state.stringValue isEqualToString:@"This run takes no part in the board"]); }
    [settingsPanel selectPage:SETTINGS_PAGE_SHOTS];
    { TOTKShotsPage *shots = (TOTKShotsPage *)settingsPanel.pages[SETTINGS_PAGE_SHOTS];
      assert([shots isKindOfClass:TOTKShotsPage.class] && !shots.hidden && !shots.toggle.enabled && [shots.state.stringValue isEqualToString:@"This run shares no screenshots"]);
      assert(shot_tab_take == screenshot_request); }
    settings_home_open = probe_home_open; [settingsPanel.homeButton performClick:nil]; settings_home_open = settings_home_open_default;
    assert(homeOpens == 1 && [homeOpened.host isEqualToString:@"127.0.0.1"] && [homeOpened.query containsString:@"game=eow"]);

    /* L1 / R1 go round the six tabs. */
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:100 active:YES];
    { const NSInteger order[] = {SETTINGS_PAGE_INPUT, SETTINGS_PAGE_EFFECTS, SETTINGS_PAGE_BINDINGS, SETTINGS_PAGE_BOARD, SETTINGS_PAGE_SHOTS, SETTINGS_PAGE_DISPLAY};
      for (int k = 0; k < 6; k++) {
        [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:100.2 + k active:YES];
        [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = 1u << 7} time:100.3 + k active:YES];
        if (settingsPanel.page != order[k]) { tick(); }   /* the bindings page may be made again on the next turn */
        assert(settingsPanel.page == order[k]);
      } }

    /* panelshot (the input file, runtime/input.c): the controller drawing as a PNG. */
    { NSString *png = [@(getenv("SETTINGS_TEST_DIR")) stringByAppendingPathComponent:@"panel.png"];
      [NSFileManager.defaultManager removeItemAtPath:png error:nil];
      runtime_settings_shot(3, png.UTF8String);
      for (int i = 0; i < 150 && ![NSFileManager.defaultManager fileExistsAtPath:png]; i++) tick();
      assert([NSFileManager.defaultManager fileExistsAtPath:png] && settingsPanel.page == SETTINGS_PAGE_BINDINGS && settingsPanel.drawings); }

    /* Esc closes it (and is the game's - button only while it is closed). */
    key_event(53, YES); key_event(53, NO); assert(!settingsOpen && settingsPanel.hidden);
    key_event(53, YES); assert(heldOut.buttons == 1u << KEYMAP_MINUS); key_event(53, NO);

    /* Chinese labels. */
    setenv("TOTK_UI_LANG", "zh", 1); press(18); assert(settingsOpen);
    shows(@"手柄类型", @"自动（PlayStation）"); assert([[settingsPanel.tabs labelForSegment:4] isEqualToString:@"性能排行榜"]);
    [settingsPanel.timer invalidate];
    puts("pass: settings panel (key 1 / Esc, six tabs, overlay and box, fixed frame rate and interpolation, rumble, DSU, effects and reset, key bindings and screenshot key and hold, board, screenshots, homepage, L1 / R1, panelshot, Chinese)");
    return 0;
}}
