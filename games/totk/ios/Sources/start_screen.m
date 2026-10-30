#import "start_screen.h"
#import <GameController/GameController.h>
#include "totk_ui_lang.h"

static BOOL zh(void) { return totk_ui_zh(); }   // the language of the host's other text (boot screen, overlays; TOTK_UI_LANG overrides)
static NSString *T(NSString *z, NSString *e) { return zh() ? z : e; }

@implementation TOTKStartView {
    void (^_onStart)(void);
    UISegmentedControl *_picture, *_fps;
    UIButton *_start;
    NSInteger _row;          // 0 picture, 1 frame rate, 2 start (controller focus)
    NSTimer *_poll;
    BOOL _held, _done;       // a controller button or direction held since the last poll
    BOOL _hasPicture;        // the game's Settings pane has Full HD (docked): the picture row is shown
}

- (instancetype)initWithFrame:(CGRect)frame onStart:(void (^)(void))onStart {
    if (!(self = [super initWithFrame:frame])) return nil;
    _onStart = [onStart copy];
    _row = 2;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    CAGradientLayer *bg = [CAGradientLayer layer];
    bg.colors = @[(id)[UIColor colorWithRed:0.10 green:0.11 blue:0.15 alpha:1].CGColor, (id)[UIColor colorWithRed:0.04 green:0.05 blue:0.07 alpha:1].CGColor];
    bg.frame = self.bounds; bg.needsDisplayOnBoundsChange = YES;
    [self.layer addSublayer:bg];

    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    NSString *name = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: @"Game";

    UIImageView *icon = [[UIImageView alloc] initWithImage:[self appIcon]];
    icon.layer.cornerRadius = 28; icon.layer.cornerCurve = kCACornerCurveContinuous; icon.clipsToBounds = YES;
    icon.contentMode = UIViewContentModeScaleAspectFill; icon.backgroundColor = [UIColor colorWithWhite:1 alpha:0.08];
    [icon.widthAnchor constraintEqualToConstant:128].active = YES; [icon.heightAnchor constraintEqualToConstant:128].active = YES;

    UILabel *title = [UILabel new];
    title.text = name; title.font = [UIFont systemFontOfSize:34 weight:UIFontWeightBold]; title.textColor = UIColor.whiteColor;
    title.adjustsFontSizeToFitWidth = YES; title.minimumScaleFactor = 0.6;

    _picture = [[UISegmentedControl alloc] initWithItems:@[T(@"720p（掌机）", @"720p (handheld)"), T(@"1080p（底座）", @"1080p (docked)")]];
    _picture.selectedSegmentIndex = [d boolForKey:@"TOTK_DOCKED"] ? 1 : 0;
    _fps = [[UISegmentedControl alloc] initWithItems:@[@"30 fps", @"60 fps"]];
    _fps.selectedSegmentIndex = [[d stringForKey:@"TOTK_PHONE_FPS"] isEqualToString:@"30"] ? 0 : 1;
    for (UISegmentedControl *s in @[_picture, _fps]) {
        s.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
        [s.widthAnchor constraintEqualToConstant:340].active = YES;
        [s addTarget:self action:@selector(changed) forControlEvents:UIControlEventValueChanged];
    }
    UIStackView *pictureRow = [self row:T(@"画面", @"Picture") control:_picture];
    UIStackView *fpsRow = [self row:T(@"帧率", @"Frame rate") control:_fps];
    _hasPicture = [self settingsPaneHas:@"TOTK_DOCKED"];

    UILabel *note = [UILabel new];
#ifdef GAME_BOTW
    note.text = T(@"1080p：输出和画面都是 1920×1080，更清晰也更吃性能。设置也可以在“设置”App 里改。",
                  @"1080p: 1920×1080 output and scene, sharper and heavier. The Settings app has these choices too.");
#else   // Tears of the Kingdom's docked settings keep the scene at 1600x900 (main.m apply_display_settings)
    note.text = T(@"1080p：输出 1920×1080，画面 1600×900，更清晰也更吃性能。设置也可以在“设置”App 里改。",
                  @"1080p: 1920×1080 output with a 1600×900 scene, sharper and heavier. The Settings app has these choices too.");
#endif
    note.font = [UIFont systemFontOfSize:13]; note.textColor = [UIColor colorWithWhite:0.6 alpha:1];
    note.numberOfLines = 0; note.textAlignment = NSTextAlignmentCenter;
    [note.widthAnchor constraintLessThanOrEqualToConstant:520].active = YES;

    UIButtonConfiguration *cfg = [UIButtonConfiguration filledButtonConfiguration];
    cfg.title = T(@"开始游戏", @"Play"); cfg.image = [UIImage systemImageNamed:@"play.fill"]; cfg.imagePadding = 8;
    cfg.baseBackgroundColor = [UIColor colorWithRed:0.10 green:0.63 blue:1.0 alpha:1]; cfg.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    cfg.contentInsets = NSDirectionalEdgeInsetsMake(14, 40, 14, 40);
    cfg.titleTextAttributesTransformer = ^NSDictionary *(NSDictionary *a) {
        NSMutableDictionary *m = a.mutableCopy; m[NSFontAttributeName] = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold]; return m; };
    _start = [UIButton buttonWithConfiguration:cfg primaryAction:[UIAction actionWithHandler:^(UIAction *a) { (void)a; [self start]; }]];

    NSArray<UIView *> *rows = _hasPicture ? @[icon, title, pictureRow, fpsRow, note, _start] : @[icon, title, fpsRow, _start];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:rows];
    stack.axis = UILayoutConstraintAxisVertical; stack.alignment = UIStackViewAlignmentCenter; stack.spacing = 18;
    [stack setCustomSpacing:28 afterView:title]; [stack setCustomSpacing:26 afterView:note];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[[stack.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
                                              [stack.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
                                              [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor constant:24]]];
    [self focus];
    __weak TOTKStartView *weak = self;
    _poll = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; [weak pollController]; }];
    return self;
}

