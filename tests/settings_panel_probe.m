/* The production Mac settings panel (key 1, runtime/settings_panel.h), its keyboard handling and the calls each row makes.
   No guest, save files, game data or GPU commands are used. */
#import <Foundation/Foundation.h>
static char spawned[256];   /* the Update page's last request to the launcher (update_tab.h), never a real one */
static BOOL spawn_ok = 1;
static BOOL probe_spawn(const char *mode, const char *arg) { if (!spawn_ok) return 0; snprintf(spawned, sizeof spawned, "%s %s", mode, arg ? arg : ""); return 1; }
#define UPDATE_TAB_SPAWN probe_spawn
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
void settings_test_output_4k(int on);
void settings_test_aspect_init(double view);
void settings_test_aspect_frame(void);
#ifdef GAME_BOTW190
void settings_test_fov_refused(int refused); void settings_test_fov_init(void); uint32_t settings_test_fov_word(void);
static uint32_t float_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
#endif

void dm_gfx_set_flag(const char *name, int value) {
    if (!strcmp(name, "mfx_interp")) mfx_set(value);
    else if (!strcmp(name, "mfx_output")) mfxup_output_set(value);
    else if (!strcmp(name, "mfx_upscale")) mfxup_set(value);
    else abort();
}
/* gfx/nvn/totk_ssao.h's row calls (PR #1316): whether this run patched the AO shaders, and the level last set. */
static int fakeSsaoLive = 1, fakeSsaoSet = -1, fakeSsaoStart = -1;
void dm_gfx_ssao_start(int pct) { fakeSsaoStart = pct; }
int dm_gfx_ssao_locked(void) { return 0; }
int dm_gfx_ssao_live(void) { return fakeSsaoLive; }
void dm_gfx_ssao_set(int pct) { if (fakeSsaoLive) fakeSsaoSet = pct; }
/* gfx/nvn/totk_fx.h's effect switches (two of them here; 1.4.3 only); the two distances are game_patches_v143.c's own (the fixture). */
#ifdef TOTK_GAME_V143
#define TOTK_V143 1
#else
#define TOTK_V143 0
#endif
static const char *const fakeFxIds[2] = {"ao", "bloom"}, *const fakeFxNames[2][2] = {{"环境光遮蔽", "Ambient occlusion"}, {"泛光", "Bloom"}};
static int fakeFxOn[2] = {1, 1};
static int fakeAnisoStart = -1, fakeFxaa = 1, fakeSharpen;
void dm_gfx_aniso_start(int n) { fakeAnisoStart = n; }
int dm_gfx_aniso_locked(void) { return 0; }
static int fakeAniso;
int dm_gfx_aniso(void) { return fakeAniso; }
void dm_gfx_aniso_set(int n) { fakeAniso = n; }
void dm_gfx_fxaa_set(int on) { fakeFxaa = on; }
int dm_gfx_fxaa_on(void) { return fakeFxaa; }
void dm_gfx_sharpen_set(int pct) { fakeSharpen = pct; }
int dm_gfx_sharpen(void) { return fakeSharpen; }
int dm_gfx_fx_count(void) { return 2; }
const char *dm_gfx_fx_id(int i) { return fakeFxIds[i]; }
const char *dm_gfx_fx_name(int i, int english) { return fakeFxNames[i][english != 0]; }
int dm_gfx_fx_available(int i) { (void)i; return 1; }
int dm_gfx_fx_on(int i) { return fakeFxOn[i]; }
void dm_gfx_fx_set(int i, int on) { fakeFxOn[i] = on != 0; }
/* runtime/gamepad.m's controller type and mouse mode without GameController: a DualSense is connected. */
static int fakePadType = GAMEPAD_TYPE_AUTO, fakePadDetected = GAMEPAD_TYPE_PLAYSTATION, fakeMouse;
int runtime_gamepad_type_set(int type) { return fakePadType = type; }
int runtime_gamepad_type(int *detected) { if (detected) *detected = fakePadDetected; return fakePadType; }
int runtime_gamepad_battery_percent(void) { return -1; }
static int fakePadBackground;
int runtime_gamepad_background_enabled(void) { return fakePadBackground; }
void runtime_gamepad_background_set(int on) { fakePadBackground = !!on; }
int runtime_gamepad_battery_left_percent(void) { return -1; }
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
static char removedName[64], removedAll[512];   /* the last file removed, and every one since removedAll was cleared */
void runtime_setting_remove(const char *name) {
    snprintf(removedName, sizeof removedName, "%s", name);
    strlcat(removedAll, name, sizeof removedAll); strlcat(removedAll, " ", sizeof removedAll);
    const char *dir = getenv("SETTINGS_TEST_DIR"); if (!dir) return;
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name); unlink(path);
}
/* runtime/account.c's local users, in memory (the file store itself is tests/test_users.c). */
static RuntimeUser fakeUsers[RUNTIME_USERS_MAX];
static int fakeUsersCount, fakeUsersActive;
int runtime_users_get(RuntimeUser *out, int max) {
    int n = 0;
    for (int i = 0; i < fakeUsersCount && n < max; i++) out[n++] = fakeUsers[i];
    return n;
}
static const int fakeUsersRunning = 0;   /* the user this run opened with: choosing another does not change it */
int runtime_users_active(RuntimeUser *out) { *out = fakeUsers[fakeUsersRunning]; return 0; }
int runtime_users_next(RuntimeUser *out) { *out = fakeUsers[fakeUsersActive]; return 0; }   /* the next launch's */
int runtime_users_add(const char *name) {
    if (fakeUsersCount >= RUNTIME_USERS_MAX || !name || !*name) return -1;
    RuntimeUser *u = &fakeUsers[fakeUsersCount];
    snprintf(u->name, sizeof u->name, "%s", name);
    arc4random_buf(&u->lo, sizeof u->lo); arc4random_buf(&u->hi, sizeof u->hi);
    return fakeUsersCount++;
}
int runtime_users_choose(int index) { if (index < 0 || index >= fakeUsersCount) return -1; fakeUsersActive = index; return 0; }
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
/* The text bar's controller timer (the User page's add opens the bar here): a controller that is never connected. */
void runtime_gamepad_text_read(RuntimeGamepadState *out) { *out = (RuntimeGamepadState){0}; }
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
/* Key 1, a page that needs another panel and the aspect ratio's window size all act through the main queue. This returns when
   what is queued so far has run: the queue keeps its order, so a block queued behind them shows it. A fixed wait does not
   do: a run loop pass can use up the time before it looks at the queue (10-19 ms on an M5 Pro right after the panel with the
   drawings was made, more on a busy runner) and then leaves, the queue unserved. */
