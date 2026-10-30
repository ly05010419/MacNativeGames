#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#import <QuartzCore/CAMetalLayer.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include "runtime.h"
#include "game_version.h"   // GAME_FOLDER and TOTK_GAME_VERSION of this build (build_ipa.py's -D flags)
#include "dmsh_chain.h"
#include "settings_map.h"
#import "touch_pad.h"
#import "start_screen.h"

int totk_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
void runtime_ios_sync_drawable(UIView *host);
void runtime_ios_sync_safe_area(UIView *host);
static char *host_argv[3];
static NSString *g_no_game_data;   // set when there is no RomFS image to play from (didFinishLaunching)
static NSString *g_seed_pack, *g_seed_unpack_to;   // a pushed shader pack to unpack before the game starts (seed_unpack)

/* The startup shaders arrive as one stream cut into 1 GiB parts (<folder>.pack.000, .001, ...; ios/tools/install_device.py
   push_seed): devicectl stops after ~25,000 of the seed's 128,000 files when it copies a folder (2026-10-23). Stream:
   "TOTKPACK1", then per file a little-endian u16 name length, the name, a u64 size and the bytes; a zero name length
   ends it. Unpacked into <folder>.tmp, renamed to <folder> when complete, the parts removed. Returns nil or the reason. */
static NSString *seed_unpack(NSString *base, NSString *dest, void (^progress)(uint64_t done, uint64_t total)) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray<NSString *> *parts = [NSMutableArray array]; uint64_t total = 0;
    for (int i = 0;; i++) {
        NSString *p = [base stringByAppendingFormat:@".%03d", i];
        NSDictionary *a = [fm attributesOfItemAtPath:p error:nil]; if (!a) break;
        [parts addObject:p]; total += a.fileSize;
    }
    if (!parts.count) return @"no pack";
    __block NSUInteger at = 0; __block FILE *f = fopen(parts[0].fileSystemRepresentation, "rb"); __block uint64_t done = 0;
    BOOL (^take)(void *, size_t) = ^BOOL(void *out, size_t n) {   // n bytes of the stream, across part boundaries
        uint8_t *o = out;
        while (n) {
            if (!f) return NO;
            size_t got = fread(o, 1, n, f); o += got; n -= got; done += got;
            if (n) { fclose(f); f = ++at < parts.count ? fopen(parts[at].fileSystemRepresentation, "rb") : NULL; }
        }
        return YES;
    };
    NSString *tmp = [dest stringByAppendingString:@".tmp"]; [fm removeItemAtPath:tmp error:nil];
    if (![fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil]) { if (f) fclose(f); return @"cannot make the folder"; }
    char magic[9]; NSString *why = nil; size_t cap = 1 << 20; uint8_t *buf = malloc(cap); uint64_t files = 0, last = 0;
    if (!buf) { if (f) fclose(f); [fm removeItemAtPath:tmp error:nil]; return @"not enough memory to unpack"; }
    if (!take(magic, 9) || memcmp(magic, "TOTKPACK1", 9)) why = @"not a shader pack";
    while (!why) {
        uint16_t nl; if (!take(&nl, 2)) { why = @"truncated"; break; }
        if (!nl) break;
        char name[256]; uint64_t size;
        if (nl >= sizeof name || !take(name, nl)) { why = @"bad name"; break; }
        name[nl] = 0;
        if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || !take(&size, 8)) { why = @"bad entry"; break; }
        FILE *o = fopen([tmp stringByAppendingPathComponent:@(name)].fileSystemRepresentation, "wb"); if (!o) { why = @"cannot write"; break; }
        while (size && !why) { size_t n = size < cap ? (size_t)size : cap; if (!take(buf, n) || fwrite(buf, 1, n, o) != n) why = @"truncated or disk full"; size -= n; }
        // Buffered writes can report success until fclose flushes the final bytes (for example, ENOSPC).
        if (fclose(o) != 0 && !why) why = @"cannot finish writing the shader file";
        files++;
        if (progress && done - last > (32u << 20)) { last = done; progress(done, total); }
    }
    free(buf); if (f) fclose(f);
    if (why) { [fm removeItemAtPath:tmp error:nil]; return why; }
    [fm removeItemAtPath:dest error:nil];
    if (![fm moveItemAtPath:tmp toPath:dest error:nil]) return @"cannot rename";
    for (NSString *p in parts) [fm removeItemAtPath:p error:nil];
    fprintf(stderr, "[totk] shader pack unpacked: %llu files, %.0f MB\n", (unsigned long long)files, total / 1048576.0);
    return nil;
}
static void *run_host(void *unused) { (void)unused; return (void *)(intptr_t)totk_host_main(3, host_argv); }
/* Key 1's settings panel is Mac only (runtime/frontend.m). */
void runtime_settings_toggle(void) {}
#if !TARGET_OS_VISION
static void apply_display_settings(void);   // below, next to the pane helpers it uses
#endif

