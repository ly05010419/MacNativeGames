#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include "runtime.h"
#include "dmsh_chain.h"
#import "touch_pad.h"
#include "profile_layout.h"
#include "user_picker_ios.h"

int totk_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
void runtime_ios_scene_active(int active);
static char *host_argv[3];
static NSString *g_no_game_data;   // set when there is no RomFS image to play from (didFinishLaunching)
static void *run_host(void *unused) { (void)unused; return (void *)(intptr_t)totk_host_main(3, host_argv); }
/* The desktop keyboard-overlay shortcut has no counterpart on iOS. */
void runtime_shortcut_help_toggle(void) {}

@interface D3MetalView : UIView @end
@implementation D3MetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface D3GameController : UIViewController @property(nonatomic,strong) D3MetalView *gameView; @end
@implementation D3GameController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.gameView = [[D3MetalView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.gameView];
    runtime_ios_attach_layer((CAMetalLayer *)self.gameView.layer, self.gameView);
    TOTKTouchPad *pad=[[TOTKTouchPad alloc] initWithFrame:self.view.bounds];pad.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:pad];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started; if (started) return; started = YES;
    if (g_no_game_data) {
        UILabel *say = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 40, 40)];
        say.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        say.text = g_no_game_data; say.textColor = UIColor.whiteColor; say.numberOfLines = 0; say.textAlignment = NSTextAlignmentCenter;
        [self.view addSubview:say];
        return;
    }
    /* Playback category (PR #97): plays with the ring / silent switch off and routes to the speaker. Without an active
       session the game's CoreAudio output stays on a null sink on iOS 27 ("no device sink in use"); activating it in
       didFinishLaunching blocked the main thread before the log file was opened. Interruptions: audio_observe. */
    AVAudioSession *audio = AVAudioSession.sharedInstance;
    NSError *catErr = nil, *actErr = nil;
    [audio setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&catErr];
    [audio setPreferredSampleRate:48000.0 error:nil];
    [audio setPreferredIOBufferDuration:0.010 error:nil];
    BOOL activated = [audio setActive:YES error:&actErr];
    fprintf(stderr, "[d3-audio] audio session category=%s active=%d route=%s err=%s%s\n",
            audio.category.UTF8String ?: "?", (int)activated, audio.currentRoute.outputs.firstObject.portType.UTF8String ?: "?",
            catErr ? catErr.localizedDescription.UTF8String : "none", actErr ? actErr.localizedDescription.UTF8String : "");
    /* Input-only diagnostics do not start the guest, load a save, or open shader caches. */
    if (getenv("TOTK_IOS_INPUT_PROBE")) {
        runtime_gamepad_start();
        UILabel *probe = [[UILabel alloc] initWithFrame:CGRectInset(self.view.bounds, 24, 24)];
        probe.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        probe.backgroundColor = UIColor.blackColor; probe.textColor = UIColor.whiteColor;
        probe.font = [UIFont monospacedSystemFontOfSize:18 weight:UIFontWeightRegular];
        probe.numberOfLines = 0; probe.textAlignment = NSTextAlignmentCenter;
        [self.view addSubview:probe];
        [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer) {
            (void)timer;
            GCController *c = GCController.controllers.firstObject;
            GCExtendedGamepad *p = c.extendedGamepad;
            probe.text = [NSString stringWithFormat:
                @"手柄输入诊断（不运行游戏）\n%@\n左摇杆 X=%+.3f  Y=%+.3f\n右摇杆 X=%+.3f  Y=%+.3f\n十字键 X=%+.1f  Y=%+.1f  A=%d  L3=%d\n先只转左摇杆 10 秒，再转右摇杆 5 秒，最后按 A\n%@",
                c ? (c.vendorName ?: @"已连接") : @"等待手柄连接",
                p.leftThumbstick.xAxis.value, p.leftThumbstick.yAxis.value,
                p.rightThumbstick.xAxis.value, p.rightThumbstick.yAxis.value,
                p.dpad.xAxis.value, p.dpad.yAxis.value, p.buttonA.isPressed, p.leftThumbstickButton.isPressed,
                getenv("TOTK_IOS_PRO_INPUT_MODE")
                    ? [NSString stringWithFormat:@"报告模式请求：%s（尚未确认生效）", getenv("TOTK_IOS_PRO_INPUT_MODE")]
                    : @"没有切换 HID 模式；没有修改映射设置"];
        }];
        fprintf(stderr, "[input-probe] guest disabled; no save/cache access\n");
        return;
    }
    void (^startGame)(void) = ^{
        pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64u << 20);
        pthread_t thread; pthread_create(&thread, &attr, run_host, NULL);
        pthread_detach(thread); pthread_attr_destroy(&attr);
    };
    if (getenv("MNG_USER_ID")) startGame();
    else mng_present_user_picker(self, @(getenv("TOTK_PROFILE_DIR")), startGame);
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