static void tick(void) {
    __block BOOL served = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ served = YES; });
    NSDate *limit = [NSDate dateWithTimeIntervalSinceNow:20];   /* within the test's 35 s for a run */
    while (!served && limit.timeIntervalSinceNow > 0) [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:limit];
    if (!served) { fprintf(stderr, "the main queue was not served in 20 s\n"); abort(); }
}
static void press(unsigned key) { runtime_input_key_synthetic(key, 1); runtime_input_key_synthetic(key, 0); tick(); }
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
/* The window's content: that height (or the most the screen's visible area holds), 16:9, each side within a point of the
   exact size: whole points cannot always be 16:9 (a 1512-point screen holds 1512x850.5; 1511x850 fits, 851 high would need 1513). */
static void window_is(CGFloat h) {
    NSRect room = [window contentRectForFrameRect:(window.screen ?: NSScreen.mainScreen).visibleFrame];
    CGFloat want = fmin(h, fmin(room.size.height, room.size.width * 9 / 16));
    NSSize now = window.contentView.bounds.size;
    if (fabs(now.height - want) > 1 || fabs(now.width - want * 16 / 9) > 1) {
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

/* The 更新 page (update_tab.h), on its own so it also runs where the window sizes in main do not fit the screen. */
@interface UpdateProbeClicks:NSObject
@property int hits;
@end
@implementation UpdateProbeClicks
-(void)hit:(id)sender{(void)sender;self.hits++;}
@end
static NSString *updateDir;
static double padAt = 1000;
enum { PAD_A = 1u << 0, PAD_L1 = 1u << 6, PAD_R1 = 1u << 7, PAD_UP = 1u << 13, PAD_DOWN = 1u << 15 };
static void pad_press(uint64_t buttons) {
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1} time:padAt += 1 active:YES];
    [settingsPanel controllerPad:(RuntimeGamepadState){.connected = 1, .buttons = buttons} time:padAt += 0.1 active:YES];
}
static TOTKUpdatePage *update_page(void) { return (TOTKUpdatePage *)settingsPanel.pages[SETTINGS_PAGE_UPDATE]; }
static NSString *update_text(void) { return update_page().text.stringValue; }
static void update_bytes(NSData *bytes) {
    NSString *path = [updateDir stringByAppendingPathComponent:@"status.json"];
    if (bytes) assert([bytes writeToFile:path atomically:YES]); else [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    [settingsPanel refreshUpdatePage]; [settingsPanel updateTargets];
}
/* A check as app/auto_update.h writes it, with these fields changed. */
static NSDictionary *update_doc(NSDictionary *fields) {
    NSMutableDictionary *d = [@{@"version": @1, @"at": @1793000000.5, @"state": @"behind", @"from": @"ab6ebe8", @"to": @"f0cfa3f",
        @"installed": @"ab6ebe8", @"commits": @[@"fix: 一个修复", @"feat: 一个功能"], @"total": @2} mutableCopy];
    [d addEntriesFromDictionary:fields]; return d;
}
static void update_status(NSDictionary *fields) { update_bytes([NSJSONSerialization dataWithJSONObject:update_doc(fields) options:0 error:nil]); }
static void says(NSString *part) {
    if ([update_text() containsString:part]) return;
    fprintf(stderr, "the page lacks %s:\n%s\n", part.UTF8String, update_text().UTF8String); abort();
}
static void lacks(NSString *part) {
    if (![update_text() containsString:part]) return;
    fprintf(stderr, "the page should not say %s:\n%s\n", part.UTF8String, update_text().UTF8String); abort();
}
static void status_is(NSString *text) {
    if ([update_page().status.stringValue isEqualToString:text]) return;
    fprintf(stderr, "status %s, expected %s\n", update_page().status.stringValue.UTF8String, text.UTF8String); abort();
}
static void update_probe(void) {
    unsetenv("TOTK_SLOT_OWNER"); setenv("TOTK_SOURCE_COMMIT", "ab6ebe8", 1);
    const char *dir = getenv("SETTINGS_TEST_DIR"); assert(dir);
    updateDir = [@(dir) stringByAppendingPathComponent:@"update"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:updateDir withIntermediateDirectories:YES attributes:nil error:nil]);
    [NSFileManager.defaultManager removeItemAtPath:[updateDir stringByAppendingPathComponent:@"status.json"] error:nil];
    setenv("TOTK_CONTROLS", [@(dir) stringByAppendingPathComponent:@"controls.txt"].UTF8String, 1);
    settings_test_output_init();
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.contentView = [[TOTKInputView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    settings_show(1); assert(settingsOpen);
    assert(settingsPanel.tabs.segmentCount == SETTINGS_PAGES && SETTINGS_PAGE_UPDATE == SETTINGS_PAGES - 1 && [[settingsPanel.tabs labelForSegment:SETTINGS_PAGE_UPDATE] isEqualToString:@"Update"]);
    for (TOTKSettingRow *r in settingsPanel.allRows) assert(r.page != SETTINGS_PAGE_UPDATE);
    assert(!spawned[0]);   /* nothing is asked before the page opens */

    /* The display page: the controller past the last row (it used to raise here), the contribute button clicks. Its real
       action could send a player's list, so the button is pointed at the probe. */
    assert(settingsPanel.page == SETTINGS_PAGE_DISPLAY);
    UpdateProbeClicks *clicks = [UpdateProbeClicks new]; NSButton *contribute = settingsPanel.displayButtons[1];
    contribute.target = clicks; contribute.action = @selector(hit:);
    pad_press(PAD_DOWN); assert(g_settings_controller_active);
    for (int n = 0; n < 20 && settingsPanel.controllerTargets[(NSUInteger)settingsPanel.controllerIndex] != contribute; n++) pad_press(PAD_DOWN);
    assert(settingsPanel.controllerTargets[(NSUInteger)settingsPanel.controllerIndex] == contribute && contribute.layer.borderWidth == 2);
    pad_press(PAD_A); assert(clicks.hits == 1);

    /* L1 / R1: the page is the last tab; opening it asks the launcher for a check. */
    pad_press(PAD_L1); assert(settingsPanel.page == SETTINGS_PAGE_UPDATE && settingsPanel.tabs.selectedSegment == SETTINGS_PAGE_UPDATE);
    assert(!strcmp(spawned, "--update-check ") && settingsPanel.rows.count == 0);
    for (NSButton *b in settingsPanel.displayButtons) assert(b.hidden);
    for (NSButton *b in settingsPanel.effectsButtons) assert(b.hidden);
    for (NSButton *b in settingsPanel.resetButtons) assert(b.hidden);
    TOTKUpdatePage *page = update_page(); NSButton *button = page.updateButton;
    assert(!page.text.selectable && page.text.refusesFirstResponder && button.refusesFirstResponder);
    status_is(@"Checking…"); assert([update_text() isEqualToString:@"Version ab6ebe8"] && button.hidden && settingsPanel.controllerTargets.count == 0);
    /* Again within ten minutes: no second check. */
    spawned[0] = 0; pad_press(PAD_R1); pad_press(PAD_L1); assert(settingsPanel.page == SETTINGS_PAGE_UPDATE && !spawned[0]);

    /* User 2026-10-31: the newest says just that; otherwise each newer commit's message and the Update button. */
    update_status(@{@"state": @"latest", @"commits": @[]}); status_is(@""); assert([update_text() isEqualToString:@"Version ab6ebe8 · the newest"] && button.hidden);
    update_status(@{});
    assert([update_text() isEqualToString:@"Version ab6ebe8\nThe newest, f0cfa3f, adds:\n•  fix: 一个修复\n•  feat: 一个功能"]);
    assert(!button.hidden && [button.title isEqualToString:@"Update (closes the game, opens it again when done)"]);
    assert(settingsPanel.controllerTargets.count == 1 && settingsPanel.controllerTargets[0] == button && NSMaxX(button.frame) <= page.frame.size.width);
    update_status(@{@"total": @45}); says(@"The newest, f0cfa3f, has 45 more commits (the newest 2 below):");
    /* What cannot be updated: why, and how to do it by hand; no button. */
    update_status(@{@"state": @"blocked", @"reason": @"local_changes", @"detail": @"M runtime/loader.c"});
    says(@"•  fix: 一个修复"); says(@"The checkout has uncommitted changes, so it cannot be updated for you."); says(@"M runtime/loader.c");
    says(@"Please update the code and build again yourself: git pull in your checkout, then python3.14 update_runtime.py"); assert(button.hidden);
    for (NSString *reason in @[@"no_repo", @"no_access", @"git_failed", @"fetch_failed", @"no_upstream", @"not_main", @"unpushed"]) {
        update_status(@{@"state": @"error", @"reason": reason, @"detail": @"", @"commits": @[]}); lacks(reason); says(@"Please update the code and build again yourself"); assert(button.hidden);
    }
    update_status(@{@"state": @"failed", @"reason": @"update_failed", @"detail": @"ERROR: the build stopped"});
    says(@"Updating the app did not work and it was left as it was"); says(@"ERROR: the build stopped"); says(@"Please update the code and build again yourself");
    /* After an update this start runs it: the newest. Another launch's update: from the next start. */
    update_status(@{@"state": @"updated", @"installed": @"f0cfa3f"}); status_is(@"Updated to f0cfa3f; from the next start"); lacks(@"adds:");
    setenv("TOTK_SOURCE_COMMIT", "f0cfa3f", 1); page.shown = NO; [settingsPanel refreshUpdatePage];
    status_is(@""); assert([update_text() isEqualToString:@"Version f0cfa3f · the newest"]);
    setenv("TOTK_SOURCE_COMMIT", "ab6ebe8", 1);
    update_status(@{@"state": @"updating"}); status_is(@"Updating…"); assert(button.hidden);
    update_status(@{@"state": @"closing"}); status_is(@"Closing the game to update…"); assert(button.hidden);
    for (NSString *word in @[@"Advice", @"worth updating", @"Copy", @"AI", @"Automatic"]) lacks(word);
    /* Anything that is not a whole version 1 document: none, never a crash. */
    NSData *whole = [NSJSONSerialization dataWithJSONObject:update_doc(@{}) options:0 error:nil];
    for (NSData *bytes in @[[@"{ not json" dataUsingEncoding:NSUTF8StringEncoding], [whole subdataWithRange:NSMakeRange(0, whole.length / 2)], NSData.data,
                            [@"[1]" dataUsingEncoding:NSUTF8StringEncoding], [NSData dataWithBytes:"\xff\xfe" length:2]]) {
        update_bytes(bytes); assert([update_text() isEqualToString:@"Version ab6ebe8"] && button.hidden);
    }
    for (NSDictionary *fields in @[@{@"version": @2}, @{@"version": @"1"}, @{@"at": NSNull.null}, @{@"state": @"other"}, @{@"commits": @"一"},
                                   @{@"commits": @[@1]}, @{@"total": @YES}, @{@"reason": @3}, @{@"to": @[]}]) {
        update_status(fields); assert([update_text() isEqualToString:@"Version ab6ebe8"] && button.hidden);
    }
    /* Up / down scroll a long list; the button stays the place. */
    NSMutableArray *many = [NSMutableArray array]; for (int i = 0; i < 30; i++) [many addObject:[NSString stringWithFormat:@"fix: 第 %d 个", i]];
    update_status(@{@"commits": many, @"total": @30}); NSClipView *clip = page.scroll.contentView; assert(clip.bounds.origin.y == 0);
    settingsPanel.controllerIndex = 0; g_settings_controller_active = YES; [settingsPanel controllerHighlight];
    pad_press(PAD_DOWN); assert(clip.bounds.origin.y == UPDATE_TAB_SCROLL && settingsPanel.controllerTargets[(NSUInteger)settingsPanel.controllerIndex] == button);
    [settingsPanel.timer fire]; assert(clip.bounds.origin.y == UPDATE_TAB_SCROLL);   /* the same document keeps its place */
    pad_press(PAD_UP); assert(clip.bounds.origin.y == 0);
    /* The Update button (A on it): the launcher's install with this game's pid; the page says the game will close. */
    update_status(@{}); spawned[0] = 0; [window makeFirstResponder:nil];
    settingsPanel.controllerIndex = 0; [settingsPanel controllerHighlight]; pad_press(PAD_A);
    char expect[64]; snprintf(expect, sizeof expect, "--update-install %d", (int)getpid());
    assert(!strcmp(spawned, expect) && window.firstResponder == window.contentView);
    status_is(@"Closing the game to update…"); spawned[0] = 0; pad_press(PAD_A); assert(!spawned[0]);   /* once */
    [settingsPanel.timer fire]; assert(button.hidden && settingsPanel.controllerTargets.count == 0);
    /* An agent's test session: no check, no button. */
    settings_show(0); page.pressed = NO; page.checkedAt = 0; spawned[0] = 0; setenv("TOTK_SLOT_OWNER", "claude", 1); settings_show(1);
    assert(settingsPanel.page == SETTINGS_PAGE_UPDATE && !spawned[0]); status_is(@"A test run: no check, no update"); assert(update_page().updateButton.hidden);
    unsetenv("TOTK_SLOT_OWNER");
    /* In Chinese. */
    settings_show(0); setenv("TOTK_UI_LANG", "zh", 1); spawn_ok = 0; settings_show(1);
    assert(settingsOpen && settingsPanel.page == SETTINGS_PAGE_UPDATE && [[settingsPanel.tabs labelForSegment:SETTINGS_PAGE_UPDATE] isEqualToString:@"更新"]);
    assert(NSContainsRect(window.contentView.bounds, settingsPanel.frame));
    update_page().pressed = NO; update_status(@{}); says(@"当前版本 ab6ebe8"); says(@"最新版 f0cfa3f 多了这些提交：");
    assert([update_page().updateButton.title isEqualToString:@"更新（会关闭游戏，更新完自动重新打开）"]);
    update_status(@{@"state": @"blocked", @"reason": @"unpushed"}); says(@"项目文件夹里有上游没有的提交，没法替你更新。"); says(@"请自己更新代码再重新编译");
    /* A newer document appears by itself while the page shows. */
    NSData *newer = [NSJSONSerialization dataWithJSONObject:update_doc(@{@"state": @"latest"}) options:0 error:nil];
    assert([newer writeToFile:[updateDir stringByAppendingPathComponent:@"status.json"] atomically:YES]);
    [settingsPanel.timer fire]; assert([update_text() isEqualToString:@"当前版本 ab6ebe8 · 已是最新版"]);
    settings_show(0); setenv("TOTK_UI_LANG", "en", 1); spawn_ok = 1;
}

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc==2 && !strcmp(argv[1],"frame")) { frame_probe(); return 0; }
    [NSApplication sharedApplication];
    controller_map_default(fakeMap, fakeModes); setenv("TOTK_UI_LANG", "en", 1); unsetenv("TOTK_SOURCE_COMMIT");
    fakeUsersCount = 2; fakeUsersActive = 0;   /* the store the User page reads (tests/test_users.c covers the files) */
    snprintf(fakeUsers[0].name, sizeof fakeUsers[0].name, "Local");
    snprintf(fakeUsers[1].name, sizeof fakeUsers[1].name, "Zelda");
    for (int i = 0; i < 2; i++) { arc4random_buf(&fakeUsers[i].lo, sizeof fakeUsers[i].lo); arc4random_buf(&fakeUsers[i].hi, sizeof fakeUsers[i].hi); }
    if (argc == 2 && !strcmp(argv[1], "update")) { update_probe(); puts("pass: update page"); return 0; }
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
    assert(settingsPanel.allRows.count == 27 + TOTK_V143);   /* 17 (anisotropy last on the display page, controller
                                                                 background input on the input page), the user page's
                                                                 two, then the effects page: FOV, two distances, shadows,
                                                                 depth of field, FXAA, sharpening and effect switches
                                                                 (1.4.3), or eight greyed rows (Breath of the Wild
                                                                 1.9.0's shadow resolution live) */

    /* Native 4K modes as game_patches.c serves them for 1.2.1 (the fixture swaps in a 4K swapchain); off again before
       the rows below read the mode. */
#ifndef GAME_BOTW
    settings_test_output_4k(1);
    assert(runtime_output_modes() & 1 << OUTPUT_MODE_NATIVE_4K);
    unsigned nw4 = 0, nh4 = 0;
    assert(runtime_output_native(OUTPUT_MODE_NATIVE_4K, &nw4, &nh4) && nw4 == 3840 && nh4 == 2160);
    assert(runtime_output_native(OUTPUT_MODE_NATIVE, &nw4, &nh4) && nw4 == 2560 && nh4 == 1440);   /* native stays 2K */
    assert(runtime_output_mode_set(OUTPUT_MODE_NATIVE_4K));
    assert(runtime_output_mode() == OUTPUT_MODE_NATIVE_4K);
    assert(runtime_output_mode_set(OUTPUT_MODE_OFF) && runtime_output_mode() == OUTPUT_MODE_OFF);
    settings_test_output_4k(0);
#endif

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
    if (hudBox) { TOTKHudBox *b = (TOTKHudBox *)hudBox; assert(!b.hidden && [b.label.stringValue isEqualToString:@"FPS 60  2560x1440\nPlaytime  0 h 00 m\nSettings: key 1 · controller L3 + R3"]); }   /* the last line: how to open this panel (a DualSense connected) */
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
    pick(@"Frame interpolation", @"60+60"); assert(mfx_requested() == MFX_60);
    assert(menu(@"Frame interpolation").numberOfItems == 6 && menu(@"Frame lock").numberOfItems == 7);
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
    /* Stronger ambient occlusion (PR #1316): off by default; every level acts at once (the Mac's shaders always carry the
       parameters) and is saved; a run without them (TOTK_SSAO=0) shows off, greyed out (rows are made at each opening). */
#if defined(TOTK_GAME_V143) || defined(GAME_BOTW190) || defined(TOTK_GAME_V121)
    shows(@"Stronger ambient occlusion", @"Off (as the game has it)"); assert(fakeSsaoStart == -1);
    pick(@"Stronger ambient occlusion", @"1.5×"); assert(settingsSsao == 150 && fakeSsaoSet == 150 && !strcmp(savedName, "ssao.txt") && !strcmp(savedWord, "150"));
    pick(@"Stronger ambient occlusion", @"2×"); assert(fakeSsaoSet == 200 && !strcmp(savedWord, "200"));
    pick(@"Stronger ambient occlusion", @"Off (as the game has it)"); assert(!settingsSsao && fakeSsaoSet == 0 && !strcmp(savedWord, "off"));
    press(18); fakeSsaoLive = 0; press(18); assert(settingsOpen && !menu(@"Stronger ambient occlusion").enabled);
    press(18); fakeSsaoLive = 1; press(18); assert(settingsOpen && menu(@"Stronger ambient occlusion").enabled);
#else
    shows(@"Stronger ambient occlusion", @"Not in this version"); assert(!menu(@"Stronger ambient occlusion").enabled);
#endif
    /* The effects page (2026-10-30): the two distances (1.4.3 only) and a switch per effect, each in force and saved at once. */
#ifdef TOTK_GAME_V143
    shows(@"Model detail distance (LOD)", @"1x (as the game has it)"); shows(@"Render distance", @"1x (as the game has it)");
    pick(@"Model detail distance (LOD)", @"4×"); assert(runtime_lod_scale() == 4 && !strcmp(savedName, "graphics-lod.txt") && !strcmp(savedWord, "400"));
    pick(@"Render distance", @"3×"); assert(runtime_far_scale() == 3 && !strcmp(savedName, "graphics-far.txt") && !strcmp(savedWord, "300"));
    pick(@"Model detail distance (LOD)", @"100×"); assert(runtime_lod_scale() == 100 && !strcmp(savedWord, "10000"));
    pick(@"Render distance", @"10×"); assert(runtime_far_scale() == 10 && !strcmp(savedWord, "1000"));
    pick(@"Model detail distance (LOD)", @"Always the finest"); assert(runtime_lod_scale() >= 1e6 && !strcmp(savedWord, "max"));
    shows(@"Shadow resolution", @"1x (as the game has it, 800)");
    pick(@"Shadow resolution", @"2×（1600）"); assert(runtime_shadow_scale() == 2 && !strcmp(savedName, "graphics-effects.txt"));
    pick(@"Shadow resolution", @"1x (as the game has it, 800)"); assert(runtime_shadow_scale() == 1);
    shows(@"Field of view (FOV)", @"1x (as the game has it)");
    pick(@"Field of view (FOV)", @"1.2×"); assert(runtime_fov_scale() == 1.2 && !strcmp(savedName, "graphics-fov.txt") && !strcmp(savedWord, "120"));
    pick(@"Field of view (FOV)", @"1x (as the game has it)");
    assert(switched(@"FXAA anti-aliasing")); flip(@"FXAA anti-aliasing", NO); assert(!fakeFxaa && !strcmp(savedName, "graphics-fxaa.txt") && !strcmp(savedWord, "off"));
    flip(@"FXAA anti-aliasing", YES); assert(fakeFxaa && !strcmp(savedWord, "on"));
    assert(switched(@"Depth of field")); flip(@"Depth of field", NO); assert(!runtime_dof_on() && !strcmp(savedName, "graphics-dof.txt") && !strcmp(savedWord, "off"));
    pick(@"Sharpening", @"50%"); assert(fakeSharpen == 50 && !strcmp(savedName, "graphics-sharpen.txt") && !strcmp(savedWord, "50"));
    pick(@"Sharpening", @"Off (as the game has it)"); assert(!fakeSharpen);
    /* "Reset to defaults" (the effects page's bottom right): every row back to the game's own, the files removed. */
    pick(@"Field of view (FOV)", @"1.3×"); pick(@"Model detail distance (LOD)", @"10×"); pick(@"Render distance", @"5×");
    flip(@"FXAA anti-aliasing", NO); pick(@"Sharpening", @"75%"); flip(@"Bloom", NO);
    [settingsPanel selectPage:SETTINGS_PAGE_EFFECTS]; tick();
    NSButton *resetEffects = settingsPanel.effectsButtons[0];
    assert(!resetEffects.hidden && [resetEffects.title isEqualToString:@"Reset to defaults"] && [settingsPanel.controllerTargets containsObject:resetEffects]);
    [resetEffects performClick:nil]; tick();
    assert(runtime_fov_scale() == 1 && runtime_lod_scale() == 1 && runtime_far_scale() == 1 && fakeFxaa && !fakeSharpen && fakeFxOn[0] && fakeFxOn[1] && !fakeAniso);
    assert(!strcmp(removedName, "graphics-dof.txt") && settingsPanel.page == SETTINGS_PAGE_EFFECTS && runtime_dof_on());   /* the last file the reset removes */
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY]; tick();
    pick(@"Model detail distance (LOD)", @"1x (as the game has it)"); pick(@"Render distance", @"1x (as the game has it)"); assert(runtime_lod_scale() == 1 && runtime_far_scale() == 1);
    assert(switched(@"Bloom")); flip(@"Bloom", NO); assert(!fakeFxOn[1] && !strcmp(savedName, "graphics-effects.txt") && !strcmp(savedWord, "bloom"));
    flip(@"Ambient occlusion", NO); assert(!strcmp(savedWord, "ao bloom"));
    flip(@"Bloom", YES); flip(@"Ambient occlusion", YES); assert(fakeFxOn[0] && fakeFxOn[1] && !strcmp(savedWord, "none"));