@interface TOTKMetalView : UIView @end
@implementation TOTKMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
#if TARGET_OS_VISION
- (void)layoutSubviews { [super layoutSubviews]; runtime_ios_sync_drawable(self); }
#endif
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; runtime_ios_sync_safe_area(self); }   // the HUD boxes keep out of the notch
@end

@interface TOTKGameController : UIViewController @property(nonatomic,strong) TOTKMetalView *gameView; @end
@implementation TOTKGameController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.gameView = [[TOTKMetalView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
#if TARGET_OS_VISION
    // Keep gamepad actions in GameController instead of visionOS converting them to pinch events.
    GCEventInteraction *controllerInput = [GCEventInteraction new];
    controllerInput.handledEventTypes = GCUIEventTypeGamepad;
    [self.gameView addInteraction:controllerInput];
#endif
    [self.view addSubview:self.gameView];
    runtime_ios_attach_layer((CAMetalLayer *)self.gameView.layer, self.gameView);
    TOTKTouchPad *pad=[[TOTKTouchPad alloc] initWithFrame:self.view.bounds];pad.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:pad];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started; if (started) return; started = YES;
    /* What happens before the game starts is said on the boot screen (runtime/frontend_ios.m), not on a label of its own:
       one laid over it showed both texts on top of each other (2026-10-23). */
    BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];   // the same rule as the boot screen (totk_ui_lang.h)
    if (g_no_game_data) {
        runtime_boot_status(zh ? "无法开始游戏" : "Cannot start the game", g_no_game_data.UTF8String);
        return;
    }
#if TARGET_OS_VISION
    [self boot];
#else
    __weak TOTKGameController *weak = self;
    void (^start)(void) = ^{
        apply_display_settings();
        runtime_ios_sync_drawable(weak.gameView);   // the drawable follows the chosen picture (TOTK_IOS_DRAWABLE_H)
        [weak boot];
    };
    /* A run started by a script skips the start screen, which would wait for a tap: devicectl ... --environment-variables
       '{"TOTK_START_SCREEN":"0"}'. The Settings app's choices apply as they are (after PR #1174). */
    const char *screen = getenv("TOTK_START_SCREEN");
    if (screen && !strcmp(screen, "0")) start();
    else [self.view addSubview:[[TOTKStartView alloc] initWithFrame:self.view.bounds onStart:start]];
#endif
}
- (void)boot {
    BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];
    if (g_seed_pack) {   // shaders installed as a pack: unpack once, on screen, then start (not on the main thread: a long launch is killed)
        static const char *stage_zh = "正在准备着色器（更新后只做一次）", *stage_en = "Preparing the shaders (once after an update)";
        const char *stage = zh ? stage_zh : stage_en;
        runtime_boot_progress(stage, 0, 0);
        NSString *pack = g_seed_pack, *to = g_seed_unpack_to; g_seed_pack = nil;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
            NSString *why = seed_unpack(pack, to, ^(uint64_t done, uint64_t total) { runtime_boot_progress(stage, done, total); });
            fprintf(stderr, "[totk] shader pack: %s in %.1f s\n", why ? why.UTF8String : "ready", CFAbsoluteTimeGetCurrent() - t0);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (why) { runtime_boot_status(zh ? "着色器准备失败" : "Could not prepare the shaders", (zh
                               ? [NSString stringWithFormat:@"%@。请在 Mac 上用 ios/tools/install_device.py 重新安装。", why]
                               : [NSString stringWithFormat:@"%@. Install again from the Mac with ios/tools/install_device.py.", why]).UTF8String); return; }
                [self startGame];
            });
        });
        return;
    }
    [self startGame];
}
- (void)startGame {
    /* Playback category (PR #97): plays with the ring / silent switch off and routes to the speaker. Without an active
       session the game's CoreAudio output stays on a null sink on iOS 27 ("no device sink in use"); activating it in
       didFinishLaunching blocked the main thread before the log file was opened. Interruptions: audio_observe. */
    AVAudioSession *audio = AVAudioSession.sharedInstance;
    NSError *catErr = nil, *actErr = nil;
    [audio setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&catErr];
    BOOL activated = [audio setActive:YES error:&actErr];
    fprintf(stderr, "[totk-audio] audio session category=%s active=%d route=%s err=%s%s\n",
            audio.category.UTF8String ?: "?", (int)activated, audio.currentRoute.outputs.firstObject.portType.UTF8String ?: "?",
            catErr ? catErr.localizedDescription.UTF8String : "none", actErr ? actErr.localizedDescription.UTF8String : "");
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread; pthread_create(&thread, &attr, run_host, NULL);
    pthread_detach(thread); pthread_attr_destroy(&attr);
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

/* iOS 27 ends apps linked against its SDK that keep the window in the app delegate (UIKit traps in
   _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption at launch), so the window belongs to a window scene. */
@interface TOTKSceneDelegate : UIResponder <UIWindowSceneDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation TOTKSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [TOTKGameController new]; [self.window makeKeyAndVisible];
#if TARGET_OS_VISION
    UIWindowSceneGeometryPreferencesVision *prefs = [[UIWindowSceneGeometryPreferencesVision alloc] initWithSize:CGSizeMake(1280, 720)];
    prefs.resizingRestrictions = UIWindowSceneResizingRestrictionsUniform;
    [(UIWindowScene *)scene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError *error) {
        fprintf(stderr, "[totk] 16:9 window refused: %s\n", error.localizedDescription.UTF8String);
    }];
