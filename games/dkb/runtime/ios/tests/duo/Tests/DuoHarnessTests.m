#import <XCTest/XCTest.h>
#import <GameController/GameController.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#if __has_include(<UIKit/UIHingeInteraction.h>)
#import <UIKit/UIHingeInteraction.h>
#import <UIKit/UIViewReservedRegion.h>
#define DUO_HARNESS_HAS_NATIVE_POSE_API 1
#else
#define DUO_HARNESS_HAS_NATIVE_POSE_API 0
#endif
#import "DmLayout.h"
#import "DmGameMenu.h"
#import "DuoHarnessRuntime.h"
#include "dm_input.h"
#include <math.h>
#include <stdio.h>
void front_picture_shape(int w, int h);   // front_ios.m (dm/host/front.h)

static NSArray<GCController *> *gDuoHarnessControllers;
static BOOL gDuoHarnessControllerSwizzleInstalled;
static UIView *gDuoHarnessReservedRegionView;
static id gDuoHarnessReservedRegion;
static BOOL gDuoHarnessSawIncludeInactiveDivisionQuery;
static NSUInteger gDuoHarnessProbeViewClassSerial;

@interface GCController (DuoHarnessControllerList)
+ (NSArray<GCController *> *)duo_harness_controllers;
@end

@implementation GCController (DuoHarnessControllerList)
+ (NSArray<GCController *> *)duo_harness_controllers {
    @synchronized (GCController.class) {
        if (gDuoHarnessControllers) return gDuoHarnessControllers;
    }
    return [self duo_harness_controllers];
}
@end

@interface DuoFakeTouch : NSObject
@property (nonatomic) CGPoint point;
@property (nonatomic) NSTimeInterval timestamp;
- (CGPoint)locationInView:(UIView *)view;
@end

@implementation DuoFakeTouch
- (CGPoint)locationInView:(UIView *)view { (void)view; return self.point; }
@end

#if DUO_HARNESS_HAS_NATIVE_POSE_API
@interface DuoFakeHinge : NSObject
@property (nonatomic) NSInteger status;
@property (nonatomic) CGFloat angle;
@end
@implementation DuoFakeHinge
@end

@interface DuoFakeReservedRegion : NSObject
@property (nonatomic) CGRect frame;
@property (nonatomic) UIEdgeInsets margins;
@property (nonatomic, strong) id identifier;
@property (nonatomic, strong) id kind;
@property (nonatomic, getter=isActive) BOOL active;
@end
@implementation DuoFakeReservedRegion
@end

@interface UIViewController (DuoHarnessNativeHingeCallback)
- (void)hingeChanged:(id _Nullable)hinge;
- (void)startGame;
@end

static NSArray<UIViewReservedRegion *> *DuoHarnessReservedRegionsForProbeView(
    id view, SEL selector, UIViewReservedRegionKind *kind, UIViewReservedRegionQueryOptions options) API_AVAILABLE(ios(27.1)) {
    (void)selector;
    (void)kind;
    if (view == gDuoHarnessReservedRegionView) {
        if (options & UIViewReservedRegionQueryOptionsIncludeInactive) {
            gDuoHarnessSawIncludeInactiveDivisionQuery = YES;
            return gDuoHarnessReservedRegion ? @[(UIViewReservedRegion *)gDuoHarnessReservedRegion] : @[];
        }
        return @[];
    }
    return @[];
}

static Class DuoHarnessInstallReservedRegionProbe(UIView *view) API_AVAILABLE(ios(27.1)) {
    Class originalClass = object_getClass(view);
    char className[96];
    snprintf(className, sizeof(className), "DuoHarnessReservedRegionProbe%lu", (unsigned long)++gDuoHarnessProbeViewClassSerial);
    Class probeClass = objc_allocateClassPair(originalClass, className, 0);
    if (!probeClass) return Nil;
    Method queryMethod = class_getInstanceMethod(UIView.class, @selector(reservedRegionsOfKind:options:));
    if (!queryMethod || !class_addMethod(probeClass, @selector(reservedRegionsOfKind:options:),
                                         (IMP)DuoHarnessReservedRegionsForProbeView, method_getTypeEncoding(queryMethod))) {
        objc_disposeClassPair(probeClass);
        return Nil;
    }
    objc_registerClassPair(probeClass);
    object_setClass(view, probeClass);
    return originalClass;
}

#endif

@interface DuoHarnessTests : XCTestCase
@property (nonatomic, strong) id savedLayout;
@property (nonatomic) BOOL controllerListOverridden;
- (void)setHarnessControllers:(NSArray<GCController *> * _Nullable)controllers notification:(NSNotificationName _Nullable)notification;
- (UIViewController *)harnessRootViewController;
- (void)waitForView:(UIView *)view hidden:(BOOL)hidden;
@end

@implementation DuoHarnessTests
- (void)setUp {
    [super setUp];
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    self.savedLayout = [defaults objectForKey:@"DmPadLayout"];
    [defaults setObject:@1 forKey:@"DmPadLayout"];
    [self setHarnessControllers:@[] notification:GCControllerDidDisconnectNotification];
    duo_harness_clear();
}

- (void)tearDown {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if (self.savedLayout) [defaults setObject:self.savedLayout forKey:@"DmPadLayout"];
    else [defaults removeObjectForKey:@"DmPadLayout"];
    if (self.controllerListOverridden) {
        @synchronized (GCController.class) {
            Method original = class_getClassMethod(GCController.class, @selector(controllers));
            Method replacement = class_getClassMethod(GCController.class, @selector(duo_harness_controllers));
            method_exchangeImplementations(original, replacement);
            gDuoHarnessControllers = nil;
            gDuoHarnessControllerSwizzleInstalled = NO;
        }
        self.controllerListOverridden = NO;
    }
    [super tearDown];
}

- (void)setHarnessControllers:(NSArray<GCController *> * _Nullable)controllers notification:(NSNotificationName _Nullable)notification {
    @synchronized (GCController.class) {
        if (!gDuoHarnessControllerSwizzleInstalled) {
            Method original = class_getClassMethod(GCController.class, @selector(controllers));
            Method replacement = class_getClassMethod(GCController.class, @selector(duo_harness_controllers));
            method_exchangeImplementations(original, replacement);
            gDuoHarnessControllerSwizzleInstalled = YES;
            self.controllerListOverridden = YES;
        }
        gDuoHarnessControllers = [controllers copy];
    }
    if (notification) [[NSNotificationCenter defaultCenter] postNotificationName:notification object:nil];
}

- (void)waitForView:(UIView *)view hidden:(BOOL)hidden {
    NSPredicate *predicate = [NSPredicate predicateWithBlock:^BOOL(UIView *candidate, NSDictionary *bindings) {
        (void)bindings;
        return candidate.hidden == hidden;
    }];
    XCTNSPredicateExpectation *expectation = [[XCTNSPredicateExpectation alloc] initWithPredicate:predicate object:view];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[expectation] timeout:5.0], XCTWaiterResultCompleted);
}

- (void)deliverSnapshotChangeFor:(GCController *)controller button:(GCControllerButtonInput *)button value:(float)value {
    [button setValue:value];
    GCExtendedGamepad *profile = controller.extendedGamepad;
    GCExtendedGamepadValueChangedHandler handler = profile.valueChangedHandler;
    if (handler) handler(profile, button);
}

- (void)testPortraitSeparatesAcrossHingeStatesAndUsesNativeDivision {
    CGRect landscape = CGRectMake(0, 0, 844, 390);
    DmLayout overlay = dm_layout(landscape, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
    XCTAssertFalse(overlay.separated);
    XCTAssertTrue(CGRectEqualToRect(overlay.controls, landscape));
    XCTAssertEqualWithAccuracy(overlay.game.size.width / overlay.game.size.height, 16.0 / 9.0, 0.001);

    CGRect portrait = CGRectMake(0, 0, 390, 844);
    UIEdgeInsets safe = UIEdgeInsetsMake(59, 0, 34, 0);
    CGRect division = CGRectMake(0, 400, 390, 24);
    DmLayout separated = dm_layout(portrait, safe, NO, division, DM_PICTURE_16_9);
    XCTAssertTrue(separated.separated, @"Portrait uses a valid native horizontal division even with no confirmed hinge state");
    XCTAssertEqualWithAccuracy(CGRectGetMinY(separated.game), 119, 1);
    XCTAssertLessThanOrEqual(CGRectGetMaxY(separated.game), CGRectGetMinY(division));
    XCTAssertTrue(CGRectEqualToRect(separated.controls, CGRectMake(0, 424, 390, 420)));
    XCTAssertLessThanOrEqual(CGRectGetMaxY(separated.game), CGRectGetMinY(separated.controls), @"Video and controls must not overlap");

    DmLayout clipped = dm_layout(portrait, safe, NO, CGRectMake(-50, 400, 500, 24), DM_PICTURE_16_9);
    XCTAssertTrue(clipped.separated, @"A division that partially overlaps the view still separates its visible panels");
    XCTAssertEqualWithAccuracy(CGRectGetMinY(clipped.controls), 424, 0.001);
    XCTAssertEqualWithAccuracy(clipped.controls.size.width, portrait.size.width, 0.001);
    DmLayout flat = dm_layout(portrait, safe, NO, CGRectMake(0, 300, 390, 0), DM_PICTURE_16_9);
    XCTAssertTrue(flat.separated, @"A zero-height native division remains a useful split boundary");
    XCTAssertEqualWithAccuracy(CGRectGetMinY(flat.controls), 300, 0.001, @"Prefer a valid native split away from the fallback midpoint");
    XCTAssertLessThanOrEqual(CGRectGetMaxY(flat.game), 300);
    XCTAssertTrue(dm_layout(portrait, safe, YES, division, DM_PICTURE_16_9).separated, @"A partially-open display can use the same portrait split");
    XCTAssertTrue(dm_layout(portrait, safe, NO, CGRectNull, DM_PICTURE_16_9).separated, @"Portrait without a division still uses a midpoint split");
    XCTAssertTrue(dm_layout(portrait, safe, YES, CGRectMake(500, 400, 500, 24), DM_PICTURE_16_9).separated, @"Invalid native regions fall back to a portrait split");
    XCTAssertTrue(dm_layout(portrait, safe, NO, CGRectMake(180, 0, 24, 844), DM_PICTURE_16_9).separated, @"A vertical region falls back to a portrait split");
}

- (void)testPortraitFallbackFitsVideoAndControlsWithinSafeAreasAtNarrowSizes {
    const CGRect sizes[] = {CGRectMake(0, 0, 390, 844), CGRectMake(0, 0, 320, 568)};
    const UIEdgeInsets insets[] = {UIEdgeInsetsMake(59, 0, 34, 0), UIEdgeInsetsMake(50, 0, 24, 0)};
    for (NSUInteger i = 0; i < 2; i++) {
        CGRect bounds = sizes[i];
        CGFloat splitY = CGRectGetMidY(bounds);
        CGRect safe = UIEdgeInsetsInsetRect(bounds, insets[i]);
        DmLayout layout = dm_layout(bounds, insets[i], NO, CGRectNull, DM_PICTURE_16_9);
        XCTAssertTrue(layout.separated);
        XCTAssertTrue(CGRectEqualToRect(layout.controls, CGRectMake(0, splitY, bounds.size.width, bounds.size.height - splitY)));
        XCTAssertEqualWithAccuracy(layout.game.size.width / layout.game.size.height, 16.0 / 9.0, 0.001);
        XCTAssertGreaterThanOrEqual(CGRectGetMinY(layout.game), CGRectGetMinY(safe));
        XCTAssertLessThanOrEqual(CGRectGetMaxY(layout.game), splitY);
        XCTAssertLessThanOrEqual(CGRectGetMaxY(layout.game), CGRectGetMinY(layout.controls), @"The midpoint boundary leaves no video/control overlap");
    }
}

- (void)testLandscapeUsesOverlayExceptForPartiallyOpenNativeDivision {
    CGRect bounds = CGRectMake(0, 0, 844, 390);
    DmLayout overlay = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
    XCTAssertFalse(overlay.separated);
    XCTAssertTrue(CGRectEqualToRect(overlay.controls, bounds));
    XCTAssertTrue(CGRectEqualToRect(overlay.game, dm_aspect_fit(bounds, DM_PICTURE_16_9)));
    XCTAssertFalse(dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectMake(0, 180, 844, 24), DM_PICTURE_16_9).separated,
                   @"A fully-open landscape display retains the overlay layout");
    CGRect division = CGRectMake(0, 180, 844, 24);
    DmLayout separated = dm_layout(bounds, UIEdgeInsetsZero, YES, division, DM_PICTURE_16_9);
    XCTAssertTrue(separated.separated,
                  @"A landscape split still requires a partially-open hinge and a valid native division");
    XCTAssertEqualWithAccuracy(CGRectGetMaxY(separated.game), 180, 1);
    XCTAssertEqualWithAccuracy(CGRectGetMinY(separated.controls), 204, 0.001,
                               @"Keep the full reserved gap between video and controls");
    XCTAssertLessThanOrEqual(CGRectGetMaxY(separated.game), CGRectGetMinY(separated.controls));
}