#else
    /* Shadow resolution (Breath of the Wild 1.9.0, the production calls of game_patches_botw.c): the next launch's choice,
       saved in shadows.txt (this run keeps the size it started with: tests/test_botw_shadow_res.c); greyed out while
       TOTK_SHADOW_RES decides (rows are made at each opening). */
#  ifdef GAME_BOTW190
    assert([row(@"Shadow resolution").title isEqualToString:@"Shadow resolution (from the next launch)"]);
    shows(@"Shadow resolution", @"As the game has it");
    pick(@"Shadow resolution", @"2048 per cascade"); assert(runtime_shadow_res_saved() == 2048 && !strcmp(savedName, "shadows.txt") && !strcmp(savedWord, "2048"));
    shows(@"Shadow resolution", @"2048 per cascade");
    pick(@"Shadow resolution", @"As the game has it"); assert(!runtime_shadow_res_saved() && !strcmp(savedWord, "off"));
    press(18); setenv("TOTK_SHADOW_RES", "2048", 1); press(18); assert(settingsOpen && !menu(@"Shadow resolution").enabled);
    shows(@"Shadow resolution", @"Set by TOTK_SHADOW_RES");
    press(18); unsetenv("TOTK_SHADOW_RES"); press(18); assert(settingsOpen && menu(@"Shadow resolution").enabled);
    /* Where the patch does not act (ios_code(): the iOS export's run here, the device's code there) nothing is offered. */
    press(18); setenv("TOTK_EXPORT_IOS", "/nonexistent", 1); press(18); assert(settingsOpen && !menu(@"Shadow resolution").enabled);
    shows(@"Shadow resolution", @"Not in this version");
    press(18); unsetenv("TOTK_EXPORT_IOS"); press(18); assert(settingsOpen && menu(@"Shadow resolution").enabled);
    /* "Reset to defaults" sets it back to the game's own and removes shadows.txt. */
    pick(@"Shadow resolution", @"2048 per cascade"); [settingsPanel selectPage:SETTINGS_PAGE_EFFECTS]; tick();
    removedAll[0] = 0; [settingsPanel.effectsButtons[0] performClick:nil]; tick();
    assert(!runtime_shadow_res_saved() && strstr(removedAll, "shadows.txt "));
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY]; tick();
    /* The field of view (Breath of the Wild 1.9.0, the production calls of game_patches_botw.c; the stub itself:
       tests/test_botw_fov.c): 1x, as the game has it; each choice acts at once and is saved in graphics-fov.txt (x100). */
    settings_test_fov_init(); assert(!settings_test_fov_word());
    shows(@"Field of view (FOV)", @"1x (as the game has it)");
    assert(menu(@"Field of view (FOV)").numberOfItems == 6 && [[menu(@"Field of view (FOV)") itemTitleAtIndex:5] isEqual:@"1.5×"]);   /* 1x up */
    pick(@"Field of view (FOV)", @"1.2×"); assert(runtime_fov_scale() == 1.2 && !strcmp(savedName, "graphics-fov.txt") && !strcmp(savedWord, "120"));
    assert(settings_test_fov_word() == float_bits(1.2f));   /* the word the stub reads, at once */
    runtime_fov_scale_set(0); runtime_fov_scale_set(-1); runtime_fov_scale_set(NAN); runtime_fov_scale_set(3);
    assert(runtime_fov_scale() == 1.2 && settings_test_fov_word() == float_bits(1.2f));   /* outside 0.5..2: never taken */
    press(18); setenv("TOTK_FOV_SCALE", "1.3", 1); press(18); assert(settingsOpen && !menu(@"Field of view (FOV)").enabled);
    shows(@"Field of view (FOV)", @"1.2×");
    press(18); unsetenv("TOTK_FOV_SCALE"); press(18); assert(settingsOpen && menu(@"Field of view (FOV)").enabled);
    press(18); setenv("TOTK_EXPORT_IOS", "/nonexistent", 1); press(18); assert(settingsOpen && !menu(@"Field of view (FOV)").enabled);
    shows(@"Field of view (FOV)", @"Not in this version");
    press(18); unsetenv("TOTK_EXPORT_IOS"); press(18); assert(settingsOpen && menu(@"Field of view (FOV)").enabled);
    press(18); settings_test_fov_refused(1); press(18); assert(settingsOpen && !menu(@"Field of view (FOV)").enabled);
    shows(@"Field of view (FOV)", @"Not in this version");   /* the camera code was not as shipped: nothing patched */
    press(18); settings_test_fov_refused(0); press(18); assert(settingsOpen && menu(@"Field of view (FOV)").enabled);
    /* "Reset to defaults" sets it back to 1x and removes graphics-fov.txt. */
    pick(@"Field of view (FOV)", @"1.3×"); [settingsPanel selectPage:SETTINGS_PAGE_EFFECTS]; tick();
    removedAll[0] = 0; [settingsPanel.effectsButtons[0] performClick:nil]; tick();
    assert(runtime_fov_scale() == 1 && settings_test_fov_word() == float_bits(1.0f) && strstr(removedAll, "graphics-fov.txt "));
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY]; tick();
#  else
    shows(@"Shadow resolution", @"Not in this version"); assert(!menu(@"Shadow resolution").enabled);
    shows(@"Field of view (FOV)", @"Not in this version"); assert(!menu(@"Field of view (FOV)").enabled);
