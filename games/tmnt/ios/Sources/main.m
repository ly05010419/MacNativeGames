/* UIKit shell for the TMNT host. Game code and resources are prepared locally. */
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

int tmnt_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *view);

@interface TMNTMetalView : UIView @end
@implementation TMNTMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

static char *gameArguments[3];
static void *run_game(void *unused) {
    (void)unused;
    @autoreleasepool { (void)tmnt_host_main(3, gameArguments); }
    return NULL;
}

@interface TMNTViewController : UIViewController {
    BOOL attached;
    BOOL appeared;
    BOOL started;
}
@end
@implementation TMNTViewController
- (void)loadView { self.view = [TMNTMetalView new]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!attached && self.view.bounds.size.width > 0 && self.view.bounds.size.height > 0) {
        runtime_ios_attach_layer((CAMetalLayer *)self.view.layer, self.view);
        attached = YES;
    }
    if (appeared) [self startGameIfReady];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    appeared = YES;
    [self startGameIfReady];
}
- (void)startGameIfReady {
    if (!attached || started) return;
    started = YES;
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *root = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"TMNT"];
    NSString *romfs = [root stringByAppendingPathComponent:@"romfs"];
    NSString *rewritten = [root stringByAppendingPathComponent:@"rewritten"];
    NSString *fonts = [root stringByAppendingPathComponent:@"fonts"];
    if (![files fileExistsAtPath:[romfs stringByAppendingPathComponent:@"runtime-index.bin"]] ||
        ![files fileExistsAtPath:[romfs stringByAppendingPathComponent:@"romfs.bin"]] ||
        ![files fileExistsAtPath:fonts] || ![files fileExistsAtPath:rewritten]) {
        UILabel *message = [[UILabel alloc] initWithFrame:self.view.bounds];
        message.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        message.text = @"TMNT: verified local game resources are not packaged.";
        message.textColor = UIColor.whiteColor;
        message.textAlignment = NSTextAlignmentCenter;
        message.numberOfLines = 0;
        [self.view addSubview:message];
        return;
    }
    NSString *documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject.path;
    NSString *profile = [documents stringByAppendingPathComponent:@"TMNT-profile"];
    NSString *shaders = [documents stringByAppendingPathComponent:@"TMNT-shaders"];
    if (![files createDirectoryAtPath:profile withIntermediateDirectories:YES attributes:nil error:nil] ||
        ![files createDirectoryAtPath:shaders withIntermediateDirectories:YES attributes:nil error:nil]) return;
    setenv("TOTK_ROMFS", romfs.fileSystemRepresentation, 1);
    setenv("TOTK_ROMFS_PROOF_DIR", profile.fileSystemRepresentation, 1);
    setenv("TOTK_SHARED_FONTS", fonts.fileSystemRepresentation, 1);
    setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
    setenv("TOTK_SHADER_CACHE", shaders.fileSystemRepresentation, 1);
    NSString *patches = [root stringByAppendingPathComponent:@"romfs-patches"];
    setenv("TOTK_ROMFS_PATCHES", patches.fileSystemRepresentation, 1);
    setenv("TOTK_ROMFS_MODS", "0", 1);
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
    setenv("TMNT_SPLIT_RECORDING", "1", 1);
    setenv("TMNT_STRICT_FRAME_DEPENDENCIES", "1", 1);
    setenv("TMNT_DRAW_TEXTURE_INVERT", "1", 1);
    if (chdir(documents.fileSystemRepresentation)) return;
    gameArguments[0] = strdup("TMNT-iPad");
    gameArguments[1] = strdup(rewritten.fileSystemRepresentation);
    gameArguments[2] = strdup("boot");
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 8u << 20);
    pthread_t gameThread;
    int result = pthread_create(&gameThread, &attributes, run_game, NULL);
    pthread_attr_destroy(&attributes);
    if (result) abort();
    pthread_detach(gameThread);
}
@end

@interface TMNTSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation TMNTSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [TMNTViewController new];
    [self.window makeKeyAndVisible];
}
@end

@interface TMNTAppDelegate : UIResponder <UIApplicationDelegate> @end
@implementation TMNTAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)application
 configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"TMNT" sessionRole:session.role];
    config.delegateClass = TMNTSceneDelegate.class;
    return config;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(TMNTAppDelegate.class)); }
}