#endif
}
@end

/* Game audio after interruptions (from nxnative b301431). A call, Siri or an alarm takes the audio session and the
   system stops the game's output unit; a media services reset invalidates it. Nothing started it again, so the game
   stayed silent. When an interruption ends the session is made active again and the output started
   (runtime_audio_output_restart); a reset rebuilds the output. Becoming active does the same, for an interruption
   whose "ended" never came. Becoming active also turns the idle timer off again. */
static BOOL g_audio_interrupted;
static void audio_resume(int recreate, const char *why) {
    NSError *err = nil;
    if (![AVAudioSession.sharedInstance setActive:YES error:&err]) fprintf(stderr,"[totk-audio] audio session not active again (%s): %s\n",why,err.localizedDescription.UTF8String);
    g_audio_interrupted = NO; runtime_audio_output_restart(recreate);
}
static void audio_observe(void) {
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter; NSOperationQueue *main = NSOperationQueue.mainQueue;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        fprintf(stderr,"[totk-audio] audio session interruption %s\n",type == AVAudioSessionInterruptionTypeBegan ? "began" : "ended");
        if (type == AVAudioSessionInterruptionTypeBegan) g_audio_interrupted = YES; else audio_resume(0,"interruption ended");
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; fprintf(stderr,"[totk-audio] media services were reset\n"); audio_resume(1,"media services reset");
    }];
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; UIApplication.sharedApplication.idleTimerDisabled = YES;
        if (g_audio_interrupted) audio_resume(0,"became active during an interruption");
    }];
}

#if !TARGET_OS_VISION
/* apply_settings runs before the window scene exists, so there is no scene screen to ask yet. */
static NSDictionary *pref_row(NSDictionary *root, NSString *key) {
    for (NSDictionary *item in root[@"PreferenceSpecifiers"] ?: @[])
        if ([item[@"Key"] isEqual:key]) return item;
    return nil;
}
#endif
/* Both layers hand their operation mode and frame pacing over through these two, so the overwrite policy exists once.
   Breath of the Wild has the same switch since its docked branch renders the scene at 1920x1080 (ad35dfa). */
static void apply_operation_mode(const char *mode) {
    setenv("TOTK_OPERATION_MODE", mode, 1);
}
/* overwrite: the custom layer owns its keys (1); the presets keep a devicectl value in place (0) and never clear. */
static void apply_frame_env(const totk_frame_env *e, int overwrite) {
    if (e->present_hz) setenv("DM_PRESENT_HZ", e->present_hz, overwrite);
    else if (overwrite) unsetenv("DM_PRESENT_HZ");
    if (e->phone_fps) setenv("TOTK_PHONE_FPS", e->phone_fps, overwrite);
    else if (overwrite) unsetenv("TOTK_PHONE_FPS");
}

#if !TARGET_OS_VISION
/* The presets' picture and frame rate, from the rows the Settings app and the start screen (start_screen.m) write;
   applied once, just before the game thread starts, so the start screen's pick counts (after PR #433: applying them in
   didFinishLaunching as well left a 1080 drawable behind when the start screen then turned Full HD off). The picture keys
   keep a devicectl value (overwrite=0), as before the pane had layers. The custom layer was handed over in apply_settings
   and the start screen does not write its rows, so it is left as it is. The mapping is settings_map.c's. */
static void apply_display_settings(void) {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    if ([d boolForKey:@"TOTK_CUSTOM_PICTURE"]) return;
    totk_preset_env prenv;
    totk_preset_env_build([d boolForKey:@"TOTK_DOCKED"], &prenv);
    if (prenv.drawable) setenv("TOTK_IOS_DRAWABLE_H", prenv.drawable, 0);
    if (prenv.scene) setenv("TOTK_RENDER_RES", prenv.scene, 0);
    setenv("TOTK_MFX_OUTPUT", prenv.output_upscale, 0);
    apply_operation_mode(prenv.operation);
    // Frame rate: Settings.bundle › TOTK_PHONE_FPS, or the 60 / 30 button of the touch controls, which saves it here too.
    NSString *fps = [d stringForKey:@"TOTK_PHONE_FPS"] ?: @"60";
    totk_frame_env fe;
    if (totk_preset_frame_env_build(fps.UTF8String, &fe)) apply_frame_env(&fe, 1);
}
#endif