#  endif
    shows(@"Render distance", @"Not in this version"); assert(!menu(@"Render distance").enabled);
    shows(@"Effect switches", @"Not in this version"); shows(@"Sharpening", @"Not in this version");
#endif
    /* Anisotropic filtering (effects page, every version): in force at once. */
    shows(@"Anisotropic filtering", @"Off (as the game has it)");
    pick(@"Anisotropic filtering", @"16×"); assert(fakeAniso == 16 && !strcmp(savedName, "graphics-aniso.txt") && !strcmp(savedWord, "16"));
    shows(@"Anisotropic filtering", @"16×");
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

    assert(!switched(@"Controller input in background"));
    flip(@"Controller input in background", YES); assert(fakePadBackground && switched(@"Controller input in background"));
    flip(@"Controller input in background", NO); assert(!fakePadBackground && !switched(@"Controller input in background"));

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

    /* Users: the page shows the user this run opened with, and a pop-up for the next launch's user; the pick marks
       the user and makes the row again; the footer button names a new user in the game's text bar — the panel closes
       while the bar is up and comes back with the users read again. */
    [settingsPanel selectPage:SETTINGS_PAGE_USERS];
    assert(settingsPanel.rows.count == 2 && !row(@"User this run").control.enabled &&
           [menu(@"User this run").titleOfSelectedItem isEqualToString:@"Local"]);
    shows(@"User (from the next launch)", @"Local");
    assert(settingsPanel.controllerTargets.count == 3);
    {   /* The panel is as tall as its tallest page below the key bindings, the user page's two rows included. */
        NSUInteger perPage[SETTINGS_PAGES] = {0};
        for (TOTKSettingRow *r in settingsPanel.allRows) perPage[r.page]++;
        NSUInteger tallest = 0;
        for (NSInteger page = 0; page < SETTINGS_PAGE_BINDINGS; page++) if (perPage[page] > tallest) tallest = perPage[page];
        assert(settingsPanel.frame.size.height >= SETTINGS_TOP + tallest * SETTINGS_ROW_H + SETTINGS_BOTTOM - 1);
        assert(perPage[SETTINGS_PAGE_USERS] == 2);
    }
    NSButton *addUser = settingsPanel.userButtons[0];
    assert([addUser.title isEqualToString:@"Add User…"] && !addUser.hidden);
    pick(@"User (from the next launch)", @"Zelda");
    /* the row is made again with Zelda marked (the remake is dispatched); a run loop pass can end before it is
       served, so wait for it instead of counting passes */
    for (int i = 0; i < 100 && [menu(@"User (from the next launch)").titleOfSelectedItem isEqualToString:@"Local"]; i++) tick();
    addUser = settingsPanel.userButtons[0];
    assert(fakeUsersActive == 1 && settingsPanel.rows.count == 2 &&
           [menu(@"User (from the next launch)").titleOfSelectedItem isEqualToString:@"Zelda"]);
    assert([menu(@"User this run").titleOfSelectedItem isEqualToString:@"Local"]);   /* this run keeps its user */
    [addUser performClick:nil];
    for (int i = 0; i < 100 && settingsOpen; i++) tick();   /* the text bar opens; the panel closed */
    assert(!settingsOpen && settingsPanel.hidden);
    runtime_front_text_test("type", "Link"); tick();
    runtime_front_text_test("enter", "");
    for (int i = 0; i < 100 && !settingsOpen; i++) tick();   /* the panel comes back, the user kept */
    assert(settingsOpen && fakeUsersCount == 3 && !strcmp(fakeUsers[2].name, "Link"));
    assert(menu(@"User (from the next launch)").numberOfItems == 3 &&
           [menu(@"User (from the next launch)").titleOfSelectedItem isEqualToString:@"Zelda"]);
    addUser = settingsPanel.userButtons[0];
    [addUser performClick:nil];
    for (int i = 0; i < 100 && settingsOpen; i++) tick();
    runtime_front_text_test("esc", "");   /* Escape answers the bar: nothing is added */
    for (int i = 0; i < 100 && !settingsOpen; i++) tick();
    assert(settingsOpen && fakeUsersCount == 3);
    addUser = settingsPanel.userButtons[0];
    while (fakeUsersCount < RUNTIME_USERS_MAX) assert(runtime_users_add("Filler") >= 0);
    [addUser performClick:nil];
    for (int i = 0; i < 100 && settingsOpen; i++) tick();   /* the console is full: the bar still asks, the user is not kept */
    runtime_front_text_test("type", "Extra"); tick();
    runtime_front_text_test("enter", "");
    for (int i = 0; i < 100 && !settingsOpen; i++) tick();
    assert(settingsOpen && fakeUsersCount == RUNTIME_USERS_MAX);
    assert([settingsPanel.versionLabel.stringValue containsString:@"At most 8 users"]);
    while (fakeUsersCount > 2) fakeUsersCount--;   /* the two the next checks name */
    pick(@"User (from the next launch)", @"Local");
    for (int i = 0; i < 100 && ![menu(@"User (from the next launch)").titleOfSelectedItem isEqualToString:@"Local"]; i++) tick();
    assert(fakeUsersActive == 0 && [menu(@"User (from the next launch)").titleOfSelectedItem isEqualToString:@"Local"]);

    /* Key bindings and the Update page follow the user page in the tabs, so the checks below start on the input page again. */
    [settingsPanel selectPage:SETTINGS_PAGE_INPUT]; tick();

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

    /* Pages: Display, Input, User and Key bindings; the page shown last opens again. */
    press(18); assert(settingsOpen);
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY];
    assert(settingsPanel.rows.count == 6 && [settingsPanel.rows[0].title isEqualToString:@"Overlay"]);   /* edge smoothing, AO and anisotropy are on the effects page */
    /* The display page's bottom right: contribute shaders and star the project, after the rows for a controller; the
       version stays clear of them; a result shows for a moment where the version is. */
    NSButton *contribute = settingsPanel.displayButtons[1], *starButton = settingsPanel.displayButtons[0];
    assert([contribute.title isEqualToString:@"Contribute shaders and pipelines"] && [starButton.title isEqualToString:@"⭐ Star the project"]);
    assert(!contribute.hidden && !starButton.hidden && NSMaxX(contribute.frame) < NSMinX(starButton.frame) && NSMaxX(starButton.frame) <= settingsPanel.frame.size.width - 14);
    assert(NSMaxX(settingsPanel.versionLabel.frame) <= NSMinX(contribute.frame) && contribute.frame.origin.y < 34 && starButton.frame.origin.y < 34);
    assert(settingsPanel.controllerTargets.count == 8 && settingsPanel.controllerTargets[6] == contribute && settingsPanel.controllerTargets[7] == starButton);
    assert(settingsPanel.effectsButtons[0].hidden);   /* "Reset to defaults" only on the effects page */
    NSString *versionText = settingsPanel.versionLabel.stringValue;
    [settingsPanel note:@"Thank you!"]; assert([settingsPanel.versionLabel.stringValue isEqualToString:@"Thank you!"]);
    settingsPanel.versionLabel.stringValue = versionText; settingsPanel.versionLabel.frame = NSMakeRect(18, 9, 230, 16);
    [settingsPanel selectPage:SETTINGS_PAGE_INPUT];
    assert(contribute.hidden && starButton.hidden);   /* the first page only */
    assert(settingsPanel.rows.count == 8 && !settingsPanel.pages[SETTINGS_PAGE_INPUT].hidden && settingsPanel.pages[SETTINGS_PAGE_DISPLAY].hidden);
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
    [settingsPanel selectPage:SETTINGS_PAGE_EFFECTS]; tick();   /* the longest page sets the panel's height */
    NSRect lastBand = [settingsPanel.rows.lastObject.control.superview convertRect:settingsPanel.rows.lastObject.control.superview.bounds toView:settingsPanel];
    NSRect version = settingsPanel.versionLabel.frame;
    assert(NSMinX(version) <= 20 && NSMinY(version) >= 6 && NSMaxY(version) <= NSMinY(lastBand) - 6 && NSMinY(lastBand) - NSMaxY(version) <= 14);
    for (NSView *view in settingsPanel.subviews)
        if ([view isKindOfClass:NSButton.class] && NSMaxY(view.frame) <= NSMinY(lastBand))   /* below the rows only the effects page's reset */
            assert(view.hidden || [settingsPanel.effectsButtons containsObject:(NSButton *)view]);
    for (NSView *view in settingsPanel.subviews)
        if ([view isKindOfClass:NSTextField.class] && view != settingsPanel.versionLabel) assert(NSMinY(view.frame) > NSMinY(lastBand));
    [settingsPanel selectPage:SETTINGS_PAGE_DISPLAY]; tick();

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
    assert(settingsPanel.page == SETTINGS_PAGE_UPDATE);
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
    /* Only a press on the panel itself drags it: a drag that started on the drawing still reaches the panel, and it used to
       jump there from the last press's spot. */
    NSEvent *(^mouse)(NSEventType, NSPoint) = ^NSEvent *(NSEventType type, NSPoint at) {
        return [NSEvent mouseEventWithType:type location:at modifierFlags:0 timestamp:0 windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:1 pressure:1]; };
    NSPoint spot = settingsPanel.frame.origin, savedAt = g_settings_at, background = [settingsPanel convertPoint:NSMakePoint(6, settingsPanel.bounds.size.height - 6) toView:nil];
    [settingsPanel mouseDown:mouse(NSEventTypeLeftMouseDown, background)];
    [settingsPanel mouseDragged:mouse(NSEventTypeLeftMouseDragged, NSMakePoint(background.x + 3, background.y + 2))];
    [settingsPanel mouseUp:mouse(NSEventTypeLeftMouseUp, NSMakePoint(background.x + 3, background.y + 2))];
    assert(NSEqualPoints(settingsPanel.frame.origin, NSMakePoint(spot.x + 3, spot.y + 2)));
    spot = settingsPanel.frame.origin;
    NSPoint onDrawing = [kb convertPoint:NSMakePoint(NSMidX(oneKey), NSMidY(oneKey)) toView:nil];   /* no menu there */
    [kb mouseDown:mouse(NSEventTypeLeftMouseDown, onDrawing)];
    [kb mouseDragged:mouse(NSEventTypeLeftMouseDragged, NSMakePoint(onDrawing.x + 3, onDrawing.y + 2))];
    [kb mouseUp:mouse(NSEventTypeLeftMouseUp, NSMakePoint(onDrawing.x + 3, onDrawing.y + 2))];
    assert(NSEqualPoints(settingsPanel.frame.origin, spot));
    g_settings_at = savedAt; [settingsPanel center];
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
    assert([row(@"本次运行的用户").control isKindOfClass:[NSPopUpButton class]]);
    shows(@"用户（下次启动生效）", @"Local");
    assert([settingsPanel.userButtons[0].title isEqualToString:@"添加用户…"]);
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
    puts("pass: settings panel (key 1 / Esc, overlay, frame lock, interpolation, resolution 1080p / MetalFX / 2K / 4K and window size, aspect 16:9 / 16:10 / 21:9 / 32:9, aspect ratio, switches, controller type, rumble / rumble strength / gyro aiming, users: this run, next launch, added through the text bar, full console, game keys blocked while open, keys 2-7 inert, Chinese)");
    return 0;
}}