- (void)testPortraitWithoutHingeStatusStillSplitsVideoAboveControls {
    DmLayout layout = dm_layout(CGRectMake(0, 0, 390, 844), UIEdgeInsetsMake(59, 0, 34, 0), NO, CGRectNull, DM_PICTURE_16_9);
    XCTAssertTrue(layout.separated);
    XCTAssertTrue(CGRectEqualToRect(layout.controls, CGRectMake(0, 422, 390, 422)));
    XCTAssertLessThanOrEqual(CGRectGetMaxY(layout.game), CGRectGetMinY(layout.controls));
}

- (void)testPortraitControlsUseAnchoredGroupsWithASeparatedThreeDimensionalLeftPair {
    struct { CGRect bounds; UIEdgeInsets insets; CGRect division; } cases[] = {
        {CGRectMake(0, 0, 669, 951), UIEdgeInsetsMake(62, 0, 34, 0), CGRectMake(0, 455.5, 669, 40)},
        {CGRectMake(0, 0, 390, 844), UIEdgeInsetsMake(59, 0, 34, 0), CGRectNull},
        {CGRectMake(0, 0, 320, 568), UIEdgeInsetsMake(50, 0, 24, 0), CGRectNull},
    };
    for (NSUInteger i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CGRect bounds = cases[i].bounds;
        DmLayout layout = [self portraitControlLayoutForBounds:bounds insets:cases[i].insets division:cases[i].division];
        XCTAssertTrue(layout.separated, @"Portrait case %lu must expose a controls panel", (unsigned long)i);
        XCTAssertLessThanOrEqual(CGRectGetMaxY(layout.game), CGRectGetMinY(layout.controls),
                                 @"Video and controls must remain disjoint in portrait case %lu", (unsigned long)i);
        UIView *pad = [self padForLayout:layout bounds:bounds];

        CGRect lower = layout.controls;
        CGFloat u = MIN(MIN(lower.size.height / 430.0, lower.size.width / 700.0), 1.3);
        CGPoint leftAnchor = CGPointMake(CGRectGetMinX(lower) + 0.28 * lower.size.width,
                                         CGRectGetMinY(lower) + 0.58 * lower.size.height);
        CGPoint rightAnchor = CGPointMake(CGRectGetMinX(lower) + 0.72 * lower.size.width, leftAnchor.y);
        CGRect stickRing = [self largestShapePathBoundsInLayer:pad.layer];
        CGPoint renderedStick = CGPointMake(CGRectGetMidX(stickRing), CGRectGetMidY(stickRing));
        CGPoint expectedStick = CGPointMake(leftAnchor.x - (48 * u + 6 * u), leftAnchor.y);
        XCTAssertEqualWithAccuracy(renderedStick.x, expectedStick.x, 0.5,
                                   @"The 3D joystick should sit left of the D-pad with their pair centered at the left anchor (case %lu)", (unsigned long)i);
        XCTAssertEqualWithAccuracy(renderedStick.y, expectedStick.y, 0.5);

        CGPoint faces = [self faceDiamondCenterInPad:pad];
        XCTAssertEqualWithAccuracy(faces.x, rightAnchor.x, 0.5,
                                   @"The ABXY diamond should be centered at the right portrait anchor (case %lu)", (unsigned long)i);
        XCTAssertEqualWithAccuracy(faces.y, rightAnchor.y, 0.5);

        CGPoint dpadCenter = CGPointMake(leftAnchor.x + (77 * u + 6 * u), leftAnchor.y);
        CGFloat artGap = dpadCenter.x - 48 * u - CGRectGetMaxX(stickRing);
        XCTAssertEqualWithAccuracy(artGap, 12 * u, 0.5,
                                   @"The rendered joystick and D-pad hit center should preserve a 12 scaled-point gap (case %lu)", (unsigned long)i);
        DuoFakeTouch *right = [self touchAt:CGPointMake(dpadCenter.x + 0.6 * 48 * u, dpadCenter.y) time:1];
        [self beginTouches:@[right] onPad:pad];
        XCTAssertEqual(duo_harness_input().buttons, DM_BTN_RIGHT,
                       @"The D-pad hit target should share its specified portrait position (case %lu)", (unsigned long)i);
        [self endTouches:@[right] onPad:pad cancelled:NO];

        XCTAssertGreaterThanOrEqual(CGRectGetMinX(stickRing), CGRectGetMinX(lower) - 0.5,
                                    @"The joystick remains inside the lower safe area (case %lu)", (unsigned long)i);
        XCTAssertLessThanOrEqual(CGRectGetMaxX(stickRing), CGRectGetMaxX(lower) + 0.5,
                                 @"The joystick remains inside the lower safe area (case %lu)", (unsigned long)i);
        XCTAssertGreaterThanOrEqual(faces.x - 77 * u, CGRectGetMinX(lower) - 0.5,
                                    @"The face cluster's expanded targets remain inside the lower safe area (case %lu)", (unsigned long)i);
        XCTAssertLessThanOrEqual(faces.x + 77 * u, CGRectGetMaxX(lower) + 0.5,
                                 @"The face cluster's expanded targets remain inside the lower safe area (case %lu)", (unsigned long)i);
    }
}

- (void)testPortraitTwoDimensionalStickAndFaceAnchorsAndKeepsDpadInactive {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    UIEdgeInsets insets = UIEdgeInsetsMake(59, 0, 34, 0);
    DmLayout layout = [self portraitControlLayoutForBounds:bounds insets:insets division:CGRectNull];
    [NSUserDefaults.standardUserDefaults setObject:@0 forKey:@"DmPadLayout"];
    UIView *pad = [self padForLayout:layout bounds:bounds];

    CGRect lower = layout.controls;
    CGFloat u = MIN(MIN(lower.size.height / 430.0, lower.size.width / 700.0), 1.3);
    CGPoint leftAnchor = CGPointMake(CGRectGetMinX(lower) + 0.28 * lower.size.width,
                                     CGRectGetMinY(lower) + 0.58 * lower.size.height);
    CGPoint rightAnchor = CGPointMake(CGRectGetMinX(lower) + 0.72 * lower.size.width, leftAnchor.y);
    CGRect stickRing = [self largestShapePathBoundsInLayer:pad.layer];
    XCTAssertEqualWithAccuracy(CGRectGetMidX(stickRing), leftAnchor.x, 0.5,
                               @"In 2D, the left stick itself occupies the left portrait anchor");
    XCTAssertEqualWithAccuracy(CGRectGetMidY(stickRing), leftAnchor.y, 0.5);
    CGPoint faces = [self faceDiamondCenterInPad:pad];
    XCTAssertEqualWithAccuracy(faces.x, rightAnchor.x, 0.5);
    XCTAssertEqualWithAccuracy(faces.y, rightAnchor.y, 0.5);

    DuoFakeTouch *stick = [self touchAt:leftAnchor time:1];
    [self beginTouches:@[stick] onPad:pad];
    [self moveTouch:stick to:CGPointMake(leftAnchor.x + 30 * u, leftAnchor.y) onPad:pad];
    XCTAssertGreaterThan(duo_harness_input().lx, 0,
                         @"A touch at the visible 2D stick must drive the left-stick mapping");
    [self endTouches:@[stick] onPad:pad cancelled:NO];

    CGPoint hiddenDpad = CGPointMake(leftAnchor.x + (77 + 6 + 0.6 * 48) * u, leftAnchor.y);
    DuoFakeTouch *dpad = [self touchAt:hiddenDpad time:2];
    [self beginTouches:@[dpad] onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons & DM_BTN_RIGHT, 0,
                   @"The 2D layout must not expose the 3D D-pad at its portrait 3D position");
    [self endTouches:@[dpad] onPad:pad cancelled:NO];
}

- (void)testPortraitThreeDimensionalAnchorsKeepStickDpadFaceAndGameTouchesIndependent {
    CGRect bounds = CGRectMake(0, 0, 669, 951);
    UIEdgeInsets insets = UIEdgeInsetsMake(62, 0, 34, 0);
    DmLayout layout = [self portraitControlLayoutForBounds:bounds insets:insets division:CGRectMake(0, 455.5, 669, 40)];
    UIView *pad = [self padForLayout:layout bounds:bounds];
    CGRect lower = layout.controls;
    CGFloat u = MIN(MIN(lower.size.height / 430.0, lower.size.width / 700.0), 1.3);
    CGPoint leftAnchor = CGPointMake(CGRectGetMinX(lower) + 0.28 * lower.size.width,
                                     CGRectGetMinY(lower) + 0.58 * lower.size.height);
    CGPoint stickCenter = CGPointMake(leftAnchor.x - 54 * u, leftAnchor.y);
    CGPoint dpadCenter = CGPointMake(leftAnchor.x + 83 * u, leftAnchor.y);
    CGPoint faceA = CGPointMake(CGRectGetMinX(lower) + 0.72 * lower.size.width + 50 * u, leftAnchor.y);
    CGPoint video = CGPointMake(CGRectGetMidX(layout.game), CGRectGetMidY(layout.game));
    DuoFakeTouch *stick = [self touchAt:stickCenter time:1];
    DuoFakeTouch *right = [self touchAt:CGPointMake(dpadCenter.x + 0.6 * 48 * u, dpadCenter.y) time:1];
    DuoFakeTouch *a = [self touchAt:faceA time:1];
    DuoFakeTouch *game = [self touchAt:video time:1];

    [self beginTouches:@[stick, right, a, game] onPad:pad];
    [self moveTouch:stick to:CGPointMake(stickCenter.x + 30 * u, stickCenter.y) onPad:pad];
    DuoHarnessInput input = duo_harness_input();
    XCTAssertGreaterThan(input.lx, 0, @"The stick touch should remain owned while three other touches begin");
    XCTAssertEqual(input.buttons & (DM_BTN_RIGHT | DM_BTN_A), DM_BTN_RIGHT | DM_BTN_A,
                   @"D-pad and ABXY retain separate button ownership");
    XCTAssertEqual(input.touch_count, 1, @"The game viewport keeps its own touch slot alongside the controls");
    XCTAssertEqualWithAccuracy(input.touches[0].x, 0.5, 0.001);
    XCTAssertEqualWithAccuracy(input.touches[0].y, 0.5, 0.001);
    [self endTouches:@[stick, right, a, game] onPad:pad cancelled:NO];
    input = duo_harness_input();
    XCTAssertEqual(input.buttons, 0);
    XCTAssertEqual(input.touch_count, 0);
}