/* The Settings app pane is the single source of defaults: every TOTK_* row with a DefaultValue is registered, so the
   game sees the same defaults before the player changes anything, then handed to the runtime as an environment
   variable (overwrite=0, so devicectl can still supply a plain key). The picture and frame keys belong to one of the
   pane's two layers, chosen by TOTK_CUSTOM_PICTURE: the presets (Full HD, frame rate) when it is off, the custom
   picture and frame rows when it is on. Only the chosen layer is handed over, so no key is written twice. visionOS is
   the exception: its platform defaults own the picture and frame keys (the pane's rows describe a phone display). */
static void apply_settings(void) {
    NSBundle *settings = [NSBundle bundleWithPath:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"Settings.bundle"]];
    NSString *path = [settings pathForResource:@"Root" ofType:@"plist"];
    NSDictionary *root = path ? [NSDictionary dictionaryWithContentsOfFile:path] : nil;
    NSMutableDictionary *defaults = [NSMutableDictionary dictionary];
    for (NSDictionary *item in root[@"PreferenceSpecifiers"] ?: @[]) {
        NSString *key = item[@"Key"]; id value = item[@"DefaultValue"];
        if (!key.length || ![key hasPrefix:@"TOTK_"] || !value) continue;
#if TARGET_OS_VISION
        /* Rows marked TOTKPhone describe a phone or tablet display. On visionOS the platform's own defaults
           (didFinishLaunching, below) own those keys: the window is not a 60 Hz panel, and its scene size comes from
           the headset rather than from a phone's list. Handing the pane's values over would silently replace them.
           The mark sits on the row itself (Root.plist), so a new display row needs no second list here. */
        if ([item[@"TOTKPhone"] boolValue]) continue;
#endif
        defaults[key] = value;
    }
    /* A package without the pane is broken; do not keep a second, hand-copied copy of the defaults here. Each
       consumer falls back to its own documented default (the picture and frame maps have theirs). */
    if (!defaults.count) fprintf(stderr, "[totk] settings: no Settings.bundle defaults; using the runtime defaults\n");
    fprintf(stderr, "[totk] settings: %lu defaults from %s\n", (unsigned long)defaults.count, path.UTF8String ?: "no Settings.bundle");
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d registerDefaults:defaults];
    /* One switch between the pane's layers (Root.plist, "Use custom settings"): the layer that is off is not handed
       over at all, so neither can overwrite the other. Which keys belong to which layer is settings_map.c's table. */
    BOOL custom = [d boolForKey:@"TOTK_CUSTOM_PICTURE"];
    totk_key_layer layer_of = custom ? TOTK_KEY_CUSTOM : TOTK_KEY_PRESET;
    for (NSString *key in defaults) {
        totk_key_layer owned = totk_key_layer_of(key.UTF8String);
        if (owned != TOTK_KEY_ALWAYS && owned != layer_of) continue;
        id value = [d objectForKey:key];
        if ([value isKindOfClass:NSNumber.class]) {
            setenv(key.UTF8String, [value boolValue] ? "1" : "0", 0);
        } else if ([value isKindOfClass:NSString.class] && [value length] && [value length] < 64) {
            /* services.c takes a language code of up to 7 bytes and runtime_fails on a longer one. */
            if ([key isEqualToString:@"TOTK_LANG"] && [value length] > 7) continue;
            /* The pane offers "" (fill the screen, never handed over) and "0" (16:9). Anything else - the old
               "display" default, another platform's ratio - would reach aspect_install, whose phone branch treats every
               value but "0"/"16:9" as fill without saying so. */
            if ([key isEqualToString:@"TOTK_ASPECT"] && ![value isEqualToString:@"0"] && ![value isEqualToString:@"16:9"]) {
                fprintf(stderr, "[totk] settings: aspect ratio \"%s\" is not a pane value; filling the screen\n", [value UTF8String]);
                continue;
            }
            setenv(key.UTF8String, [value UTF8String], 0);
        }
        /* overwrite=0: a devicectl value for a plain key still wins. The layer block below owns the picture and frame
           keys of the chosen layer. */
    }
