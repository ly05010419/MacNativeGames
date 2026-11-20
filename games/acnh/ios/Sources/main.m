/* The Animal Crossing: New Horizons iPhone / iPad app (forked from games/dread/ios/Sources/main.m): a full-screen Metal
   view, the player environment of the Mac launcher (scripts/acnh_app_launcher.c) with every path in the app or its
   Documents, and the loader's main (runtime/loader.c, built as acnh_host_main) on a game thread. Play with a game controller or the on-screen controls (touch_controls.m), which hide while a controller is
   connected; the runtime's own boot screen (runtime/frontend_ios.m) says what happens. */
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "runtime.h"
#include "dmsh_chain.h"
#import "touch_controls.h"
#import "mng_active.h"   // the day's report of active players

int acnh_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
static char *host_argv[3];
static NSString *g_no_game_data;   // set when the app carries no RomFS image (didFinishLaunching)

static void *run_host(void *unused) { (void)unused; return (void *)(intptr_t)acnh_host_main(3, host_argv); }

/* The shaders translated on the Mac while playing (build_ipa.py: <work>/build/shader-cache, GameData/shader-cache in the
   app) into the cache in Documents: the device cannot translate (totk_shader_rt.m no_translator). The prepared seed
   (GameData/shader-seed) is imported by the runtime itself (totk_shader_seed.h). Files the device already has stay; a
   .variants index gains the rows it lacks, so the seed's rows in it are kept. */
static NSArray *variant_rows(NSString *path) {
    NSData *d = [NSData dataWithContentsOfFile:path]; id a = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    return [a isKindOfClass:NSArray.class] ? a : nil;
}
static unsigned long import_shaders(NSString *from, NSString *to) {
    NSFileManager *fm = NSFileManager.defaultManager; unsigned long copied = 0;
    for (NSString *name in [fm contentsOfDirectoryAtPath:from error:nil] ?: @[]) {
        NSString *src = [from stringByAppendingPathComponent:name], *dst = [to stringByAppendingPathComponent:name];
        if ([name hasSuffix:@".variants"] && [fm fileExistsAtPath:dst]) {
            NSArray *add = variant_rows(src); NSMutableArray *have = [variant_rows(dst) mutableCopy];
            if (!add || !have) continue;
            NSMutableSet *known = [NSMutableSet set];
            for (id row in have) if ([row isKindOfClass:NSDictionary.class] && row[@"variant"]) [known addObject:row[@"variant"]];
            BOOL changed = NO;
            for (id row in add) if ([row isKindOfClass:NSDictionary.class] && row[@"variant"] && ![known containsObject:row[@"variant"]]) { [have addObject:row]; changed = YES; }
            NSData *data = changed ? [NSJSONSerialization dataWithJSONObject:have options:NSJSONWritingSortedKeys error:nil] : nil;
            if (data && [data writeToFile:dst atomically:YES]) copied++;
            continue;
        }
        if (![fm fileExistsAtPath:dst] && [fm copyItemAtPath:src toPath:dst error:nil]) copied++;
    }
    return copied;
}

@interface ACNHMetalView : UIView @end
@implementation ACNHMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface ACNHGameController : UIViewController @property(nonatomic,strong) ACNHMetalView *gameView; @end
@implementation ACNHGameController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.gameView = [[ACNHMetalView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.gameView];
    runtime_ios_attach_layer((CAMetalLayer *)self.gameView.layer, self.gameView);
    ACNHTouchControls *pad = [[ACNHTouchControls alloc] initWithFrame:self.view.bounds];
    pad.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight; [self.view addSubview:pad];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started; if (started) return; started = YES;
    if (g_no_game_data) { runtime_boot_status("Cannot start the game", g_no_game_data.UTF8String); return; }
    /* Playback category: plays with the silent switch on and routes to the speaker; without an active session the
       CoreAudio output stays on a null sink. */
    NSError *err = nil;
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err];
    if (![AVAudioSession.sharedInstance setActive:YES error:&err]) fprintf(stderr, "[acnh] audio session not active: %s\n", err.localizedDescription.UTF8String);
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread; pthread_create(&thread, &attr, run_host, NULL);
    pthread_detach(thread); pthread_attr_destroy(&attr);
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

/* iOS 27 ends apps that keep the window in the app delegate, so the window belongs to a window scene. */
@interface ACNHSceneDelegate : UIResponder <UIWindowSceneDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation ACNHSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [ACNHGameController new]; [self.window makeKeyAndVisible];
}
@end