- (void)testLandscapeControlAnchorsAndMultitouchKeepTheirExistingLayout {
    CGRect bounds = CGRectMake(0, 0, 844, 390);
    DmLayout layout = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
    XCTAssertFalse(layout.separated, @"Landscape keeps the overlay layout");
    XCTAssertTrue(CGRectEqualToRect(layout.game, dm_aspect_fit(bounds, DM_PICTURE_16_9)));
    UIView *pad = [self padForLayout:layout bounds:bounds];
    CGFloat u = MIN(bounds.size.height / 430.0, 1.3);
    CGRect stickRing = [self largestShapePathBoundsInLayer:pad.layer];
    XCTAssertEqualWithAccuracy(CGRectGetMidX(stickRing), 81 * u, 0.5);
    XCTAssertEqualWithAccuracy(CGRectGetMidY(stickRing), bounds.size.height - 82 * u, 0.5);
    CGPoint faces = [self faceDiamondCenterInPad:pad];
    XCTAssertEqualWithAccuracy(faces.x, bounds.size.width - 81 * u, 0.5);
    XCTAssertEqualWithAccuracy(faces.y, bounds.size.height - 82 * u, 0.5);

    CGPoint stickCenter = CGPointMake(81 * u, bounds.size.height - 82 * u);
    CGPoint dpadCenter = CGPointMake(231 * u, bounds.size.height - 64 * u);
    CGPoint aCenter = [self faceAIn:layout bounds:bounds];
    CGPoint video = CGPointMake(CGRectGetMidX(layout.game), CGRectGetMidY(layout.game));
    DuoFakeTouch *stick = [self touchAt:stickCenter time:1];
    DuoFakeTouch *right = [self touchAt:CGPointMake(dpadCenter.x + 0.6 * 48 * u, dpadCenter.y) time:1];
    DuoFakeTouch *a = [self touchAt:aCenter time:1];
    DuoFakeTouch *game = [self touchAt:video time:1];
    [self beginTouches:@[stick, right, a, game] onPad:pad];
    [self moveTouch:stick to:CGPointMake(stickCenter.x + 30 * u, stickCenter.y) onPad:pad];
    DuoHarnessInput input = duo_harness_input();
    XCTAssertGreaterThan(input.lx, 0);
    XCTAssertEqual(input.buttons & (DM_BTN_RIGHT | DM_BTN_A), DM_BTN_RIGHT | DM_BTN_A);
    XCTAssertEqual(input.touch_count, 1);
    [self endTouches:@[stick, right, a, game] onPad:pad cancelled:NO];
    input = duo_harness_input();
    XCTAssertEqual(input.buttons, 0);
    XCTAssertEqual(input.touch_count, 0);
}

- (void)testPictureFillCoversTheLandscapeViewWithoutStretching {
    XCTAssertTrue(dm_picture_fill("fill"));
    XCTAssertFalse(dm_picture_fill("fit"));
    XCTAssertFalse(dm_picture_fill(NULL), @"Unset keeps Fit, today's picture");
    XCTAssertFalse(dm_picture_fill(""));
    const CGRect sizes[] = {CGRectMake(0, 0, 844, 390), CGRectMake(0, 0, 932, 430), CGRectMake(0, 0, 1024, 768), CGRectMake(0, 0, 1280, 720)};
    for (NSUInteger i = 0; i < 4; i++) {
        CGRect bounds = sizes[i];
        DmLayout fit = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
        DmLayout fill = dm_layout_picture(fit, bounds, YES, DM_PICTURE_16_9);
        XCTAssertTrue(CGRectEqualToRect(dm_layout_picture(fit, bounds, NO, DM_PICTURE_16_9).game, fit.game), @"Fit leaves the layout as it was");
        XCTAssertFalse(fill.separated);
        XCTAssertTrue(CGRectEqualToRect(fill.controls, bounds), @"The touch controls keep the full view");
        XCTAssertEqualWithAccuracy(fill.game.size.width / fill.game.size.height, 16.0 / 9.0, 0.0001, @"Fill scales, never stretches");
        XCTAssertTrue(CGRectContainsRect(CGRectInset(fill.game, -0.001, -0.001), bounds), @"Fill covers the whole view");
        XCTAssertEqualWithAccuracy(CGRectGetMidX(fill.game), CGRectGetMidX(bounds), 0.001);
        XCTAssertEqualWithAccuracy(CGRectGetMidY(fill.game), CGRectGetMidY(bounds), 0.001);
        XCTAssertTrue(fill.game.size.width == bounds.size.width || fill.game.size.height == bounds.size.height,
                      @"The smallest cover: one side matches the view");
    }
    CGRect wide = dm_aspect_fill(CGRectMake(0, 0, 844, 390), DM_PICTURE_16_9);
    XCTAssertEqualWithAccuracy(wide.size.height, 474.75, 0.001);
    XCTAssertEqualWithAccuracy(CGRectGetMinY(wide), -42.375, 0.001, @"A wide iPhone crops the top and the bottom");
    CGRect pad = dm_aspect_fill(CGRectMake(0, 0, 1024, 768), DM_PICTURE_16_9);
    XCTAssertEqualWithAccuracy(CGRectGetMinX(pad), -170.667, 0.001, @"A 4:3 iPad crops the sides");
    XCTAssertTrue(CGRectIsEmpty(dm_aspect_fill(CGRectZero, DM_PICTURE_16_9)));
}

- (void)testPictureFillKeepsPortraitAndTabletopSplitsFitted {
    CGRect portrait = CGRectMake(0, 0, 390, 844);
    DmLayout split = dm_layout(portrait, UIEdgeInsetsMake(59, 0, 34, 0), NO, CGRectNull, DM_PICTURE_16_9);
    DmLayout filled = dm_layout_picture(split, portrait, YES, DM_PICTURE_16_9);
    XCTAssertTrue(filled.separated);
    XCTAssertTrue(CGRectEqualToRect(filled.game, split.game), @"Portrait keeps the picture fitted above the controls");
    CGRect landscape = CGRectMake(0, 0, 844, 390);
    DmLayout tabletop = dm_layout(landscape, UIEdgeInsetsZero, YES, CGRectMake(0, 180, 844, 24), DM_PICTURE_16_9);
    XCTAssertTrue(tabletop.separated);
    XCTAssertTrue(CGRectEqualToRect(dm_layout_picture(tabletop, landscape, YES, DM_PICTURE_16_9).game, tabletop.game), @"Tabletop keeps the picture fitted in its half");
}

- (void)testPictureFillTouchesFollowTheCroppedPicture {
    CGRect bounds = CGRectMake(0, 0, 844, 390);
    DmLayout layout = dm_layout_picture(dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9), bounds, YES, DM_PICTURE_16_9);
    CGPoint top = dm_game_touch_point(layout.game, CGPointMake(422, 0));
    XCTAssertEqualWithAccuracy(top.x, 0.5, 0.001);
    XCTAssertEqualWithAccuracy(top.y, 42.375 / 474.75, 0.001, @"The view's top edge is below the cropped top of the picture");
    CGPoint bottom = dm_game_touch_point(layout.game, CGPointMake(0, 390));
    XCTAssertEqualWithAccuracy(bottom.x, 0, 0.001);
    XCTAssertEqualWithAccuracy(bottom.y, (390 + 42.375) / 474.75, 0.001);
    CGPoint outside = dm_game_touch_point(layout.game, CGPointMake(-50, 900));
    XCTAssertEqualWithAccuracy(outside.x, 0, 0.0001);
    XCTAssertEqualWithAccuracy(outside.y, 0.999, 0.0001);

    UIView *pad = [self padForLayout:layout bounds:bounds];
    DuoFakeTouch *game = [self touchAt:CGPointMake(300, 150) time:1];
    [self beginTouches:@[game] onPad:pad];
    DuoHarnessInput input = duo_harness_input();
    XCTAssertEqual(input.touch_count, 1, @"A touch on no control goes to the game");
    XCTAssertEqualWithAccuracy(input.touches[0].x, 300 / 844.0, 0.001);
    XCTAssertEqualWithAccuracy(input.touches[0].y, (150 + 42.375) / 474.75, 0.001, @"Normalized on the scaled picture, not the view");
    [self endTouches:@[game] onPad:pad cancelled:NO];
    XCTAssertEqual(duo_harness_input().touch_count, 0);
}

- (void)testTallerPictureFillsAScreenOfItsShape {
    // DM_SCREEN_ASPECT=1.43: the game presents 1920x1344, and an unfolded iPhone Duo's landscape view has that shape
    CGFloat tall = 1920.0 / 1344.0;
    CGRect bounds = CGRectMake(0, 0, 1000, 700);
    DmLayout fit = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, tall);
    XCTAssertTrue(CGRectEqualToRect(fit.game, bounds), @"Fit fills the screen with the picture of its shape: %@", NSStringFromCGRect(fit.game));
    XCTAssertTrue(CGRectEqualToRect(dm_layout_picture(fit, bounds, YES, tall).game, bounds), @"Fill crops nothing");
    XCTAssertEqualWithAccuracy(dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9).game.size.height, 562.5, 0.001, @"A 16:9 picture leaves bars");
    CGPoint top = dm_game_touch_point(fit.game, CGPointMake(500, 0));
    XCTAssertEqualWithAccuracy(top.y, 0, 0.0001, @"The view's top edge is the picture's top edge");
    CGRect phone = CGRectMake(0, 0, 844, 390);
    CGRect wide = dm_layout(phone, UIEdgeInsetsZero, NO, CGRectNull, tall).game;
    XCTAssertEqualWithAccuracy(wide.size.height, 390, 0.001, @"A wider screen fits the taller picture by its height");
    XCTAssertEqualWithAccuracy(wide.size.width / wide.size.height, tall, 0.01, @"Never stretched");
}