#if !TARGET_OS_VISION
    NSString *layer;
    if (custom) {
        /* The picture mode owns the drawable, the output upscale and the operation mode (the 1080 entry runs the
           game's docked settings). The scene upscale stays the pane's own switch, except for the MetalFX entry: its
           output upscale only runs while that switch is on, so the entry turns it on itself. */
        NSString *picture = [d stringForKey:@"TOTK_IOS_PICTURE_MODE"];
        totk_picture_env penv;
        if (!totk_picture_env_build(picture.UTF8String, &penv)) {
            fprintf(stderr, "[totk] settings: picture mode \"%s\" is not a pane value; using %s\n",
                    picture.UTF8String ?: "", totk_picture_mode_default());
            picture = [NSString stringWithUTF8String:totk_picture_mode_default()];
            totk_picture_env_build(picture.UTF8String, &penv);
        }
        setenv("TOTK_IOS_DRAWABLE_H", penv.drawable, 1);
        setenv("TOTK_MFX_OUTPUT", penv.output_upscale, 1);
        if (penv.scene_upscale) setenv("TOTK_MFX_UPSCALE", penv.scene_upscale, 1);
        apply_operation_mode(penv.operation);
        /* Frame rate is one choice: the cap. */
        NSString *cap = [d stringForKey:@"TOTK_FPS_CAP"];
        totk_frame_env fenv;
        if (!totk_frame_env_build(cap.UTF8String, &fenv)) {
            fprintf(stderr, "[totk] settings: frame cap \"%s\" is not a pane value; using %s\n",
                    cap.UTF8String ?: "", totk_frame_cap_default());
            cap = [NSString stringWithUTF8String:totk_frame_cap_default()];
            [d setObject:cap forKey:@"TOTK_FPS_CAP"];
            totk_frame_env_build(cap.UTF8String, &fenv);
        }
        apply_frame_env(&fenv, 1);
        /* A stored scene the pane no longer offers would leave the game at a size it cannot name; use the pane's
           default and say so. A value from devicectl is this launch's input rather than the player's stored choice,
           so it is left alone (the runtime validates it itself). */
        NSDictionary *scene_row = pref_row(root, @"TOTK_RENDER_RES");
        NSArray *scene_values = scene_row[@"Values"] ?: @[];
        NSString *fallback = scene_row[@"DefaultValue"] ?: @"960x540";
        NSString *scene_text = [d stringForKey:@"TOTK_RENDER_RES"];
        if (scene_values.count && (!scene_text || ![scene_values containsObject:scene_text])) {
            fprintf(stderr, "[totk] settings: scene \"%s\" is not a pane value; using %s\n",
                    scene_text.UTF8String ?: "", fallback.UTF8String);
            [d setObject:fallback forKey:@"TOTK_RENDER_RES"];
            if (!getenv("TOTK_RENDER_RES")) setenv("TOTK_RENDER_RES", fallback.UTF8String, 1);
        }
        layer = [NSString stringWithFormat:@"custom (%@, scene %s, %@ fps)", picture, getenv("TOTK_RENDER_RES") ?: "?", cap];
    } else {
        /* The presets: the Full HD switch owns the drawable and the scene, the frame row owns the pacing. Their picture
           is handed over by apply_display_settings when the game starts, after the start screen; here only the frame row
           goes (overwrite 0) and is checked, and apply_display_settings sets it again. */
        /* The row's own default, so the pane stays the only place it is written (the literal only covers a pane that
           lost the row). */
        NSDictionary *fps_row = pref_row(root, @"TOTK_PHONE_FPS");
        NSString *preset_fps = [d stringForKey:@"TOTK_PHONE_FPS"] ?: fps_row[@"DefaultValue"] ?: @"60";
        totk_frame_env pfenv;
        if (!totk_preset_frame_env_build(preset_fps.UTF8String, &pfenv)) {
            fprintf(stderr, "[totk] settings: preset frame rate \"%s\" is not a pane value; using 60\n", preset_fps.UTF8String);
            preset_fps = @"60";
            totk_preset_frame_env_build(preset_fps.UTF8String, &pfenv);
        }
        apply_frame_env(&pfenv, 0);
        layer = [NSString stringWithFormat:@"presets (%@, %@ fps)",
                 [d boolForKey:@"TOTK_DOCKED"] ? @"Full HD docked" : @"handheld", preset_fps];
    }
    /* The pane's rows are handed over as they are. Two overlays are their own switches (the runtime reads them
       independently, gfx/nvn/fps_overlay.c); they come through this loop with every other row, so a pane that is
       missing leaves the runtime's own default (on) in place. */
    fprintf(stderr, "[totk] settings: %s; drawable %s, scene %s, hz %s, phone %s, overlays %s/%s, taa %d, mfx %s/%s\n",
            layer.UTF8String,
            getenv("TOTK_IOS_DRAWABLE_H") ?: "default",
            getenv("TOTK_RENDER_RES") ?: "default",
            getenv("DM_PRESENT_HZ") ?: "-", getenv("TOTK_PHONE_FPS") ?: "-",
            getenv("TOTK_FPS_OVERLAY") ?: "1", getenv("TOTK_SHADER_OVERLAY") ?: "1",
            (int)[d boolForKey:@"TOTK_TAA"],
            getenv("TOTK_MFX_UPSCALE") ?: "1", getenv("TOTK_MFX_OUTPUT") ?: "0");