/* iOS 27 ends apps linked against its SDK that keep the window in the app delegate (UIKit traps in
   _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption at launch), so the window belongs to a window scene. */
@interface D3SceneDelegate : UIResponder <UIWindowSceneDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation D3SceneDelegate
/* Use the owning scene's lifecycle, not application-wide notifications.
   Never wait for GPU drain on UIKit's thread: a blocked commit needs this same
   thread to deliver didBecomeActive before it can proceed. */
- (void)sceneDidBecomeActive:(UIScene *)scene {
    if (scene != self.window.windowScene) return;
    runtime_ios_scene_active(1);
    UIApplication.sharedApplication.idleTimerDisabled = YES;
}
- (void)sceneWillResignActive:(UIScene *)scene {
    if (scene == self.window.windowScene) runtime_ios_scene_active(0);
}
- (void)sceneDidEnterBackground:(UIScene *)scene {
    if (scene == self.window.windowScene) runtime_ios_scene_active(0);
}
- (void)sceneDidDisconnect:(UIScene *)scene {
    if (scene == self.window.windowScene) runtime_ios_scene_active(0);
}
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [D3GameController new]; [self.window makeKeyAndVisible];
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
    if (![AVAudioSession.sharedInstance setActive:YES error:&err]) fprintf(stderr,"[d3-audio] audio session not active again (%s): %s\n",why,err.localizedDescription.UTF8String);
    g_audio_interrupted = NO; runtime_audio_output_restart(recreate);
}
static void audio_observe(void) {
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter; NSOperationQueue *main = NSOperationQueue.mainQueue;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        fprintf(stderr,"[d3-audio] audio session interruption %s\n",type == AVAudioSessionInterruptionTypeBegan ? "began" : "ended");
        if (type == AVAudioSessionInterruptionTypeBegan) g_audio_interrupted = YES; else audio_resume(0,"interruption ended");
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; fprintf(stderr,"[d3-audio] media services were reset\n"); audio_resume(1,"media services reset");
    }];
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; UIApplication.sharedApplication.idleTimerDisabled = YES;
        if (g_audio_interrupted) audio_resume(0,"became active during an interruption");
    }];
}