- (void)testProductionRootFollowsThePresentedPictureShape {
    UIViewController *root = [self harnessRootViewController];
    UIView *game = [root valueForKey:@"game"];
    front_picture_shape(1920, 1344);
    XCTestExpectation *applied = [self expectationWithDescription:@"shape applied"];
    dispatch_async(dispatch_get_main_queue(), ^{ [applied fulfill]; });
    [self waitForExpectations:@[applied] timeout:2];
    [root.view layoutIfNeeded];
    BOOL portrait = root.view.bounds.size.height > root.view.bounds.size.width;
    XCTAssertEqualWithAccuracy(game.frame.size.width / game.frame.size.height, 1920.0 / 1344.0, 0.01, @"The game's view takes the presented shape: %@", NSStringFromCGRect(game.frame));
    if (!portrait) XCTAssertEqualWithAccuracy(game.frame.size.height, root.view.bounds.size.height, 1.0);
    front_picture_shape(1920, 1080);   // back to 16:9 for the other tests
    XCTestExpectation *restored = [self expectationWithDescription:@"16:9 restored"];
    dispatch_async(dispatch_get_main_queue(), ^{ [restored fulfill]; });
    [self waitForExpectations:@[restored] timeout:2];
    [root.view layoutIfNeeded];
    XCTAssertEqualWithAccuracy(game.frame.size.width / game.frame.size.height, 16.0 / 9.0, 0.01);
}

- (void)testSmallSeparatedHitTargetsMeetMinimumsWithoutGrowingArtwork {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    DmLayout layout = [self separatedLayout:bounds];
    UIView *pad = [self padForLayout:layout bounds:bounds];
    CGFloat u = MIN(MIN(layout.controls.size.height / 430.0, layout.controls.size.width / 700.0), 1.3);
    XCTAssertEqualWithAccuracy(u, 390.0 / 700.0, 0.001);

    // Probe just inside 44 pt face targets, inward from each visible button center. The
    // expected button remains the nearest target even where neighboring target circles overlap.
    NSArray<NSString *> *faces = @[@"A", @"B", @"X", @"Y"];
    const uint64_t faceBits[] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y};
    const CGPoint faceOffsets[] = {{-21.5, 0}, {0, -21.5}, {0, 21.5}, {21.5, 0}};
    for (NSUInteger i = 0; i < faces.count; i++) {
        NSString *name = faces[i];
        CGPoint center = [self controlCenterNamed:name inPad:pad];
        CGPoint probe = CGPointMake(center.x + faceOffsets[i].x, center.y + faceOffsets[i].y);
        XCTAssertLessThan([self controlPathBoundsNamed:name inPad:pad].size.width, 44);
        XCTAssertFalse([self controlNamed:name containsPoint:probe inPad:pad], @"%@ artwork should keep its scaled visual size", name);
        [self assertButton:faceBits[i] at:probe onPad:pad time:10 + i];
    }

    // Shoulder buttons, triggers, and stick clicks are frequent 44 pt targets too.
    NSArray<NSString *> *wideButtons = @[@"ZL", @"L", @"ZR", @"R", @"L3", @"R3"];
    const uint64_t wideBits[] = {DM_BTN_ZL, DM_BTN_L, DM_BTN_ZR, DM_BTN_R, DM_BTN_LS, DM_BTN_RS};
    // Probe outward on corner controls that overlap expanded L3/R3 targets.
    const CGPoint wideOffsets[] = {{0, -21.5}, {0, 21.5}, {0, -21.5}, {0, 21.5}, {21.5, 0}, {-21.5, 0}};
    for (NSUInteger i = 0; i < wideButtons.count; i++) {
        NSString *name = wideButtons[i];
        CGPoint center = [self controlCenterNamed:name inPad:pad];
        CGPoint probe = CGPointMake(center.x + wideOffsets[i].x, center.y + wideOffsets[i].y);
        XCTAssertLessThan([self controlPathBoundsNamed:name inPad:pad].size.width, 44);
        XCTAssertFalse([self controlNamed:name containsPoint:probe inPad:pad], @"%@ artwork should keep its scaled visual size", name);
        [self assertButton:wideBits[i] at:probe onPad:pad time:20 + i];
    }

    // Minus/plus use 28 pt targets. Their vertical probe is outside the drawn pill.
    NSArray<NSString *> *menuButtons = @[@"−", @"+"];
    const uint64_t menuBits[] = {DM_BTN_MINUS, DM_BTN_PLUS};
    for (NSUInteger i = 0; i < menuButtons.count; i++) {
        NSString *name = menuButtons[i];
        CGPoint center = [self controlCenterNamed:name inPad:pad];
        CGPoint probe = CGPointMake(center.x, center.y + 13.25);
        XCTAssertLessThan([self controlPathBoundsNamed:name inPad:pad].size.height, 28);
        XCTAssertFalse([self controlNamed:name containsPoint:probe inPad:pad], @"%@ artwork should keep its scaled visual size", name);
        [self assertButton:menuBits[i] at:probe onPad:pad time:30 + i];
    }

    UIButton *menu = [self menuButtonInPad:pad];
    CGPoint menuProbe = CGPointMake(menu.center.x, menu.center.y + 13.7);
    XCTAssertLessThan(menu.bounds.size.width, 28);
    XCTAssertFalse(CGRectContainsPoint(menu.frame, menuProbe), @"The menu button's artwork keeps its scaled size");
    XCTAssertEqual([pad hitTest:menuProbe withEvent:nil], menu, @"Its 44 pt target answers outside the drawn circle");
}

- (void)testExpandedTargetsRemainReachableWithinSafeAreaInNarrowSeparated {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    DmLayout layout = [self separatedLayout:bounds];
    // Model a 16 pt side safe area and 34 pt bottom safe area without depending on
    // the simulator's current display insets.
    layout.controls = CGRectMake(16, CGRectGetMinY(layout.controls), bounds.size.width - 32,
                                 CGRectGetHeight(layout.controls) - 34);
    UIView *pad = [self padForLayout:layout bounds:bounds];
    CGRect controls = layout.controls;

    for (NSString *name in @[@"A", @"B", @"X", @"Y"]) {
        CGPoint center = [self controlCenterNamed:name inPad:pad];
        XCTAssertGreaterThanOrEqual(center.x - 22, CGRectGetMinX(controls) - 0.01);
        XCTAssertLessThanOrEqual(center.x + 22, CGRectGetMaxX(controls) + 0.01);
        XCTAssertGreaterThanOrEqual(center.y - 22, CGRectGetMinY(controls) - 0.01);
        XCTAssertLessThanOrEqual(center.y + 22, CGRectGetMaxY(controls) + 0.01);
    }
    for (NSString *name in @[@"ZL", @"L", @"ZR", @"R", @"L3", @"R3"]) {
        CGPoint center = [self controlCenterNamed:name inPad:pad];
        XCTAssertGreaterThanOrEqual(center.x - 22, CGRectGetMinX(controls) - 0.01);
        XCTAssertLessThanOrEqual(center.x + 22, CGRectGetMaxX(controls) + 0.01);
        XCTAssertGreaterThanOrEqual(center.y - 22, CGRectGetMinY(controls) - 0.01);
        XCTAssertLessThanOrEqual(center.y + 22, CGRectGetMaxY(controls) + 0.01);
    }

    CGPoint a = [self controlCenterNamed:@"A" inPad:pad];
    CGPoint b = [self controlCenterNamed:@"B" inPad:pad];
    DuoFakeTouch *right = [self touchAt:CGPointMake(a.x + 21.5, a.y) time:1];
    [self beginTouches:@[right] onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_A);
    [self endTouches:@[right] onPad:pad cancelled:NO];
    DuoFakeTouch *bottom = [self touchAt:CGPointMake(b.x, b.y + 21.5) time:2];
    [self beginTouches:@[bottom] onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_B);
    [self endTouches:@[bottom] onPad:pad cancelled:NO];

    CGPoint zl = [self controlCenterNamed:@"ZL" inPad:pad];
    DuoFakeTouch *left = [self touchAt:CGPointMake(zl.x - 21.5, zl.y) time:3];
    [self beginTouches:@[left] onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_ZL);
    [self endTouches:@[left] onPad:pad cancelled:NO];

    UIButton *menu = [self menuButtonInPad:pad];
    XCTAssertEqual([pad hitTest:CGPointMake(menu.center.x, CGRectGetMinY(controls) + 0.8) withEvent:nil], menu, @"The menu button stays reachable at the safe top edge");
}

- (void)testFaceTargetsKeepTheirMultiButtonSlideBehavior {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    DmLayout layout = [self separatedLayout:bounds];
    UIView *pad = [self padForLayout:layout bounds:bounds];
    CGPoint a = [self controlCenterNamed:@"A" inPad:pad];
    CGPoint b = [self controlCenterNamed:@"B" inPad:pad];
    DuoFakeTouch *touch = [self touchAt:a time:1];

    [self beginTouches:@[touch] onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_A);
    [self moveTouch:touch to:CGPointMake((a.x + b.x) / 2, (a.y + b.y) / 2) onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_A | DM_BTN_B,
                   @"A face touch near two enlarged targets should preserve the intentional union");
    [self moveTouch:touch to:b onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_B);
    [self endTouches:@[touch] onPad:pad cancelled:NO];
    XCTAssertEqual(duo_harness_input().buttons, 0);
}

- (void)testOverlappingLockedButtonTargetsChooseNearestAndStayLocked {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    UIView *pad = [self padForLayout:[self separatedLayout:bounds] bounds:bounds];
    CGPoint zl = [self controlCenterNamed:@"ZL" inPad:pad];
    CGPoint ls = [self controlCenterNamed:@"L3" inPad:pad];
    CGPoint nearerToL3 = CGPointMake(zl.x + (ls.x - zl.x) * 0.6, zl.y);
    DuoFakeTouch *touch = [self touchAt:nearerToL3 time:1];

    [self beginTouches:@[touch] onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_LS,
                   @"When enlarged shoulder/click targets overlap, the nearest normalized target wins");
    [self moveTouch:touch to:zl onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_LS,
                   @"A regular button stays owned by the touch-down selection while the finger moves");
    [self endTouches:@[touch] onPad:pad cancelled:NO];
    XCTAssertEqual(duo_harness_input().buttons, 0);
}

- (void)testMenuButtonAndMinusOverlapUseNearestTargetThenKeepOwnership {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    UIView *pad = [self padForLayout:[self separatedLayout:bounds] bounds:bounds];
    CGPoint minus = [self controlCenterNamed:@"−" inPad:pad];
    UIButton *menu = [self menuButtonInPad:pad];
    CGPoint m = menu.center;
    XCTAssertEqual([pad hitTest:minus withEvent:nil], pad, @"− at its own centre belongs to the pad");
    XCTAssertEqual([pad hitTest:m withEvent:nil], menu);
    UIView *last = pad; int flips = 0;
    for (int i = 0; i <= 40; i++) {   // from − to the menu button: the nearer normalized target wins, so the owner changes once
        CGPoint p = CGPointMake(minus.x + (m.x - minus.x) * i / 40.0, minus.y + (m.y - minus.y) * i / 40.0);
        UIView *v = [pad hitTest:p withEvent:nil];
        if (v != last) { flips++; last = v; }
    }
    XCTAssertEqual(flips, 1);
    DuoFakeTouch *minusTouch = [self touchAt:minus time:1];
    [self beginTouches:@[minusTouch] onPad:pad];
    [self moveTouch:minusTouch to:m onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_MINUS, @"A − touch slid onto the menu button stays −");
    [self endTouches:@[minusTouch] onPad:pad cancelled:NO];
    XCTAssertEqual(duo_harness_input().buttons, 0);
}