#endif
}

@interface TOTKApp : UIResponder <UIApplicationDelegate> @end
@implementation TOTKApp
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = TOTKSceneDelegate.class; return c;
}
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)options;
    // Controller input does not reset the idle timer; auto-lock would suspend the game mid-play or mid shader import.
    application.idleTimerDisabled = YES;
    audio_observe();
    /* The Mac launcher passes the preferred language as TOTK_LOCALE (PR #97); "zh-Hans-CN" maps to zh-Hans in services.c. */
    NSString *preferred = NSLocale.preferredLanguages.firstObject;
    if (preferred.length) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData"];
    for (NSString *name in @[@"save", @"temp", @"shader-cache", @"game-slot"]) [fm createDirectoryAtPath:[docs stringByAppendingPathComponent:name] withIntermediateDirectories:YES attributes:nil error:nil];
    /* The log is written only with debug on: the Settings app (Settings.bundle › TOTK_DEBUG_LOG) or TOTK_DEBUG_LOG=1 from
       devicectl (it wins over the setting). Off, stderr goes to /dev/null fully buffered, so nothing reaches the phone's
       storage (a line per write had stalled the game in bursts), and an old log.txt is removed. */
    NSString *log = [docs stringByAppendingPathComponent:@"log.txt"];
    const char *debugEnv = getenv("TOTK_DEBUG_LOG");
    BOOL debugLog = debugEnv ? (*debugEnv && strcmp(debugEnv,"0")) : [NSUserDefaults.standardUserDefaults boolForKey:@"TOTK_DEBUG_LOG"];
    if (debugLog) { if (freopen(log.fileSystemRepresentation,"w",stderr)) setvbuf(stderr,NULL,_IOLBF,0); setenv("TOTK_UW_CATALOG","1",0); }
    else { [fm removeItemAtPath:log error:nil]; if (freopen("/dev/null","w",stderr)) setvbuf(stderr,NULL,_IOFBF,1<<16); }
    apply_settings();
    setenv("TOTK_ROMFS",[res stringByAppendingPathComponent:@"romfs"].fileSystemRepresentation,1);
    setenv("TOTK_GAME_DATA",[res stringByAppendingPathComponent:@"game-data"].fileSystemRepresentation,1);   // runtime/game_data.h
    /* The 17 GB image may live in Documents/GameData instead of the app (romfs.bin or romfs.bin.000, ...; ios/tools/
       install_device.py --game-data puts it there): a new build then installs without it, and reinstalling keeps it.
       Excluded from iCloud backups. The runtime still hashes it once against the build's pin (TOTK_ROMFS_PROOF_DIR). */
    NSString *gameData = [docs stringByAppendingPathComponent:@"GameData"];
    BOOL image = [fm fileExistsAtPath:[gameData stringByAppendingPathComponent:@"romfs.bin"]] ||
                 [fm fileExistsAtPath:[gameData stringByAppendingPathComponent:@"romfs.bin.000"]];
    NSString *mark = [NSString stringWithContentsOfFile:[gameData stringByAppendingPathComponent:@"romfs.sha256"] encoding:NSUTF8StringEncoding error:nil];
    BOOL copying = mark && [mark stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length != 64;   // install_device.py writes the hash last
    if (image && !copying) {
        [[NSURL fileURLWithPath:gameData isDirectory:YES] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        setenv("TOTK_ROMFS_DATA",gameData.fileSystemRepresentation,1);
    } else if (![fm fileExistsAtPath:[res stringByAppendingPathComponent:@"romfs/romfs.bin"]]) {
        /* No image to play from: said on the screen instead of the game ending at once (runtime_fail exits). */
        BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];
        NSString *app = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: @"TOTK";   // the Files app's folder name
        g_no_game_data = copying || image
            ? [NSString stringWithFormat:(zh ? @"游戏数据还在复制到“文件 › 我的 iPhone › %@ › GameData”，复制完再打开游戏。"
                                             : @"The game data is still being copied to Files › On My iPhone › %@ › GameData. Open the game again when it is done."), app]
            : [NSString stringWithFormat:(zh ? @"没有游戏数据。请在 Mac 上用 ios/tools/install_device.py --game-data 把它复制到“文件 › 我的 iPhone › %@ › GameData”。"
                                             : @"No game data. On the Mac, copy it to Files › On My iPhone › %@ › GameData with ios/tools/install_device.py --game-data."), app];
        fprintf(stderr, "[totk] no game image: %s\n", g_no_game_data.UTF8String);
    }
    /* The startup shaders likewise (build_app.py --ios-game-data documents): install_device.py puts the seed into its own
       folder in Documents/GameData once per content and writes shader-seed.txt last ("<folder>\n<fingerprint>"), so the app
       carries none and a new build installs in seconds. Read in place like the app's own (totk_shader_seed.h); folders of
       earlier seeds are removed in the background. Without either seed the game has no shaders: said on the screen. */
    NSString *seedDir = [res stringByAppendingPathComponent:@"shader-seed"];
    NSArray<NSString *> *seedMark = [[NSString stringWithContentsOfFile:[gameData stringByAppendingPathComponent:@"shader-seed.txt"] encoding:NSUTF8StringEncoding error:nil] componentsSeparatedByString:@"\n"];
    NSString *pushedName = seedMark.count >= 2 && seedMark[1].length == 64 && [seedMark[0] hasPrefix:@"shader-seed-"] && ![seedMark[0] containsString:@"/"] ? seedMark[0] : nil;
    NSString *pushed = pushedName ? [gameData stringByAppendingPathComponent:pushedName] : nil;
    BOOL packed = pushed && ![fm fileExistsAtPath:[pushed stringByAppendingPathComponent:@"seed-manifest.json"]] &&
                  [fm fileExistsAtPath:[pushed stringByAppendingString:@".pack.000"]];
    if (packed) { g_seed_pack = [pushed stringByAppendingString:@".pack"]; g_seed_unpack_to = pushed; }   // unpacked in viewDidAppear
    if (pushed && (packed || [fm fileExistsAtPath:[pushed stringByAppendingPathComponent:@"seed-manifest.json"]])) {
        seedDir = pushed; setenv("TOTK_SHADER_SEED_PUSHED", pushed.fileSystemRepresentation, 1);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
            NSFileManager *files = [NSFileManager new];
            for (NSString *name in [files contentsOfDirectoryAtPath:gameData error:nil] ?: @[])
                if ([name hasPrefix:@"shader-seed-"] && ![name hasPrefix:pushedName]) [files removeItemAtPath:[gameData stringByAppendingPathComponent:name] error:nil];
        });
    } else if (!g_no_game_data && ![fm fileExistsAtPath:[seedDir stringByAppendingPathComponent:@"seed-manifest.json"]]) {
        BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];
        g_no_game_data = zh ? @"没有着色器。请在 Mac 上用 ios/tools/install_device.py 重新安装（会把着色器复制到手机上）。"
                            : @"No shaders. Install the game again from the Mac with ios/tools/install_device.py (it copies them to the device).";
        fprintf(stderr, "[totk] no shader seed: %s\n", g_no_game_data.UTF8String);
    }
