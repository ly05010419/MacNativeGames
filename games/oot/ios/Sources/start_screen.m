#import "start_screen.h"
#import <GameController/GameController.h>
#include "totk_ui_lang.h"
#include "ns2_resolution.h"

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
    BOOL _hasScene;          // and a docked scene row: 1080p comes as original and HD
    BOOL _hasResolution;     // the game's own width x height parameter
    NSString *_sceneKey;     // that row's key: TOTK_BOTW_SCENE (Breath of the Wild) or TOTK_DOCKED_SCENE (Tears of the Kingdom)
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

    /* The pane's docked scene row (Breath of the Wild: TOTK_BOTW_SCENE, build_ipa.py botw_pane; Tears of the Kingdom:
       TOTK_DOCKED_SCENE, settings_map.c totk_docked_scene): 1080p original is the Switch's 1600x900 scene upscaled to
       1920x1080, 1080p HD the scene at 1920x1080. */
    _sceneKey = [self settingsPaneHas:@"TOTK_BOTW_SCENE"] ? @"TOTK_BOTW_SCENE" : [self settingsPaneHas:@"TOTK_DOCKED_SCENE"] ? @"TOTK_DOCKED_SCENE" : nil;
    _hasScene = _sceneKey != nil;
    _hasResolution = [self settingsPaneHas:@"TOTK_OUTPUT_RES"];
    _picture = _hasScene
        ? [[UISegmentedControl alloc] initWithItems:@[T(@"720p（掌机）", @"720p (handheld)"), T(@"1080p 原始", @"1080p original"), T(@"1080p 高清", @"1080p HD")]]
        : [[UISegmentedControl alloc] initWithItems:@[T(@"720p（掌机）", @"720p (handheld)"), T(@"1080p（底座）", @"1080p (docked)")]];
    _picture.selectedSegmentIndex = ![d boolForKey:@"TOTK_DOCKED"] ? 0 : _hasScene && [[d stringForKey:_sceneKey] isEqualToString:@"1920x1080"] ? 2 : 1;
    if (_hasResolution) {
        _picture = [[UISegmentedControl alloc] initWithItems:@[@"540p", @"720p", @"1080p", T(@"自定义…", @"Custom…")]];
        NSString *size=[d stringForKey:@"TOTK_OUTPUT_RES"] ?: @"960x540";
        _picture.selectedSegmentIndex=[size isEqualToString:@"960x540"]?0:[size isEqualToString:@"1280x720"]?1:[size isEqualToString:@"1920x1080"]?2:3;
    }
    _fps = [[UISegmentedControl alloc] initWithItems:@[@"30 fps", @"60 fps"]];
    _fps.selectedSegmentIndex = [[d stringForKey:@"TOTK_PHONE_FPS"] isEqualToString:@"30"] ? 0 : 1;
    for (UISegmentedControl *s in @[_picture, _fps]) {
        s.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
        [s.widthAnchor constraintEqualToConstant:(_hasScene || _hasResolution) && s == _picture ? 420 : 340].active = YES;   // picture presets need the room
        [s addTarget:self action:s==_picture && _hasResolution ? @selector(resolutionChanged:) : @selector(changed) forControlEvents:UIControlEventValueChanged];
    }
    UIStackView *pictureRow = [self row:T(@"画面", @"Picture") control:_picture];
    UIStackView *fpsRow = [self row:T(@"帧率", @"Frame rate") control:_fps];
    _hasPicture = _hasResolution || [self settingsPaneHas:@"TOTK_DOCKED"];

    UILabel *note = [UILabel new];
    if (_hasResolution)
        note.text = T(@"默认 540p（960×540）。自定义宽×高可在这里或“设置”App 中修改。保留原始贴图及完整 mip。",
                      @"540p (960×540) by default. Enter a custom width × height here or in Settings. Original textures and full mip chains are preserved.");
    else if (_hasScene)
        note.text = T(@"1080p 原始：输出 1920×1080，画面 1600×900，和 Switch 底座一样。1080p 高清：画面也是 1920×1080，更清晰也更吃性能。设置也可以在“设置”App 里改。",
                      @"1080p original: 1920×1080 output with a 1600×900 scene, as the Switch docked. 1080p HD: the scene at 1920×1080 too, sharper and heavier. The Settings app has these choices too.");
    else   // a pane without the scene row keeps the docked scene at 1600x900 (main.m apply_display_settings)
        note.text = T(@"1080p：输出 1920×1080，画面 1600×900，和 Switch 底座一样，更清晰也更吃性能。设置也可以在“设置”App 里改。",
                      @"1080p: 1920×1080 output with a 1600×900 scene, as the Switch docked; sharper and heavier. The Settings app has these choices too.");
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

    /* The icon only on an iPad: on an iPhone in landscape the rows did not fit with it (user 2026-11-01). */
    NSMutableArray<UIView *> *rows = [NSMutableArray arrayWithObject:title];
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) [rows insertObject:icon atIndex:0];
    [rows addObjectsFromArray:_hasPicture ? @[pictureRow, fpsRow, note, _start] : @[fpsRow, _start]];
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
    if (_hasResolution) {
        if(_picture.selectedSegmentIndex>=0 && _picture.selectedSegmentIndex<3) [d setObject:@[@"960x540",@"1280x720",@"1920x1080"][_picture.selectedSegmentIndex] forKey:@"TOTK_OUTPUT_RES"];
    } else [d setBool:_picture.selectedSegmentIndex >= 1 forKey:@"TOTK_DOCKED"];
    if (_hasScene && _picture.selectedSegmentIndex >= 1)   // 720p keeps the docked scene the player chose last
        [d setObject:_picture.selectedSegmentIndex == 2 ? @"1920x1080" : @"1600x900" forKey:_sceneKey];
    [d setObject:_fps.selectedSegmentIndex == 0 ? @"30" : @"60" forKey:@"TOTK_PHONE_FPS"];
}