- (void)testMenuLayoutPickReleasesHeldKeysAndIsSaved {
    dm_set_opmode(0);
    CGRect bounds = CGRectMake(0, 0, 844, 390);
    DmLayout layout = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
    UIView *pad = [self padForLayout:layout bounds:bounds];
    dm_pad_layout_set(0);
    DuoFakeTouch *a = [self touchAt:[self faceAIn:layout bounds:bounds] time:1];
    [self beginTouches:@[a] onPad:pad];
    XCTAssertEqual(duo_harness_input().buttons, DM_BTN_A);
    dm_pad_layout_set(1);
    XCTAssertEqual(duo_harness_input().buttons, 0, @"Picking a layout releases every held key");
    XCTAssertEqual(dm_pad_layout(), 1);
    XCTAssertEqualObjects([NSUserDefaults.standardUserDefaults objectForKey:@"DmPadLayout"], @1, @"and is kept for the next start");
    XCTAssertFalse([self textLayerNamed:@"L3" inLayer:pad.layer].superlayer.hidden, @"the 3D layout's keys show");
    dm_pad_layout_set(2);
    UIButton *menu = [self menuButtonInPad:pad];
    XCTAssertFalse(menu.hidden);
    XCTAssertEqual([pad hitTest:menu.center withEvent:nil], menu, @"Tap keeps the menu button, so the menu is still reachable");
    [self endTouches:@[a] onPad:pad cancelled:YES];
}

- (void)testDockingDropsTheTapLayout {
    dm_set_opmode(0);
    CGRect bounds = CGRectMake(0, 0, 844, 390);
    [self padForLayout:dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9) bounds:bounds];
    dm_pad_layout_set(2);
    XCTAssertEqual(dm_pad_layout(), 2);
    dm_set_opmode(1); dm_gamepad_opmode_changed();
    XCTAssertEqual(dm_pad_layout(), 0, @"Docked, the game reads no touch screen: Tap goes");
    XCTAssertFalse(dm_pad_layout_offered(2));
    dm_pad_layout_set(2);
    XCTAssertEqual(dm_pad_layout(), 0, @"Tap cannot be picked while docked");
    XCTAssertEqualObjects([NSUserDefaults.standardUserDefaults objectForKey:@"DmPadLayout"], @0);
    dm_set_opmode(0);
}

- (void)testInGameMenuSettingsTakeEffectAtOnce {
    UIViewController *root = [self harnessRootViewController];
    XCTAssertNotNil(root);
    [root.view layoutIfNeeded];
    UIView *game = [root valueForKey:@"game"];
    XCTAssertTrue(dm_setting_live("DM_PICTURE_FIT", NULL) && dm_setting_live("DM_OPMODE", NULL) && dm_setting_live("DM_RUMBLE", NULL)
                  && dm_setting_live("DM_SHOW_FPS", NULL) && dm_setting_live("DM_TOUCH_HAPTICS", NULL));
    XCTAssertFalse(dm_setting_live("DM_LANG", NULL), @"A setting the game reads once at start is not offered");
    XCTAssertFalse(dm_setting_live("DM_FPS_TARGET", NULL));

    CGRect b = root.view.bounds;
    BOOL duo = [[root valueForKey:@"hingeStatus"] integerValue] != 0;
    DmLayout fit = dm_layout(b, dm_game_safe_insets(b, root.view.safeAreaInsets, duo), [[root valueForKey:@"partiallyOpen"] boolValue], CGRectNull, DM_PICTURE_16_9);
    dm_setting_live("DM_PICTURE_FIT", "fill"); [root.view layoutIfNeeded];
    XCTAssertTrue(CGRectEqualToRect(game.frame, dm_layout_picture(fit, b, YES, DM_PICTURE_16_9).game), @"Fill applies at once: %@", NSStringFromCGRect(game.frame));
    dm_setting_live("DM_PICTURE_FIT", "fit"); [root.view layoutIfNeeded];
    XCTAssertTrue(CGRectEqualToRect(game.frame, fit.game), @"and Fit brings the bars back: %@", NSStringFromCGRect(game.frame));

    dm_setting_live("DM_RUMBLE", "0"); XCTAssertEqual(duo_harness_rumble(), 0);
    dm_setting_live("DM_RUMBLE", "1"); XCTAssertEqual(duo_harness_rumble(), 1);
    dm_setting_live("DM_OPMODE", "1"); XCTAssertEqual(dm_opmode(), 1);
    dm_setting_live("DM_OPMODE", "0"); XCTAssertEqual(dm_opmode(), 0);
    dm_setting_live("DM_SHOW_FPS", "1");
    UILabel *fps = [root valueForKey:@"fps"];
    XCTAssertNotNil(fps); XCTAssertFalse(fps.hidden);
    dm_setting_live("DM_SHOW_FPS", "0"); XCTAssertTrue(fps.hidden);
}

- (void)testGameViewDrawableHonoursAPixelCap {
    UIViewController *root = [self harnessRootViewController];
    [root.view layoutIfNeeded];
    UIView *game = [root valueForKey:@"game"];
    CAMetalLayer *metal = (CAMetalLayer *)game.layer;
    CGFloat full = metal.drawableSize.width;
    XCTAssertGreaterThan(full, 300);
    [game setValue:@300 forKey:@"maxPixelWidth"]; [game setNeedsLayout]; [game layoutIfNeeded];
    XCTAssertEqualWithAccuracy(metal.drawableSize.width, 300, 1, @"A TV's cap survives every later layout pass (DmGameView layoutSubviews)");
    [game setNeedsLayout]; [game layoutIfNeeded];
    XCTAssertEqualWithAccuracy(metal.drawableSize.width, 300, 1);
    [game setValue:@0 forKey:@"maxPixelWidth"]; [game setNeedsLayout]; [game layoutIfNeeded];
    XCTAssertEqualWithAccuracy(metal.drawableSize.width, full, 1);
}

- (void)testGamepadTextLayersRefreshScaleFromTheCurrentTraitCollection {
    if (@available(iOS 17.0, *)) {
        CGRect bounds = CGRectMake(0, 0, 844, 390);
        DmLayout layout = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
        UIView *pad = [self padForLayout:layout bounds:bounds];
        CATextLayer *face = [self textLayerNamed:@"A" inLayer:pad.layer];
        XCTAssertNotNil(face);
        UIViewController *root = nil;
        for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
            if (![scene isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow *window in ((UIWindowScene *)scene).windows) if (window.isKeyWindow) root = window.rootViewController;
        }
        XCTAssertNotNil(root);
        pad.frame = root.view.bounds;
        [root.view addSubview:pad];
        [root.view layoutIfNeeded];
        [pad layoutIfNeeded];
        CGRect visualBefore = [self controlPathBoundsNamed:@"A" inPad:pad];
        CGFloat currentScale = pad.traitCollection.displayScale;
        CGFloat overrideScale = fabs(currentScale - 1.0) < 0.01 ? 2.0 : 1.0;

        pad.traitOverrides.displayScale = overrideScale;
        [pad setNeedsLayout];
        [pad layoutIfNeeded];

        XCTAssertEqualWithAccuracy(pad.traitCollection.displayScale, overrideScale, 0.001);
        XCTAssertEqualWithAccuracy(face.contentsScale, overrideScale, 0.001);
        XCTAssertEqualWithAccuracy([self controlPathBoundsNamed:@"A" inPad:pad].size.width, visualBefore.size.width, 0.001,
                                   @"Changing the text raster scale must not resize the gamepad artwork");
        pad.traitOverrides.displayScale = currentScale;
        [pad removeFromSuperview];
    }
}

- (void)testFrontRootStartsProductionGamepadAndMatchesCurrentGeometry {
    UIViewController *root = [self harnessRootViewController];
    XCTAssertNotNil(root);
    [root.view layoutIfNeeded];
    UIView *game = [root valueForKey:@"game"];
    UIView *pad = [root valueForKey:@"pad"];
    XCTAssertNotNil(game);
    XCTAssertNotNil(pad);
    CGRect division = CGRectNull;
#if DUO_HARNESS_HAS_NATIVE_POSE_API
    if (@available(iOS 27.1, *)) {
        for (UIViewReservedRegion *region in [root.view reservedRegionsOfKind:UIViewReservedRegionKind.divisionRegionKind options:UIViewReservedRegionQueryOptionsIncludeInactive]) {
            CGRect candidate = dm_horizontal_division(root.view.bounds, region.frame);
            if (CGRectIsNull(division) && !CGRectIsNull(candidate)) division = candidate;
        }
    }
#endif
    BOOL partiallyOpen = [[root valueForKey:@"partiallyOpen"] boolValue];
    BOOL duo = [[root valueForKey:@"hingeStatus"] integerValue] != 0 || !CGRectIsNull(division);
    UIEdgeInsets insets = dm_game_safe_insets(root.view.bounds, root.view.safeAreaInsets, duo);
    DmLayout layout = dm_layout(root.view.bounds, insets, partiallyOpen, division, DM_PICTURE_16_9);
    XCTAssertTrue(CGRectEqualToRect(game.frame, layout.game),
                  @"The production game view must use the current scene bounds and native division region: actual %@, expected %@",
                  NSStringFromCGRect(game.frame), NSStringFromCGRect(layout.game));
    XCTAssertTrue(CGRectEqualToRect(pad.frame, root.view.bounds));
    XCTAssertTrue([game.layer isKindOfClass:CAMetalLayer.class]);
    CAMetalLayer *metal = (CAMetalLayer *)game.layer;
    UIScreen *screen = root.view.window.windowScene.screen;
    XCTAssertNotNil(screen);
    XCTAssertEqualWithAccuracy(metal.drawableSize.width, game.bounds.size.width * screen.nativeScale, 1.0);
    XCTAssertEqualWithAccuracy(metal.drawableSize.height, game.bounds.size.height * screen.nativeScale, 1.0);
}