@interface ACNHApp : UIResponder <UIApplicationDelegate> @end
@implementation ACNHApp
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = ACNHSceneDelegate.class; return c;
}
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)options;
    mng_active_start(@"acnh", mng_active_version());   // the day's report of active players (mng_active.h)
    application.idleTimerDisabled = YES;   // controller input does not reset the idle timer
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData"];
    NSString *profile = [docs stringByAppendingPathComponent:@"profile"], *slot = [docs stringByAppendingPathComponent:@"game-slot"];
    NSString *cache = [docs stringByAppendingPathComponent:@"shader-cache/rt-" DMSH_CHAIN];
    for (NSString *dir in @[profile, slot, cache]) [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    /* The log: Documents/acnh.log (the Files app shows it; ios/tools/install_device.py can copy it back), the run before
       as acnh.previous.log. Line buffered: a crash keeps the last lines. */
    NSString *log = [docs stringByAppendingPathComponent:@"acnh.log"], *prev = [docs stringByAppendingPathComponent:@"acnh.previous.log"];
    [fm removeItemAtPath:prev error:nil]; [fm moveItemAtPath:log toPath:prev error:nil];
    if (freopen(log.fileSystemRepresentation, "w", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
    if (![fm fileExistsAtPath:[res stringByAppendingPathComponent:@"romfs/romfs.bin"]])
        g_no_game_data = @"The app carries no game data. Build it again on the Mac with games/acnh/build.py --platform ios.";
    unsigned long shaders = import_shaders([res stringByAppendingPathComponent:@"shader-cache"], cache);
    fprintf(stderr, "[acnh] shaders: %lu files imported into %s\n", shaders, cache.fileSystemRepresentation);

    // The Mac launcher's player environment (app/launcher.m); the bundle is read-only, so state goes to Documents.
    NSString *preferred = NSLocale.preferredLanguages.firstObject; if (preferred.length) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
    setenv("DM_PRESENT_HZ", "60", 0);
    setenv("TOTK_ROMFS", [res stringByAppendingPathComponent:@"romfs"].fileSystemRepresentation, 1);
    setenv("TOTK_ROMFS_PATCHES", "0", 1); setenv("TOTK_ROMFS_MODS", "0", 1);
    setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
    setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
    setenv("TOTK_ROMFS_PROOF_DIR", docs.fileSystemRepresentation, 1);   // hash the 10 GB RomFS once, not on every launch
    setenv("TOTK_SHADER_CACHE", cache.fileSystemRepresentation, 1);
    setenv("TOTK_SHADER_SEED_DIR", [res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation, 1);   // scripts/prepare_shader_seed.py
    // A variant the app lacks (skipped draws, [shader-miss]) as a shader-list row: merge it into assets/shader_list.tsv
    setenv("TOTK_SHADER_CONTRIB", [docs stringByAppendingPathComponent:@"shader-list-contribution.txt"].fileSystemRepresentation, 1);
    for (NSString *name in @[@"TOTK_GPU_ALLOW_SUBMIT", @"TOTK_GPU_WIDEN", @"TOTK_GPU_BLOCKRT", @"TOTK_GPU_MIDCLEAR"]) setenv(name.UTF8String, "1", 1);
    setenv("TOTK_GPU_BUDGET_MB", "3000", 0);   // the Mac's 5000 MB is more than an iPhone's footprint limit
    // Animal Crossing's own (scripts/acnh_app_launcher.c): the system fonts and the add-ons the app carries, the original
    // 30 FPS (no frame-rate assets: iOS cannot patch code at run time, runtime/frame_rate.c), its NVN settings
    setenv("TOTK_SHARED_FONTS", [res stringByAppendingPathComponent:@"fonts"].fileSystemRepresentation, 1);
    setenv("ACNH_DLC_DIR", [res stringByAppendingPathComponent:@"dlc"].fileSystemRepresentation, 1);
    unsetenv("ACNH_FPS_ASSETS"); unsetenv("ACNH_FPS120_ASSETS"); setenv("ACNH_FPS", "30", 1);
    setenv("DM_PRESERVE_INVARIANCE", "1", 1);
    setenv("DM_NVN_GPU_CODE_MAX_MINOR", "31", 1);
    setenv("TOTK_UNCAPPED", "0", 1);
    setenv("TOTK_MOVIE_PACE", "0", 1);
    setenv("TOTK_SHADER_ASYNC", "0", 0);
    setenv("TOTK_SLOT_OWNER", "user", 0); setenv("TOTK_SLOT_PURPOSE", "player", 0);
    unsetenv("TOTK_HEADLESS");
    chdir(docs.fileSystemRepresentation);
    host_argv[0] = strdup("acnh-native"); host_argv[1] = strdup(res.fileSystemRepresentation); host_argv[2] = strdup("boot");
    return YES;
}
@end

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ACNHApp.class)); } }