- (void)resolutionChanged:(UISegmentedControl *)sender {
    if(sender.selectedSegmentIndex!=3) { [self changed]; return; }
    NSUserDefaults *d=NSUserDefaults.standardUserDefaults;
    UIAlertController *a=[UIAlertController alertControllerWithTitle:T(@"自定义分辨率",@"Custom resolution")
        message:T(@"输入宽x高，例如 960x540 或 1280x720。",@"Enter widthxheight, such as 960x540 or 1280x720.") preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField *t){
        t.text=[d stringForKey:@"TOTK_OUTPUT_RES"] ?: @"960x540";
        t.autocapitalizationType=UITextAutocapitalizationTypeNone;t.autocorrectionType=UITextAutocorrectionTypeNo;t.keyboardType=UIKeyboardTypeASCIICapable;
        t.accessibilityLabel=T(@"渲染分辨率",@"Render resolution");
    }];
    [a addAction:[UIAlertAction actionWithTitle:T(@"取消",@"Cancel") style:UIAlertActionStyleCancel handler:^(UIAlertAction *action){
        (void)action;NSString *s=[d stringForKey:@"TOTK_OUTPUT_RES"];
        sender.selectedSegmentIndex=[s isEqualToString:@"960x540"]?0:[s isEqualToString:@"1280x720"]?1:[s isEqualToString:@"1920x1080"]?2:3;
    }]];
    [a addAction:[UIAlertAction actionWithTitle:T(@"使用",@"Use") style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
        (void)action;unsigned w,h;NSString *s=[a.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if(!ns2_parse_resolution(s.UTF8String,&w,&h)) {
            a.textFields.firstObject.text=@"960x540";
            sender.selectedSegmentIndex=0;[self changed];
            UIAlertController *error=[UIAlertController alertControllerWithTitle:T(@"分辨率格式无效",@"Invalid resolution")
                message:T(@"请输入宽x高，每边 1–16384，例如 1280x720。已恢复 540p。",@"Use widthxheight, each 1–16384, such as 1280x720. Restored 540p.") preferredStyle:UIAlertControllerStyleAlert];
            [error addAction:[UIAlertAction actionWithTitle:T(@"好",@"OK") style:UIAlertActionStyleDefault handler:nil]];
            [self.window.rootViewController presentViewController:error animated:YES completion:nil];return;
        }
        [d setObject:[NSString stringWithFormat:@"%ux%u",w,h] forKey:@"TOTK_OUTPUT_RES"];
        sender.selectedSegmentIndex=(w==960&&h==540)?0:(w==1280&&h==720)?1:(w==1920&&h==1080)?2:3;
    }]];
    [self.window.rootViewController presentViewController:a animated:YES completion:nil];
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
        s.selectedSegmentIndex = MAX(0, MIN((NSInteger)s.numberOfSegments - 1, s.selectedSegmentIndex + (right ? 1 : -1)));
        if(_hasResolution && s==_picture) [self resolutionChanged:s]; else [self changed];
    } else if (confirm && _row == 2) { [self start]; return; }
    else if (confirm) _row = 2;
    [self focus];
}
@end