- (void)testProductionRootRelayoutsMetalAndInputDuringWindowResizing {
    UIViewController *root = [self harnessRootViewController];
    UIView *game = [root valueForKey:@"game"];
    UIView *pad = [root valueForKey:@"pad"];
    NSPredicate *touchControlsActive = [NSPredicate predicateWithFormat:@"padOff == NO"];
    XCTNSPredicateExpectation *ready = [[XCTNSPredicateExpectation alloc] initWithPredicate:touchControlsActive object:pad];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[ready] timeout:5.0], XCTWaiterResultCompleted);
    CGRect original = root.view.frame;
    UIViewAutoresizing originalMask = root.view.autoresizingMask;
    root.view.autoresizingMask = UIViewAutoresizingNone;
    const CGSize sizes[] = {{1024, 768}, {744, 900}, {375, 650}, {640, 360}, {500, 500}, {1024, 768}};
    @try {
        for (NSUInteger i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            root.view.frame = (CGRect){CGPointZero, sizes[i]};
            [root.view setNeedsLayout];
            [root.view layoutIfNeeded];
            XCTAssertTrue(CGSizeEqualToSize(root.view.bounds.size, sizes[i]));
            XCTAssertTrue(CGRectEqualToRect(pad.frame, root.view.bounds));
            XCTAssertTrue(CGRectContainsRect(root.view.bounds, game.frame));
            XCTAssertEqualWithAccuracy(game.bounds.size.width / game.bounds.size.height, 16.0 / 9.0, 0.001);
            CAMetalLayer *metal = (CAMetalLayer *)game.layer;
            CGFloat scale = root.view.window.windowScene.screen.nativeScale;
            XCTAssertEqualWithAccuracy(metal.drawableSize.width, game.bounds.size.width * scale, 1.0);
            XCTAssertEqualWithAccuracy(metal.drawableSize.height, game.bounds.size.height * scale, 1.0);
            [self assertButton:DM_BTN_A at:[self controlCenterNamed:@"A" inPad:pad] onPad:pad time:i + 1];
            [pad setValue:@YES forKey:@"tap"];
            DuoFakeTouch *touch = [self touchAt:CGPointMake(CGRectGetMidX(game.frame), CGRectGetMidY(game.frame)) time:i + 1];
            [self beginTouches:@[touch] onPad:pad];
            XCTAssertEqual(duo_harness_input().touch_count, 1);
            XCTAssertEqualWithAccuracy(duo_harness_input().touches[0].x, 0.5, 0.001);
            XCTAssertEqualWithAccuracy(duo_harness_input().touches[0].y, 0.5, 0.001);
            [self endTouches:@[touch] onPad:pad cancelled:NO];
            [pad setValue:@NO forKey:@"tap"];
        }
    } @finally {
        root.view.frame = original;
        root.view.autoresizingMask = originalMask;
        [root.view setNeedsLayout];
        [root.view layoutIfNeeded];
    }
}

- (void)testResizingReleasesHeldInputAndUpdatesVideoTouchCoordinates {
    const CGRect windows[] = {CGRectMake(0, 0, 1024, 768), CGRectMake(0, 0, 320, 480),
                              CGRectMake(0, 0, 640, 360), CGRectMake(0, 0, 500, 500)};
    UIView *pad = [self padForLayout:dm_layout(windows[0], UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9) bounds:windows[0]];
    for (NSUInteger i = 0; i < sizeof windows / sizeof windows[0]; i++) {
        CGRect bounds = windows[i];
        DmLayout layout = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
        pad.frame = bounds;
        dm_gamepad_arrange(pad, layout);
        [pad layoutIfNeeded];
        XCTAssertEqual(duo_harness_input().buttons, 0);
        XCTAssertEqual(duo_harness_input().touch_count, 0);
        XCTAssertEqualWithAccuracy(layout.game.size.width / layout.game.size.height, 16.0 / 9.0, 0.001);
        [self assertButton:DM_BTN_A at:[self controlCenterNamed:@"A" inPad:pad] onPad:pad time:i + 1];
        // Tap mode isolates the game's touch viewport from the 3D right-stick gesture area.
        [pad setValue:@YES forKey:@"tap"];
        DuoFakeTouch *touch = [self touchAt:CGPointMake(CGRectGetMidX(layout.game), CGRectGetMidY(layout.game)) time:i + 1];
        [self beginTouches:@[touch] onPad:pad];
        DuoHarnessInput input = duo_harness_input();
        XCTAssertEqual(input.touch_count, 1);
        XCTAssertEqualWithAccuracy(input.touches[0].x, 0.5, 0.001);
        XCTAssertEqualWithAccuracy(input.touches[0].y, 0.5, 0.001);
        [pad setValue:@NO forKey:@"tap"];
    }
    dm_gamepad_arrange(pad, dm_layout(windows[0], UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9));
    XCTAssertEqual(duo_harness_input().touch_count, 0);
}

- (void)testDuoPortraitKeepsOnlyTopSafeArea {
    CGRect portrait = CGRectMake(0, 0, 744, 1133);
    UIEdgeInsets safe = UIEdgeInsetsMake(54, 20, 34, 24);
    UIEdgeInsets duo = dm_game_safe_insets(portrait, safe, YES);
    XCTAssertTrue(UIEdgeInsetsEqualToEdgeInsets(duo, UIEdgeInsetsMake(54, 0, 0, 0)));
    XCTAssertTrue(UIEdgeInsetsEqualToEdgeInsets(dm_game_safe_insets(portrait, safe, NO), safe));
    XCTAssertTrue(UIEdgeInsetsEqualToEdgeInsets(dm_game_safe_insets(CGRectMake(0, 0, 1133, 744), safe, YES), safe));
    DmLayout layout = dm_layout(portrait, duo, NO, CGRectNull, DM_PICTURE_16_9);
    XCTAssertEqual(CGRectGetMinX(layout.controls), 0);
    XCTAssertEqual(CGRectGetMaxX(layout.controls), 744);
    XCTAssertEqual(CGRectGetMaxY(layout.controls), 1133);
    XCTAssertGreaterThanOrEqual(CGRectGetMinY(layout.game), 54);
}



- (void)testRootOrientationMaskRemainsStableAcrossHingeStatuses {
    UIViewController *root = [self harnessRootViewController];
    XCTAssertNotNil(root);
    if (!root) return;
    UIInterfaceOrientationMask expectedMask = UIInterfaceOrientationMaskAll;
    XCTAssertEqual(root.supportedInterfaceOrientations, expectedMask);

#if DUO_HARNESS_HAS_NATIVE_POSE_API
    if (@available(iOS 27.1, *)) {
        SEL hingeCallback = @selector(hingeChanged:);
        XCTAssertTrue([root respondsToSelector:hingeCallback]);
        if (![root respondsToSelector:hingeCallback]) return;
        Class hingeClass = NSClassFromString(@"UIHingeInteraction");
        BOOL installed = NO;
        for (id interaction in root.view.interactions) if ([interaction isKindOfClass:hingeClass]) installed = YES;
        XCTAssertTrue(installed, @"The production root must observe native hinge state for layout only");

        [(id)root hingeChanged:nil];
        XCTAssertEqual(root.supportedInterfaceOrientations, expectedMask);
        XCTAssertEqualObjects([root valueForKey:@"partiallyOpen"], @NO);

        DuoFakeHinge *hinge = [DuoFakeHinge new];
        const NSInteger statuses[] = {UIHingeStatusClosed, UIHingeStatusUnknown,
                                      UIHingeStatusPartiallyOpen, UIHingeStatusFullyOpen};
        for (NSUInteger i = 0; i < sizeof statuses / sizeof statuses[0]; i++) {
            hinge.status = statuses[i];
            [(id)root hingeChanged:hinge];
            XCTAssertEqual(root.supportedInterfaceOrientations, expectedMask,
                           @"Hinge status %ld must not change the device's supported orientations", (long)statuses[i]);
            XCTAssertEqualObjects([root valueForKey:@"partiallyOpen"],
                                  @(statuses[i] == UIHingeStatusPartiallyOpen));
        }
    }
#endif

    NSData *plist = [NSData dataWithContentsOfURL:[NSBundle.mainBundle URLForResource:@"Info" withExtension:@"plist"]];
    NSDictionary *info = [NSPropertyListSerialization propertyListWithData:plist options:0 format:nil error:nil];
    NSSet *landscape = [NSSet setWithArray:@[@"UIInterfaceOrientationLandscapeLeft", @"UIInterfaceOrientationLandscapeRight"]];
    XCTAssertEqualObjects([NSSet setWithArray:info[@"UISupportedInterfaceOrientations"]], landscape);
    NSSet *all = [landscape setByAddingObjectsFromSet:[NSSet setWithArray:@[@"UIInterfaceOrientationPortrait", @"UIInterfaceOrientationPortraitUpsideDown"]]];
    XCTAssertEqualObjects([NSSet setWithArray:info[@"UISupportedInterfaceOrientations~ipad"]], all);
    XCTAssertNil(info[@"UIRequiresFullScreen"]);

}

- (void)testInactiveNativeDivisionCanDriveSeparatedLayout {
#if DUO_HARNESS_HAS_NATIVE_POSE_API
    if (@available(iOS 27.1, *)) {
        UIViewController *root = [self harnessRootViewController];
        XCTAssertNotNil(root);
        SEL hingeCallback = @selector(hingeChanged:);
        XCTAssertTrue([root respondsToSelector:hingeCallback]);
        if (![root respondsToSelector:hingeCallback]) return;

        UIView *view = root.view;
        Class originalClass = DuoHarnessInstallReservedRegionProbe(view);
        XCTAssertNotNil(originalClass);
        if (!originalClass) return;
        CGRect bounds = view.bounds;
        DuoFakeReservedRegion *region = [DuoFakeReservedRegion new];
        region.frame = CGRectMake(CGRectGetMinX(bounds), CGRectGetMidY(bounds) - 12, CGRectGetWidth(bounds), 24);
        region.identifier = [NSObject new];
        region.kind = UIViewReservedRegionKind.divisionRegionKind;
        region.active = NO;
        gDuoHarnessReservedRegionView = view;
        gDuoHarnessReservedRegion = region;
        gDuoHarnessSawIncludeInactiveDivisionQuery = NO;
        @try {
            DuoFakeHinge *hinge = [DuoFakeHinge new];
            hinge.status = UIHingeStatusPartiallyOpen;
            hinge.angle = 2.25;
            [(id)root hingeChanged:hinge];
            [view setNeedsUpdateConstraints]; [view updateConstraintsIfNeeded];
            [root viewDidLayoutSubviews];
            [view setNeedsLayout]; [view layoutIfNeeded];

            DmLayout expected = dm_layout(view.bounds, dm_game_safe_insets(view.bounds, view.safeAreaInsets, YES), YES, region.frame, DM_PICTURE_16_9);
            UIView *game = [root valueForKey:@"game"];
            XCTAssertTrue(gDuoHarnessSawIncludeInactiveDivisionQuery,
                          @"The root must query UIKit's IncludeInactive option for the division boundary");
            XCTAssertFalse(region.isActive, @"This boundary case must use an inactive native division");
            XCTAssertTrue(expected.separated);
            XCTAssertTrue(CGRectEqualToRect(game.frame, expected.game),
                          @"An inactive but valid horizontal division should still separate the panels");

            hinge.status = UIHingeStatusFullyOpen;
            [(id)root hingeChanged:hinge];
            [root viewDidLayoutSubviews];
            DmLayout fullyOpen = dm_layout(view.bounds, dm_game_safe_insets(view.bounds, view.safeAreaInsets, YES), NO, region.frame, DM_PICTURE_16_9);
            XCTAssertEqual(fullyOpen.separated, view.bounds.size.height > view.bounds.size.width,
                           @"A fully-open landscape display overlays while portrait remains separated");
            XCTAssertTrue(CGRectEqualToRect(game.frame, fullyOpen.game),
                          @"Fully-open layout follows the stable portrait-split, landscape-overlay policy");
        } @finally {
            object_setClass(view, originalClass);
            gDuoHarnessReservedRegionView = nil;
            gDuoHarnessReservedRegion = nil;
            gDuoHarnessSawIncludeInactiveDivisionQuery = NO;
            [(id)root hingeChanged:nil];
        }
    } else {
        XCTSkip(@"Reserved division regions are available from iOS 27.1");
    }
#else
    XCTSkip(@"This SDK does not contain UIView reserved-region queries");
#endif
}