/* Whether the app's Settings pane offers a setting: the start screen shows only what the Settings app has. */
- (BOOL)settingsPaneHas:(NSString *)key {
    NSString *path = [NSBundle.mainBundle pathForResource:@"Root" ofType:@"plist" inDirectory:@"Settings.bundle"];
    for (NSDictionary *spec in [NSDictionary dictionaryWithContentsOfFile:path][@"PreferenceSpecifiers"] ?: @[])
        if ([spec[@"Key"] isEqualToString:key]) return YES;
    return NO;
}

- (void)layoutSubviews { [super layoutSubviews]; self.layer.sublayers.firstObject.frame = self.bounds; }

- (UIStackView *)row:(NSString *)label control:(UIView *)control {
    UILabel *l = [UILabel new];
    l.text = label; l.font = [UIFont systemFontOfSize:17 weight:UIFontWeightMedium]; l.textColor = [UIColor colorWithWhite:0.85 alpha:1];
    [l.widthAnchor constraintEqualToConstant:90].active = YES;
    UIStackView *s = [[UIStackView alloc] initWithArrangedSubviews:@[l, control]];
    s.spacing = 16; s.alignment = UIStackViewAlignmentCenter;
    return s;
}

/* The home screen icon the build put in the app (build_ipa.py: the game's own). */
- (UIImage *)appIcon {
    NSDictionary *icons = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleIcons"];
    NSString *file = [icons[@"CFBundlePrimaryIcon"][@"CFBundleIconFiles"] lastObject];
    UIImage *image = file ? [UIImage imageNamed:file] : nil;
    return image ?: [UIImage imageNamed:@"AppIcon"] ?: [UIImage systemImageNamed:@"gamecontroller.fill"];
}

- (void)changed {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setBool:_picture.selectedSegmentIndex == 1 forKey:@"TOTK_DOCKED"];
    [d setObject:_fps.selectedSegmentIndex == 0 ? @"30" : @"60" forKey:@"TOTK_PHONE_FPS"];
}

- (void)start {
    if (_done) return;
    _done = YES;
    [_poll invalidate]; _poll = nil;
    [self changed];
    void (^go)(void) = _onStart; _onStart = nil;
    [UIView animateWithDuration:0.25 animations:^{ self.alpha = 0; } completion:^(BOOL f) { (void)f; [self removeFromSuperview]; }];
    if (go) go();
}

/* The controller's focus: a ring around the row it moves. */
- (void)focus {
    NSArray<UIView *> *rows = @[_picture, _fps, _start];   // _row 0 is never focused without the picture row
    for (NSUInteger i = 0; i < rows.count; i++) {
        rows[i].layer.borderWidth = (NSInteger)i == _row ? 2 : 0;
        rows[i].layer.borderColor = [UIColor colorWithRed:0.10 green:0.63 blue:1.0 alpha:1].CGColor;
        rows[i].layer.cornerRadius = i == 2 ? 26 : 9;
    }
}

- (void)pollController {
    GCExtendedGamepad *g = GCController.current.extendedGamepad ?: GCController.controllers.firstObject.extendedGamepad;
    if (!g) return;
    float x = g.leftThumbstick.xAxis.value, y = g.leftThumbstick.yAxis.value;
    BOOL up = g.dpad.up.isPressed || y > 0.5, down = g.dpad.down.isPressed || y < -0.5;
    BOOL left = g.dpad.left.isPressed || x < -0.5, right = g.dpad.right.isPressed || x > 0.5;
    BOOL confirm = g.buttonA.isPressed || g.buttonMenu.isPressed;   // A or the menu button; B is back, never "Play" (after PR #1174)
    BOOL any = up || down || left || right || confirm;
    if (!any) { _held = NO; return; }
    if (_held) return;
    _held = YES;
    if (up) _row = MAX(_hasPicture ? 0 : 1, _row - 1);
    else if (down) _row = MIN(2, _row + 1);
    else if ((left || right) && _row < 2) {
        UISegmentedControl *s = _row == 0 ? _picture : _fps;
        s.selectedSegmentIndex = right ? 1 : 0;
        [self changed];
    } else if (confirm && _row == 2) { [self start]; return; }
    else if (confirm) _row = 2;
    [self focus];
}
@end
