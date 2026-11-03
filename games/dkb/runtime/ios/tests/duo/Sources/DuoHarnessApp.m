#import <UIKit/UIKit.h>
#import "DmLayout.h"
#import "DuoHarnessRuntime.h"
#include <stdlib.h>
#include <string.h>
#if __has_include(<UIKit/UIHingeInteraction.h>)
#import <UIKit/UIHingeInteraction.h>
#endif

UIViewController *front_ios_root(void);

static NSArray<NSNumber *> *DuoHarnessRectArray(CGRect rect) {
    return @[@(CGRectGetMinX(rect)), @(CGRectGetMinY(rect)), @(CGRectGetWidth(rect)), @(CGRectGetHeight(rect))];
}

static NSArray<NSArray<NSNumber *> *> *DuoHarnessTouchArray(DuoHarnessInput input) {
    NSMutableArray *touches = [NSMutableArray arrayWithCapacity:(NSUInteger)input.last_touch_count];
    for (int i = 0; i < input.last_touch_count; i++) {
        [touches addObject:@[@(input.last_touches[i].x), @(input.last_touches[i].y)]];
    }
    return touches;
}

@interface DuoHarnessDiagnosticsOverlay : UIView
@property (nonatomic, copy) void (^didLayout)(void);
@end

@implementation DuoHarnessDiagnosticsOverlay
- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.didLayout) self.didLayout();
}
@end

@interface DuoHarnessAppDelegate : UIResponder <UIApplicationDelegate>
@end

@interface DuoHarnessSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) UILabel *diagnosticsLabel;
@property (nonatomic, strong) id inputObserver;
@property (nonatomic) NSInteger diagnosticHingeStatus;
@property (nonatomic) CGFloat diagnosticHingeAngle;
#if __has_include(<UIKit/UIHingeInteraction.h>)
@property (nonatomic, strong) UIHingeInteraction *diagnosticHingeInteraction API_AVAILABLE(ios(27.1));
#endif
- (void)refreshDiagnostics;
@end

@implementation DuoHarnessAppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    // Synthetic touches should not start device motion or trigger hardware feedback.
    setenv("DM_MOTION", "0", 1);
    setenv("DM_TOUCH_HAPTICS", "0", 1);
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
    options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    return [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
}
@end

@implementation DuoHarnessSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *root = front_ios_root();
    self.window.rootViewController = root;
    [self.window makeKeyAndVisible];

    self.diagnosticHingeStatus = -1;
    self.diagnosticHingeAngle = -1;
    self.diagnosticsLabel = [UILabel new];
    self.diagnosticsLabel.frame = CGRectMake(0, 0, 1, 1);
    self.diagnosticsLabel.isAccessibilityElement = YES;
    self.diagnosticsLabel.accessibilityIdentifier = @"duo.diagnostics";
    self.diagnosticsLabel.accessibilityLabel = @"Duo native rotation diagnostics";
    self.diagnosticsLabel.accessibilityTraits = UIAccessibilityTraitStaticText;
    self.diagnosticsLabel.userInteractionEnabled = NO;

    DuoHarnessDiagnosticsOverlay *overlay = [[DuoHarnessDiagnosticsOverlay alloc] initWithFrame:root.view.bounds];
    overlay.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    overlay.backgroundColor = UIColor.clearColor;
    overlay.userInteractionEnabled = NO;
    [overlay addSubview:self.diagnosticsLabel];
    __weak DuoHarnessSceneDelegate *weakSelf = self;
    overlay.didLayout = ^{ [weakSelf refreshDiagnostics]; };
    [root.view addSubview:overlay];

    self.inputObserver = [NSNotificationCenter.defaultCenter addObserverForName:DuoHarnessInputDidChangeNotification
                                                                          object:nil
                                                                           queue:NSOperationQueue.mainQueue
                                                                      usingBlock:^(NSNotification *note) {
        (void)note;
        [weakSelf refreshDiagnostics];
    }];
#if __has_include(<UIKit/UIHingeInteraction.h>)
    if (@available(iOS 27.1, *)) {
        self.diagnosticHingeInteraction = [[UIHingeInteraction alloc] initWithUpdateHandler:^(UIHingeInteraction *interaction, UIHingeInteractionUpdate *update) {
            (void)interaction;
            UIHinge *hinge = update.hinge;
            weakSelf.diagnosticHingeStatus = hinge ? hinge.status : -1;
            weakSelf.diagnosticHingeAngle = hinge ? hinge.angle : -1;
            [weakSelf refreshDiagnostics];
        }];
        [root.view addInteraction:self.diagnosticHingeInteraction];
    }
#endif
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf refreshDiagnostics]; });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(NSEC_PER_SEC / 2)), dispatch_get_main_queue(), ^{
        [weakSelf.window.rootViewController.view layoutIfNeeded];
        [weakSelf refreshDiagnostics];
    });
}

- (void)windowScene:(UIWindowScene *)windowScene
    didUpdateEffectiveGeometry:(UIWindowSceneGeometry *)previousEffectiveGeometry API_AVAILABLE(ios(26.0)) {
    (void)windowScene; (void)previousEffectiveGeometry;
    __weak DuoHarnessSceneDelegate *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf refreshDiagnostics]; });
}

- (void)refreshDiagnostics {
    UIWindow *window = self.window;
    UIViewController *root = window.rootViewController;
    if (!window || !root || !self.diagnosticsLabel) return;
    UIWindowScene *scene = window.windowScene;
    UIView *game = [root valueForKey:@"game"];
    UIView *pad = [root valueForKey:@"pad"];
    if (!game || !pad) {
        self.diagnosticsLabel.accessibilityValue = @"Duo diagnostics unavailable: production gamepad has not started";
        return;
    }
    NSValue *storedLayout = [pad valueForKey:@"layout"];
    if (![storedLayout isKindOfClass:NSValue.class] || strcmp(storedLayout.objCType, @encode(DmLayout)) != 0) {
        self.diagnosticsLabel.accessibilityValue = @"Duo diagnostics unavailable: production gamepad layout is not readable";
        return;
    }
    self.diagnosticsLabel.frame = game.frame;
    DmLayout layout;
    [storedLayout getValue:&layout size:sizeof(layout)];

    DuoHarnessInput input = duo_harness_input();
    NSDictionary *snapshot = @{
        @"orientation": @(scene.effectiveGeometry.interfaceOrientation),
        @"bounds": DuoHarnessRectArray(root.view.bounds),
        @"game": DuoHarnessRectArray(game.frame),
        @"controls": DuoHarnessRectArray(layout.controls),
        @"separated": @(layout.separated),
        @"touchCount": @(input.touch_count),
        @"touchUpdates": @(input.touch_update_count),
        @"lastTouches": DuoHarnessTouchArray(input),
        @"hingeStatus": @(self.diagnosticHingeStatus),
        @"hingeAngle": @(self.diagnosticHingeAngle)
    };
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:snapshot options:0 error:&error];
    if (!data) {
        self.diagnosticsLabel.accessibilityValue = [NSString stringWithFormat:@"diagnostics error: %@", error];
        return;
    }
    NSString *value = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (![self.diagnosticsLabel.accessibilityValue isEqualToString:value]) self.diagnosticsLabel.accessibilityValue = value;
}

- (void)dealloc {
    if (self.inputObserver) [NSNotificationCenter.defaultCenter removeObserver:self.inputObserver];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(DuoHarnessAppDelegate.class)); }
}