- (UIViewController *)harnessRootViewController {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows) if (window.isKeyWindow) return window.rootViewController;
    }
    return nil;
}

- (DmLayout)separatedLayout:(CGRect)bounds {
    return dm_layout(bounds, UIEdgeInsetsZero, YES, CGRectMake(0, bounds.size.height * 0.48, bounds.size.width, 24), DM_PICTURE_16_9);
}

- (UIView *)padForLayout:(DmLayout)layout bounds:(CGRect)bounds {
    UIView *pad = dm_gamepad_view(bounds);
    dm_gamepad_arrange(pad, layout);
    [pad setNeedsLayout];
    [pad layoutIfNeeded];
    return pad;
}

- (DmLayout)portraitControlLayoutForBounds:(CGRect)bounds insets:(UIEdgeInsets)insets division:(CGRect)division {
    DmLayout layout = dm_layout(bounds, insets, NO, division, DM_PICTURE_16_9);
    CGRect safe = UIEdgeInsetsInsetRect(bounds, insets);
    CGFloat controlsTop = CGRectGetMinY(layout.controls);
    layout.controls = CGRectMake(CGRectGetMinX(safe), controlsTop, safe.size.width,
                                 MAX(0, CGRectGetMaxY(safe) - controlsTop));
    return layout;
}

- (void)collectShapePathBoundsInLayer:(CALayer *)layer result:(NSMutableArray<NSValue *> *)result {
    if ([layer isKindOfClass:CAShapeLayer.class]) {
        CGPathRef path = ((CAShapeLayer *)layer).path;
        if (path) [result addObject:[NSValue valueWithCGRect:CGPathGetBoundingBox(path)]];
    }
    for (CALayer *child in layer.sublayers) [self collectShapePathBoundsInLayer:child result:result];
}

- (CGRect)largestShapePathBoundsInLayer:(CALayer *)layer {
    NSMutableArray<NSValue *> *bounds = [NSMutableArray array];
    [self collectShapePathBoundsInLayer:layer result:bounds];
    CGRect largest = CGRectNull;
    CGFloat largestArea = 0;
    for (NSValue *value in bounds) {
        CGRect candidate = value.CGRectValue;
        CGFloat area = candidate.size.width * candidate.size.height;
        if (area > largestArea) { largest = candidate; largestArea = area; }
    }
    return largest;
}

- (CGPoint)faceDiamondCenterInPad:(UIView *)pad {
    CGPoint sum = CGPointZero;
    for (NSString *name in @[@"A", @"B", @"X", @"Y"]) {
        CGPoint center = [self controlCenterNamed:name inPad:pad];
        sum.x += center.x; sum.y += center.y;
    }
    return CGPointMake(sum.x / 4, sum.y / 4);
}

- (DuoFakeTouch *)touchAt:(CGPoint)point time:(NSTimeInterval)time {
    DuoFakeTouch *touch = [DuoFakeTouch new];
    touch.point = point; touch.timestamp = time;
    return touch;
}

- (void)beginTouches:(NSArray<DuoFakeTouch *> *)touches onPad:(UIView *)pad {
    [pad touchesBegan:(NSSet<UITouch *> *)[NSSet setWithArray:touches] withEvent:nil];
}

- (void)moveTouch:(DuoFakeTouch *)touch to:(CGPoint)point onPad:(UIView *)pad {
    touch.point = point;
    [pad touchesMoved:(NSSet<UITouch *> *)[NSSet setWithObject:touch] withEvent:nil];
}

- (void)endTouches:(NSArray<DuoFakeTouch *> *)touches onPad:(UIView *)pad cancelled:(BOOL)cancelled {
    NSSet<UITouch *> *set = (NSSet<UITouch *> *)[NSSet setWithArray:touches];
    if (cancelled) [pad touchesCancelled:set withEvent:nil];
    else [pad touchesEnded:set withEvent:nil];
}

- (CGPoint)faceAIn:(DmLayout)layout bounds:(CGRect)bounds {
    CGRect safe = CGRectIntersection(layout.controls, UIEdgeInsetsInsetRect(bounds, UIEdgeInsetsZero));
    CGFloat u = layout.separated ? MIN(MIN(safe.size.height / 430.0, safe.size.width / 700.0), 1.3) : MIN(bounds.size.height / 430.0, 1.3);
    if (layout.separated && bounds.size.height > bounds.size.width) {
        CGPoint faceCenter = CGPointMake(CGRectGetMinX(safe) + 0.72 * safe.size.width,
                                         CGRectGetMinY(safe) + 0.58 * safe.size.height);
        return CGPointMake(faceCenter.x + 50 * u, faceCenter.y);
    }
    return CGPointMake(CGRectGetMaxX(safe) - 81 * u + 50 * u, CGRectGetMaxY(safe) - 82 * u);
}

- (CATextLayer *)textLayerNamed:(NSString *)name inLayer:(CALayer *)layer {
    if ([layer isKindOfClass:CATextLayer.class] && [(id)((CATextLayer *)layer).string isEqual:name]) return (CATextLayer *)layer;
    for (CALayer *child in layer.sublayers) {
        CATextLayer *found = [self textLayerNamed:name inLayer:child];
        if (found) return found;
    }
    return nil;
}

- (UIButton *)menuButtonInPad:(UIView *)pad {
    for (UIView *v in pad.subviews) if ([v isKindOfClass:UIButton.class] && [v.accessibilityLabel isEqual:@"Menu"]) return (UIButton *)v;
    XCTFail(@"Expected the gamepad's menu button");
    return nil;
}

- (CGPoint)controlCenterNamed:(NSString *)name inPad:(UIView *)pad {
    CATextLayer *label = [self textLayerNamed:name inLayer:pad.layer];
    XCTAssertNotNil(label, @"Expected a visible control label named %@", name);
    return CGPointMake(CGRectGetMidX(label.frame), CGRectGetMidY(label.frame));
}

- (CGRect)controlPathBoundsNamed:(NSString *)name inPad:(UIView *)pad {
    CATextLayer *label = [self textLayerNamed:name inLayer:pad.layer];
    XCTAssertNotNil(label, @"Expected a visible control label named %@", name);
    CAShapeLayer *shape = (CAShapeLayer *)label.superlayer;
    XCTAssertTrue(shape.path != NULL, @"Expected the control to have a rendered path");
    return CGPathGetBoundingBox(shape.path);
}

- (BOOL)controlNamed:(NSString *)name containsPoint:(CGPoint)point inPad:(UIView *)pad {
    CATextLayer *label = [self textLayerNamed:name inLayer:pad.layer];
    XCTAssertNotNil(label, @"Expected a visible control label named %@", name);
    CAShapeLayer *shape = (CAShapeLayer *)label.superlayer;
    XCTAssertTrue(shape.path != NULL, @"Expected the control to have a rendered path");
    return CGPathContainsPoint(shape.path, NULL, point, false);
}

- (void)assertButton:(uint64_t)button at:(CGPoint)point onPad:(UIView *)pad time:(NSTimeInterval)time {
    DuoFakeTouch *touch = [self touchAt:point time:time];
    [self beginTouches:@[touch] onPad:pad];
    DuoHarnessInput input = duo_harness_input();
    XCTAssertEqual(input.buttons, button, @"The enlarged hit area should select exactly the nearest button");
    [self endTouches:@[touch] onPad:pad cancelled:NO];
}

- (void)testSimultaneousFaceButtonsAndVideoTouchKeepIndependentSlots {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    DmLayout layout = [self separatedLayout:bounds];
    UIView *pad = [self padForLayout:layout bounds:bounds];
    CGPoint a = [self faceAIn:layout bounds:bounds];
    CGFloat u = MIN(MIN(layout.controls.size.height / 430.0, layout.controls.size.width / 700.0), 1.3);
    CGPoint b = CGPointMake(a.x - 50 * u, a.y + 50 * u);
    CGPoint video = CGPointMake(CGRectGetMidX(layout.game), CGRectGetMidY(layout.game));
    DuoFakeTouch *ta = [self touchAt:a time:1], *tb = [self touchAt:b time:1], *tv = [self touchAt:video time:1];

    [self beginTouches:@[ta, tb, tv] onPad:pad];
    DuoHarnessInput input = duo_harness_input();
    XCTAssertEqual(input.buttons & (DM_BTN_A | DM_BTN_B), DM_BTN_A | DM_BTN_B);
    XCTAssertEqual(input.touch_count, 1);
    XCTAssertEqualWithAccuracy(input.touches[0].x, 0.5, 0.001);
    XCTAssertEqualWithAccuracy(input.touches[0].y, 0.5, 0.001);

    [self endTouches:@[ta] onPad:pad cancelled:NO];
    input = duo_harness_input();
    XCTAssertEqual(input.buttons & DM_BTN_A, 0);
    XCTAssertNotEqual(input.buttons & DM_BTN_B, 0);
    XCTAssertEqual(input.touch_count, 1);

    [self endTouches:@[tb, tv] onPad:pad cancelled:YES];
    input = duo_harness_input();
    XCTAssertEqual(input.buttons, 0);
    XCTAssertEqual(input.touch_count, 0);
}