#if TARGET_OS_VISION
    setenv("TOTK_OPERATION_MODE","docked",1);
    setenv("TOTK_IOS_DRAWABLE_H","1080",0);
    setenv("TOTK_RENDER_RES","1600x900",0);   // avoid the iPhone's default 960x540 in 1.4.3
    setenv("TOTK_MFX_UPSCALE","0",0);
    setenv("TOTK_FPS_OVERLAY","0",0);
#else
    /* The picture and frame rate are applied when the game starts (apply_display_settings), after the start screen. */
#endif
    // Game language: the Settings app (Settings.bundle › TOTK_LANG) stores the choice in our defaults; empty means the
    // system language, which is what the runtime already follows. iOS 27 does not honour Info.plist LSEnvironment, so
    // the value must be applied here. overwrite=0 keeps devicectl --environment-variables winning when testing from the Mac.
    NSString *lang = [NSUserDefaults.standardUserDefaults stringForKey:@"TOTK_LANG"];
    if (lang.length>0&&lang.length<8) setenv("TOTK_LANG",lang.UTF8String,0);
    setenv("TOTK_ROMFS_PATCHES",[res stringByAppendingPathComponent:@"romfs-patches"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PATCHES_OUT",[res stringByAppendingPathComponent:@"romfs-patches-out"].fileSystemRepresentation,1);
    setenv("TOTK_SHARED_FONTS",[res stringByAppendingPathComponent:@"shared-fonts"].fileSystemRepresentation,1);
    NSString *aoc = [res stringByAppendingPathComponent:@"aoc"];   // Breath of the Wild's add-on content packs (build_ipa.py; runtime/aoc_romfs.c)
    if ([fm fileExistsAtPath:aoc]) setenv("TOTK_AOC",aoc.fileSystemRepresentation,1);
    setenv("TOTK_SHADER_SEED_DIR",seedDir.fileSystemRepresentation,1);
    /* One folder per translation chain, as on the Mac: the runtime refuses translations of another chain, and the seed
       import keeps files that are already there. Builds before the v5 chain (2026-10-20) kept them directly in
       shader-cache; those, and other chains' folders, are only cache and are removed in the background. */
    NSString *cacheRoot = [docs stringByAppendingPathComponent:@"shader-cache"], *chainDir = @"rt-" DMSH_CHAIN;
    NSString *cache = [cacheRoot stringByAppendingPathComponent:chainDir];
    /* Test launches (devicectl --environment-variables '{"TOTK_SHADER_CACHE":"cache-test/1"}') can name another folder
       in Documents, e.g. an empty one to time a first start; it lies outside shader-cache, so the cleanup leaves it. */
    const char *testCache = getenv("TOTK_SHADER_CACHE");
    if (testCache && *testCache && !strstr(testCache, "..")) cache = [docs stringByAppendingPathComponent:@(testCache)];
    [fm createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("TOTK_SHADER_CACHE",cache.fileSystemRepresentation,1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        NSFileManager *files = [NSFileManager new]; NSUInteger removed = 0;
        for (NSString *name in [files contentsOfDirectoryAtPath:cacheRoot error:nil] ?: @[]) {
            if ([name isEqualToString:chainDir]) continue;
            if ([files removeItemAtPath:[cacheRoot stringByAppendingPathComponent:name] error:nil]) removed++;
        }
        if (removed) fprintf(stderr,"[shader-cache] removed %lu entries of earlier translation chains\n",(unsigned long)removed);
    });
    // Shaders the shared list lacks (totk_shader_contrib.h), in the Files app for players who want to send them; only the Mac asks.
    setenv("TOTK_SHADER_CONTRIB",[docs stringByAppendingPathComponent:@"shader-list-contribution.txt"].fileSystemRepresentation,1);
    // ... with the game and version it came from, so the Mac that sends it names them in the issue (shader_contrib_watch.py)
    setenv("TOTK_CONTRIB_GAME",GAME_FOLDER " " TOTK_GAME_VERSION,1);
    setenv("DM_NVN_SHMAP",[seedDir stringByAppendingPathComponent:@"shader_map.tsv"].fileSystemRepresentation,1);
    setenv("DM_MSL_DIR",seedDir.fileSystemRepresentation,1);
    NSString *font = [res stringByAppendingPathComponent:@"font"]; setenv("TOTK_FONT_DIR",font.fileSystemRepresentation,1);
    // Same player environment as the Mac launcher (app/launcher.m); the bundle is read-only, so state goes to Documents.
    setenv("TOTK_PROFILE_DIR",[docs stringByAppendingPathComponent:@"save"].fileSystemRepresentation,1);
    setenv("TOTK_GAME_SLOT_DIR",[docs stringByAppendingPathComponent:@"game-slot"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PROOF_DIR",docs.fileSystemRepresentation,1);   // hash the 17 GB RomFS once, not on every launch
    for (NSString *name in @[@"TOTK_GPU_ALLOW_SUBMIT",@"TOTK_GPU_WIDEN",@"TOTK_GPU_BLOCKRT",@"TOTK_GPU_MIDCLEAR"]) setenv(name.UTF8String,"1",1);
    setenv("TOTK_SLOT_OWNER","user",0); setenv("TOTK_SLOT_PURPOSE","player",0);
    /* Settings › Controller rumble / Controller gyro: the runtime reads TOTK_RUMBLE / TOTK_GYRO (runtime/feedback_settings.h,
       on unless "0"); a switch the player turned off is passed on, an explicit devicectl variable still wins (after PR #432). */
    for (NSString *key in @[@"TOTK_RUMBLE", @"TOTK_GYRO"]) {
        id saved = [NSUserDefaults.standardUserDefaults objectForKey:key];
        if (saved && ![saved boolValue]) setenv(key.UTF8String, "0", 0);
    }
    /* Settings › Use a hardware keyboard (on by default) and Keep touch with a keyboard (off by default): gamepad.m and
       touch_pad.m read TOTK_KEYBOARD ("0" off) and TOTK_TOUCH_WITH_KEYBOARD ("1" on) (after PR #431). */
    id keyboard = [NSUserDefaults.standardUserDefaults objectForKey:@"TOTK_KEYBOARD"];
    if (keyboard && ![keyboard boolValue]) setenv("TOTK_KEYBOARD", "0", 0);
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"TOTK_TOUCH_WITH_KEYBOARD"]) setenv("TOTK_TOUCH_WITH_KEYBOARD", "1", 0);
    unsetenv("TOTK_AUTOLOAD");
    chdir(docs.fileSystemRepresentation);
    host_argv[0] = strdup("TOTK"); host_argv[1] = strdup(res.fileSystemRepresentation); host_argv[2] = strdup("boot");
    return YES;   // the window is made when the window scene connects (TOTKSceneDelegate)
}
@end

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(TOTKApp.class)); } }