@interface D3App : UIResponder <UIApplicationDelegate> @end
@implementation D3App
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = D3SceneDelegate.class; return c;
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
    /* Test runs always use a new independent snapshot. Refuse reuse: never merge into player data. */
    NSString *testRoot = nil;
    const char *testName = getenv("TOTK_IOS_TEST_RUN");
    if (testName) {
        NSString *name = @(testName);
        NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"];
        if (!name.length || [name rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound) abort();
        testRoot = [[docs stringByAppendingPathComponent:@"test-runs"] stringByAppendingPathComponent:name];
        if ([fm fileExistsAtPath:testRoot]) abort();
        NSError *err = nil;
        if (![fm createDirectoryAtPath:testRoot withIntermediateDirectories:YES attributes:nil error:&err]) abort();
        NSString *layout = [docs stringByAppendingPathComponent:@"controls.txt"];
        if ([fm fileExistsAtPath:layout] && ![fm copyItemAtPath:layout toPath:[testRoot stringByAppendingPathComponent:@"controls.txt"] error:&err]) abort();
        for (NSString *entry in @[@"profile", @"shader-cache", @"game-slot"]) {
            NSString *source = [docs stringByAppendingPathComponent:entry], *dest = [testRoot stringByAppendingPathComponent:entry];
            if ([fm fileExistsAtPath:source]) {
                if (![fm copyItemAtPath:source toPath:dest error:&err]) abort();
            } else if (![entry isEqualToString:@"save"] && ![entry isEqualToString:@"profile"] && ![fm createDirectoryAtPath:dest withIntermediateDirectories:YES attributes:nil error:&err]) abort();
        }
    }
    NSString *profileError = ios_profile_prepare(testRoot ?: docs);
    if (profileError) { g_no_game_data = profileError; return YES; }
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData"];
    for (NSString *name in @[@"temp", @"shader-cache", @"game-slot"]) [fm createDirectoryAtPath:[(testRoot ?: docs) stringByAppendingPathComponent:name] withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *log = [(testRoot ?: docs) stringByAppendingPathComponent:@"log.txt"];
    if (freopen(log.fileSystemRepresentation,"w",stderr)) setvbuf(stderr,NULL,_IOLBF,0);
    setenv("TOTK_ROMFS",[res stringByAppendingPathComponent:@"romfs"].fileSystemRepresentation,1);
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
        NSString *app = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: @"Diablo III";   // the Files app's folder name
        g_no_game_data = copying || image
            ? [NSString stringWithFormat:(zh ? @"游戏数据还在复制到“文件 › 我的 iPhone › %@ › GameData”，复制完再打开游戏。"
                                             : @"The game data is still being copied to Files › On My iPhone › %@ › GameData. Open the game again when it is done."), app]
            : [NSString stringWithFormat:(zh ? @"没有游戏数据。请在 Mac 上用 ios/tools/install_device.py --game-data 把它复制到“文件 › 我的 iPhone › %@ › GameData”。"
                                             : @"No game data. On the Mac, copy it to Files › On My iPhone › %@ › GameData with ios/tools/install_device.py --game-data."), app];
        fprintf(stderr, "[d3] no game image: %s\n", g_no_game_data.UTF8String);
    }
    setenv("TOTK_OPERATION_MODE","handheld",1);
    /* Portable graphics with four external controller slots (tabletop play).
       Do not change performance mode just to expose multiplayer inputs. */
    setenv("TOTK_CONTROLLER_MODE","fullkey",1);
    // Game language: the Settings app (Settings.bundle › TOTK_LANG) stores the choice in our defaults; empty means the
    // system language. overwrite=0 keeps devicectl --environment-variables winning when testing from the Mac.
    NSString *lang = [NSUserDefaults.standardUserDefaults stringForKey:@"TOTK_LANG"];
    if (lang.length>0&&lang.length<8) setenv("TOTK_LANG",lang.UTF8String,0);
    setenv("TOTK_ROMFS_PATCHES","0",1);   // Diablo III ships no RomFS overlays
    setenv("TOTK_ROMFS_MODS","0",1);
    setenv("TOTK_AOC_DIR",[res stringByAppendingPathComponent:@"aoc"].fileSystemRepresentation,1);   // the add-on content markers (fs_host.c mount_aoc)
    setenv("TOTK_SHARED_FONTS",[res stringByAppendingPathComponent:@"shared-fonts"].fileSystemRepresentation,1);
    setenv("TOTK_SHADER_SEED_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    /* One folder per translation chain, as on the Mac; other chains' folders are only cache and are removed in the background. */
    NSString *cacheRoot = [(testRoot ?: docs) stringByAppendingPathComponent:@"shader-cache"], *chainDir = @"rt-" DMSH_CHAIN;
    NSString *cache = [cacheRoot stringByAppendingPathComponent:chainDir];
    /* Test launches (devicectl --environment-variables '{"TOTK_SHADER_CACHE":"cache-test/1"}') can name another folder
       in Documents, e.g. an empty one to time a first start; it lies outside shader-cache, so the cleanup leaves it. */
    const char *testCache = getenv("TOTK_SHADER_CACHE");
    if (!testRoot && testCache && *testCache && !strstr(testCache, "..")) cache = [docs stringByAppendingPathComponent:@(testCache)];
    [fm createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("TOTK_SHADER_CACHE",cache.fileSystemRepresentation,1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        NSFileManager *files = [NSFileManager new]; NSUInteger removed = 0;
        for (NSString *name in [files contentsOfDirectoryAtPath:cacheRoot error:nil] ?: @[]) {
            if ([name isEqualToString:chainDir] || getenv("TOTK_IOS_INPUT_PROBE")) continue;
            if ([files removeItemAtPath:[cacheRoot stringByAppendingPathComponent:name] error:nil]) removed++;
        }
        if (removed) fprintf(stderr,"[shader-cache] removed %lu entries of earlier translation chains\n",(unsigned long)removed);
    });
    // Shaders the bundled seed lacks (totk_shader_contrib.h), in the Files app; the Mac translates them (tools/fill_shaders.py).
    setenv("TOTK_SHADER_CONTRIB",[(testRoot ?: docs) stringByAppendingPathComponent:@"shader-list-contribution.txt"].fileSystemRepresentation,1);
    setenv("DM_NVN_SHMAP",[res stringByAppendingPathComponent:@"shader-seed/shader_map.tsv"].fileSystemRepresentation,1);
    setenv("DM_MSL_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    // Same player environment as the Mac launcher (app/launcher.m); the bundle is read-only, so state goes to Documents.
    setenv("TOTK_PROFILE_DIR",[(testRoot ?: docs) stringByAppendingPathComponent:@"profile"].fileSystemRepresentation,1);
    setenv("TOTK_GAME_SLOT_DIR",[(testRoot ?: docs) stringByAppendingPathComponent:@"game-slot"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PROOF_DIR",docs.fileSystemRepresentation,1);   // hash the 17 GB RomFS once, not on every launch
    setenv("TOTK_CONTROLS",[(testRoot ?: docs) stringByAppendingPathComponent:@"controls.txt"].fileSystemRepresentation,1);
    NSInteger savedFps = [NSUserDefaults.standardUserDefaults integerForKey:@"TOTK_IOS_FPS_CAP"];
    if (savedFps == 30 || savedFps == 60 || savedFps == 90 || savedFps == 120) runtime_fps_cap_set((int)savedFps);
    for (NSString *name in @[@"TOTK_GPU_ALLOW_SUBMIT",@"TOTK_GPU_WIDEN",@"TOTK_GPU_BLOCKRT",@"TOTK_GPU_MIDCLEAR",@"TOTK_SHADER_PREFETCH"]) setenv(name.UTF8String,"1",1);
    setenv("TOTK_SLOT_OWNER","user",0); setenv("TOTK_SLOT_PURPOSE","player",0);
    /* Commands for tests from the Mac (devicectl copy to Documents/input.txt), as the Mac launcher's input.txt. */
    NSString *cmds = [(testRoot ?: docs) stringByAppendingPathComponent:@"input.txt"];
    [NSData.data writeToFile:cmds atomically:YES];
    setenv("TOTK_INPUT_FILE",cmds.fileSystemRepresentation,0);
    unsetenv("TOTK_AUTOLOAD");
    chdir(docs.fileSystemRepresentation);
    host_argv[0] = strdup("D3"); host_argv[1] = strdup(res.fileSystemRepresentation); host_argv[2] = strdup("boot");
    return YES;   // the window is made when the window scene connects (D3SceneDelegate)
}
@end

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(D3App.class)); } }