- (void)testVideoCoordinatesClampAtCornersAndIgnoreFoldAndControlArea {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    DmLayout layout = [self separatedLayout:bounds];
    UIView *pad = [self padForLayout:layout bounds:bounds];
    DuoFakeTouch *touch = [self touchAt:CGPointMake(CGRectGetMinX(layout.game), CGRectGetMinY(layout.game)) time:1];
    [self beginTouches:@[touch] onPad:pad];
    DuoHarnessInput input = duo_harness_input();
    XCTAssertEqual(input.touch_count, 1);
    XCTAssertEqualWithAccuracy(input.touches[0].x, 0, 0.001);
    XCTAssertEqualWithAccuracy(input.touches[0].y, 0, 0.001);

    [self moveTouch:touch to:CGPointMake(CGRectGetMidX(layout.game), CGRectGetMidY(layout.game)) onPad:pad];
    input = duo_harness_input();
    XCTAssertEqual(input.touches[0].id, 0);
    XCTAssertEqualWithAccuracy(input.touches[0].x, 0.5, 0.001);
    XCTAssertEqualWithAccuracy(input.touches[0].y, 0.5, 0.001);

    [self moveTouch:touch to:CGPointMake(CGRectGetMaxX(layout.game) - 0.001, CGRectGetMaxY(layout.game) - 0.001) onPad:pad];
    input = duo_harness_input();
    XCTAssertEqualWithAccuracy(input.touches[0].x, 0.999, 0.001);
    XCTAssertEqualWithAccuracy(input.touches[0].y, 0.999, 0.001);

    [self endTouches:@[touch] onPad:pad cancelled:NO];
    DuoFakeTouch *foldGap = [self touchAt:CGPointMake(CGRectGetMidX(bounds), CGRectGetMinY(layout.controls) - 10) time:2];
    [self beginTouches:@[foldGap] onPad:pad];
    input = duo_harness_input();
    XCTAssertEqual(input.touch_count, 0);
    XCTAssertEqual(input.buttons, 0);
    [self endTouches:@[foldGap] onPad:pad cancelled:NO];

    CGRect lower = layout.controls;
    // The lower-panel midpoint can now fall on the portrait D-pad. Use the bottom-centre
    // safe-area strip, clear of the anchored thumb groups, to verify controls do not claim it.
    DuoFakeTouch *outside = [self touchAt:CGPointMake(CGRectGetMidX(lower), CGRectGetMaxY(lower) - 10) time:2];
    [self beginTouches:@[outside] onPad:pad];
    input = duo_harness_input();
    XCTAssertEqual(input.touch_count, 0);
    XCTAssertEqual(input.buttons, 0);
    [self endTouches:@[outside] onPad:pad cancelled:NO];

    DuoFakeTouch *rightThirdVideo = [self touchAt:CGPointMake(CGRectGetMaxX(layout.game) - 1, CGRectGetMidY(layout.game)) time:3];
    [self beginTouches:@[rightThirdVideo] onPad:pad];
    input = duo_harness_input();
    XCTAssertEqual(input.touch_count, 1);
    XCTAssertEqual(input.rx, 0);
    XCTAssertEqual(input.ry, 0);
    XCTAssertEqualWithAccuracy(input.touches[0].x, 0.997, 0.002);
    XCTAssertEqualWithAccuracy(input.touches[0].y, 0.5, 0.001);
    [self endTouches:@[rightThirdVideo] onPad:pad cancelled:NO];
}

- (void)testNoOpArrangePreservesHeldInputs {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    DmLayout separated = [self separatedLayout:bounds];
    UIView *pad = [self padForLayout:separated bounds:bounds];
    DuoFakeTouch *video = [self touchAt:CGPointMake(CGRectGetMidX(separated.game), CGRectGetMidY(separated.game)) time:1];
    DuoFakeTouch *button = [self touchAt:[self faceAIn:separated bounds:bounds] time:1];
    [self beginTouches:@[video, button] onPad:pad];

    dm_gamepad_arrange(pad, separated);
    DuoHarnessInput input = duo_harness_input();
    XCTAssertNotEqual(input.buttons & DM_BTN_A, 0);
    XCTAssertEqual(input.touch_count, 1);
    XCTAssertEqualWithAccuracy(input.touches[0].x, 0.5, 0.001);
    XCTAssertEqualWithAccuracy(input.touches[0].y, 0.5, 0.001);
    [self endTouches:@[video, button] onPad:pad cancelled:NO];
}

- (void)testPoseChangeReleasesHeldButtonAndGameTouch {
    CGRect bounds = CGRectMake(0, 0, 390, 844);
    DmLayout separated = [self separatedLayout:bounds];
    UIView *pad = [self padForLayout:separated bounds:bounds];
    DuoFakeTouch *video = [self touchAt:CGPointMake(CGRectGetMidX(separated.game), CGRectGetMidY(separated.game)) time:1];
    DuoFakeTouch *button = [self touchAt:[self faceAIn:separated bounds:bounds] time:1];
    [self beginTouches:@[video, button] onPad:pad];
    XCTAssertNotEqual(duo_harness_input().buttons & DM_BTN_A, 0);
    XCTAssertEqual(duo_harness_input().touch_count, 1);

    dm_gamepad_arrange(pad, dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9));
    DuoHarnessInput input = duo_harness_input();
    XCTAssertEqual(input.buttons, 0);
    XCTAssertEqual(input.touch_count, 0);
    [self endTouches:@[video, button] onPad:pad cancelled:NO];
}

- (void)testConnectedVirtualControllerKeepsVideoTouchAndMapsPhysicalButtons {
    if (@available(iOS 17.0, *)) {
        [self setHarnessControllers:nil notification:nil];
        CGRect bounds = CGRectMake(0, 0, 844, 390);
        DmLayout layout = dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9);
        UIView *pad = [self padForLayout:layout bounds:bounds];
        GCVirtualControllerConfiguration *config = [GCVirtualControllerConfiguration new];
        config.hidden = YES;
        config.elements = [NSSet setWithArray:@[GCInputButtonA, GCInputButtonB, GCInputButtonX, GCInputButtonY,
                                                 GCInputLeftShoulder, GCInputRightShoulder, GCInputButtonMenu,
                                                 GCInputLeftThumbstick, GCInputRightThumbstick, GCInputDirectionPad]];
        GCVirtualController *controller = [[GCVirtualController alloc] initWithConfiguration:config];
        XCTestExpectation *connected = [self expectationWithDescription:@"virtual controller connected"];
        __block NSError *connectError;
        [controller connectWithReplyHandler:^(NSError *error) { connectError = error; [connected fulfill]; }];
        [self waitForExpectations:@[connected] timeout:5];
        if (connectError || !controller.controller.extendedGamepad) {
            [controller disconnect];
            XCTSkip(@"Simulator virtual extended gamepad is unavailable");
        }
        [self setHarnessControllers:@[controller.controller] notification:GCControllerDidConnectNotification];

        [controller setValue:1 forButtonElement:GCInputButtonB];
        NSPredicate *hasA = [NSPredicate predicateWithBlock:^BOOL(id object, NSDictionary *bindings) {
            (void)object; (void)bindings; return (duo_harness_state().players[0].buttons & DM_BTN_A) != 0;
        }];
        [self expectationForPredicate:hasA evaluatedWithObject:self handler:nil];
        [self waitForExpectationsWithTimeout:5 handler:nil];

        CGRect portrait = CGRectMake(0, 0, 390, 844);
        DmLayout separated = [self separatedLayout:portrait];
        pad.frame = portrait;
        dm_gamepad_arrange(pad, separated);
        [pad setNeedsLayout]; [pad layoutIfNeeded];
        XCTAssertNotEqual(duo_harness_state().players[0].buttons & DM_BTN_A, 0,
                          @"Changing the UI pose must preserve player one's controller state");

        DuoFakeTouch *video = [self touchAt:CGPointMake(CGRectGetMidX(separated.game), CGRectGetMidY(separated.game)) time:1];
        [self beginTouches:@[video] onPad:pad];
        DuoHarnessInput input = duo_harness_input();
        XCTAssertNotEqual(duo_harness_state().players[0].buttons & DM_BTN_A, 0);
        XCTAssertEqual(input.touch_count, 1);
        XCTAssertEqualWithAccuracy(input.touches[0].x, 0.5, 0.001);
        XCTAssertEqualWithAccuracy(input.touches[0].y, 0.5, 0.001);
        [self endTouches:@[video] onPad:pad cancelled:NO];
        [controller setValue:0 forButtonElement:GCInputButtonB];
        [self setHarnessControllers:@[] notification:GCControllerDidDisconnectNotification];
        [controller disconnect];
    } else {
        XCTSkip(@"Virtual controllers require iOS 17 or later");
    }
}


- (void)testControllersKeepIndependentPlayersAndLayoutKeepsMotionState {
    if (@available(iOS 13.0, *)) {
        GCController *first = [GCController controllerWithExtendedGamepad];
        GCController *second = [GCController controllerWithExtendedGamepad];
        XCTAssertTrue(first.isSnapshot);
        XCTAssertTrue(second.isSnapshot);
        [self setHarnessControllers:@[first, second] notification:GCControllerDidConnectNotification];

        CGRect bounds = CGRectMake(0, 0, 844, 390);
        UIView *pad = [self padForLayout:dm_layout(bounds, UIEdgeInsetsZero, NO, CGRectNull, DM_PICTURE_16_9) bounds:bounds];
        [self deliverSnapshotChangeFor:first button:first.extendedGamepad.buttonB value:1];
        [self deliverSnapshotChangeFor:second button:second.extendedGamepad.buttonA value:1];
        DuoHarnessState state = duo_harness_state();
        XCTAssertEqual(state.connected_players, 0x3);
        XCTAssertEqual(state.players[0].buttons, DM_BTN_A);
        XCTAssertEqual(state.players[1].buttons, DM_BTN_B);
        GCExtendedGamepadValueChangedHandler staleFirstHandler = first.extendedGamepad.valueChangedHandler;

        const float acceleration[] = {0.25f, -0.5f, 0.75f};
        const float rotation[] = {-1.0f, 2.0f, -3.0f};
        dm_motion_set(1, acceleration, rotation);
        CGRect portrait = CGRectMake(0, 0, 390, 844);
        DmLayout separated = [self separatedLayout:portrait];
        pad.frame = portrait;
        dm_gamepad_arrange(pad, separated);
        [pad layoutIfNeeded];
        state = duo_harness_state();
        XCTAssertEqual(state.connected_players, 0x3);
        XCTAssertEqual(state.players[0].buttons, DM_BTN_A);
        XCTAssertEqual(state.players[1].buttons, DM_BTN_B);
        XCTAssertEqual(state.controller_motion_mask & (1u << 1), 1u << 1,
                       @"Changing the UI pose must not clear player two's motion source");
        for (NSUInteger axis = 0; axis < 3; axis++) {
            XCTAssertEqualWithAccuracy(state.controller_motion[1].acceleration[axis], acceleration[axis], 0.001);
            XCTAssertEqualWithAccuracy(state.controller_motion[1].rotation[axis], rotation[axis], 0.001);
        }

        // Reordering the connected-controller snapshot must not move either player's slot.
        [self setHarnessControllers:@[second, first] notification:GCControllerDidConnectNotification];
        [self setHarnessControllers:@[second] notification:GCControllerDidDisconnectNotification];
        state = duo_harness_state();
        XCTAssertEqual(state.connected_players, 0x2);
        XCTAssertEqual(state.players[0].buttons, 0);
        XCTAssertEqual(state.players[1].buttons, DM_BTN_B);
        XCTAssertEqual(state.controller_motion_mask & 0x3, 0x2,
                       @"Disconnecting player one must leave player two's motion source intact");
        if (staleFirstHandler) staleFirstHandler(first.extendedGamepad, first.extendedGamepad.buttonB);
        state = duo_harness_state();
        XCTAssertEqual(state.players[0].buttons, 0,
                       @"A queued callback from the disconnected controller must not reclaim its slot");
        XCTAssertEqual(state.players[1].buttons, DM_BTN_B);

        [self setHarnessControllers:@[] notification:GCControllerDidDisconnectNotification];
        state = duo_harness_state();
        XCTAssertEqual(state.connected_players, 0);
        XCTAssertEqual(state.players[1].buttons, 0);
        XCTAssertEqual(state.controller_motion_mask & (1u << 1), 0,
                       @"Disconnecting player two clears its motion source");
    }
}


@end
