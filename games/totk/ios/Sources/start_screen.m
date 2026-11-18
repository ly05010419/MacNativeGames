#import "start_screen.h"
#import <GameController/GameController.h>
#include "totk_ui_lang.h"
#include "ns2_resolution.h"
#include "picture_settings.h"
#import "advanced_view.h"

static BOOL zh(void) { return totk_ui_zh(); }   // the language of the host's other text (boot screen, overlays; TOTK_UI_LANG overrides)
static NSString *T(NSString *z, NSString *e) { return zh() ? z : e; }

@implementation TOTKStartView {
    void (^_onStart)(void);
    UISegmentedControl *_picture, *_fps;
    UISegmentedControl *_profile, *_metalfx;   // Tears of the Kingdom / Breath of the Wild: the picture profile and MetalFX
    UIButton *_start, *_advanced;
    UILabel *_profileNote;   // what the chosen profile does
    TOTKAdvancedView *_advancedView;   // open over this screen; this screen's controller polling waits meanwhile
    pp_metalfx _lastMfxOn;   // what "On" saves: the last On value (Scene, or Scene + screen from Advanced)
    BOOL _hasProfiles;       // the pane has the picture profile (the two Zelda games)
    NSArray<UIView *> *_controls;   // what the controller's focus moves through, top to bottom; the Play button last
    NSInteger _row;          // index into _controls
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
    /* Tears of the Kingdom and Breath of the Wild (user 2026-11-17): the picture
       profiles, a line on what the chosen one does, MetalFX on its own row (it never changes the profile), Advanced. */
    _hasProfiles = [self settingsPaneHas:@"TOTK_PICTURE_PROFILE"];
    UIStackView *profileRow = nil, *metalfxRow = nil;
    UILabel *mfxNote = nil;
    if (_hasProfiles) {
        pp_profile profile; pp_metalfx mfx;
        totk_current_picture(&profile, &mfx);
        NSMutableArray *items = [@[T(@"品质", @"Quality"), T(@"性能", @"Performance"), T(@"省电", @"Economy")] mutableCopy];
        if (totk_has_custom_picture() || profile == PP_CUSTOM) [items addObject:T(@"自定义", @"Custom")];
        _profile = [[UISegmentedControl alloc] initWithItems:items];
        _profile.selectedSegmentIndex = profile == PP_CUSTOM && items.count < 4 ? PP_PERFORMANCE : (NSInteger)profile;
        _metalfx = [[UISegmentedControl alloc] initWithItems:@[T(@"关", @"Off"), mfx == PP_MFX_SCREEN ? T(@"开（到屏幕）", @"On (to the screen)") : T(@"开", @"On")]];
        _metalfx.selectedSegmentIndex = mfx == PP_MFX_OFF ? 0 : 1;
        _lastMfxOn = mfx == PP_MFX_SCREEN ? PP_MFX_SCREEN : PP_MFX_SCENE;
        for (UISegmentedControl *c in @[_profile, _metalfx]) {
            c.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
            [c.widthAnchor constraintEqualToConstant:c == _profile ? 460 : 340].active = YES;
        }
        [_profile addTarget:self action:@selector(profileChanged) forControlEvents:UIControlEventValueChanged];
        [_metalfx addTarget:self action:@selector(metalfxChanged) forControlEvents:UIControlEventValueChanged];
        profileRow = [self row:T(@"画面", @"Picture") control:_profile];
        metalfxRow = [self row:@"MetalFX" control:_metalfx];
        _profileNote = [UILabel new];
        mfxNote = [UILabel new];
        mfxNote.text = T(@"MetalFX 放大场景：更清晰，几乎不耗性能。", @"MetalFX upscales the scene: sharper, at almost no cost.");
        for (UILabel *l in @[_profileNote, mfxNote]) {
            l.font = [UIFont systemFontOfSize:13]; l.textColor = [UIColor colorWithWhite:0.6 alpha:1];
            l.numberOfLines = 0; l.textAlignment = NSTextAlignmentCenter;
            [l.widthAnchor constraintLessThanOrEqualToConstant:520].active = YES;
        }
        UIButtonConfiguration *ac = [UIButtonConfiguration plainButtonConfiguration];
        ac.title = T(@"进阶 ›", @"Advanced ›"); ac.baseForegroundColor = [UIColor colorWithRed:0.45 green:0.75 blue:1.0 alpha:1];
        _advanced = [UIButton buttonWithConfiguration:ac primaryAction:nil];
        [_advanced addTarget:self action:@selector(openAdvanced) forControlEvents:UIControlEventTouchUpInside];
        [self showProfile];
    }

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
    if (_hasProfiles) [rows addObjectsFromArray:@[profileRow, _profileNote, metalfxRow, mfxNote, _advanced, _start]];
    else [rows addObjectsFromArray:_hasPicture ? @[pictureRow, fpsRow, note, _start] : @[fpsRow, _start]];
    _controls = _hasProfiles ? @[_profile, _metalfx, _advanced, _start] : _hasPicture ? @[_picture, _fps, _start] : @[_fps, _start];
    _row = (NSInteger)_controls.count - 1;   // the focus starts on Play
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:rows];
    stack.axis = UILayoutConstraintAxisVertical; stack.alignment = UIStackViewAlignmentCenter; stack.spacing = 18;
    [stack setCustomSpacing:28 afterView:title];
    if ([rows containsObject:note]) [stack setCustomSpacing:26 afterView:note];
    if (_hasProfiles) [stack setCustomSpacing:26 afterView:mfxNote];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    /* In a scroll view: the profile rows are taller than an iPhone in landscape (final review 2026-11-18). Centred while
       they fit, scrolled when they do not. */
    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack]; [self addSubview:scroll];
    UILayoutGuide *content = scroll.contentLayoutGuide, *visible = scroll.frameLayoutGuide, *safe = self.safeAreaLayoutGuide;
    NSLayoutConstraint *fill = [content.heightAnchor constraintEqualToAnchor:visible.heightAnchor]; fill.priority = UILayoutPriorityDefaultLow;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [content.widthAnchor constraintEqualToAnchor:visible.widthAnchor],
        [content.heightAnchor constraintGreaterThanOrEqualToAnchor:visible.heightAnchor], fill,
        [stack.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:content.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:content.topAnchor constant:16],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:content.bottomAnchor constant:-16],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:24]]];
    [self focus];
    [self startPolling];
    return self;
}

- (void)startPolling {
    __weak TOTKStartView *weak = self;
    _poll = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; [weak pollController]; }];
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

/* The chosen profile, and what it does (user 2026-11-17); Custom says what its combination is. */
- (pp_profile)chosenProfile { return (pp_profile)_profile.selectedSegmentIndex; }
- (void)profileChanged { totk_choose_profile([self chosenProfile]); [self showProfile]; }
- (void)metalfxChanged {
    totk_choose_metalfx(_metalfx.selectedSegmentIndex == 0 ? PP_MFX_OFF : _lastMfxOn);
    [_metalfx setTitle:_lastMfxOn == PP_MFX_SCREEN ? T(@"开（到屏幕）", @"On (to the screen)") : T(@"开", @"On") forSegmentAtIndex:1];
}
- (void)openAdvanced {
    if (_advancedView || _done) return;
    [_poll invalidate]; _poll = nil;
    __weak TOTKStartView *weak = self;
    _advancedView = [[TOTKAdvancedView alloc] initWithFrame:self.bounds onBack:^{ [weak closeAdvanced]; }
                                                    onStart:^{ [weak closeAdvanced]; [weak start]; }];
    [self addSubview:_advancedView];
}
/* Back from Advanced: the profile may now be Custom and MetalFX another value. */
- (void)closeAdvanced {
    [_advancedView removeFromSuperview]; _advancedView = nil;
    pp_profile profile; pp_metalfx mfx;
    totk_current_picture(&profile, &mfx);
    if (profile == PP_CUSTOM && _profile.numberOfSegments < 4) [_profile insertSegmentWithTitle:T(@"自定义", @"Custom") atIndex:3 animated:NO];
    _profile.selectedSegmentIndex = (NSInteger)profile;
    if (mfx != PP_MFX_OFF) _lastMfxOn = mfx;
    [_metalfx setTitle:_lastMfxOn == PP_MFX_SCREEN ? T(@"开（到屏幕）", @"On (to the screen)") : T(@"开", @"On") forSegmentAtIndex:1];
    _metalfx.selectedSegmentIndex = mfx == PP_MFX_OFF ? 0 : 1;
    [self showProfile];
    _held = YES;   // the button that closed Advanced is still down
    if (!_done) [self startPolling];
}
- (void)showProfile {
    BOOL pad = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    pp_picture pic = totk_current_picture(NULL, NULL);
    switch ([self chosenProfile]) {
    case PP_QUALITY:
        _profileNote.text = T(@"1080p，场景 1600×900，边缘平滑，30 fps。最清晰；会发热。",
                              @"1080p with a 1600×900 scene, smooth edges, 30 fps. The sharpest; runs warm."); break;
    case PP_PERFORMANCE:
        _profileNote.text = T(@"720p，场景 960×540，60 fps。流畅、均衡。", @"720p with a 960×540 scene, 60 fps. Smooth and balanced."); break;
    case PP_ECONOMY:
        _profileNote.text = pad ? T(@"720p，场景 640×360，30 fps，Switch 屏幕大小。发热最少，最省电。",
                                    @"720p with a 640×360 scene, 30 fps, at the Switch's screen size. The coolest, easiest on the battery.")
                                : T(@"720p，场景 640×360，30 fps。发热最少，最省电。",
                                    @"720p with a 640×360 scene, 30 fps. The coolest, easiest on the battery."); break;
    default: {
        unsigned w = pp_scene_effective(&pic, PP_THIS_GAME);
        NSMutableArray<NSString *> *parts = [NSMutableArray arrayWithObjects:
            pic.mode == PP_DOCKED ? T(@"1080p 底座", @"1080p docked") : @"720p",
            [NSString stringWithFormat:T(@"场景 %u×%u%@", @"%u×%u scene%@"), w, w * 9 / 16,
             pic.scene_w > w ? T(@"（掌机上限）", @" (the handheld limit)") : @""],
            [NSString stringWithFormat:@"%d fps", pic.fps], nil];
        if (pic.taa) [parts addObject:T(@"边缘平滑", @"smooth edges")];
        if (pic.switch_size && pad) [parts addObject:T(@"Switch 屏幕大小", @"Switch screen size")];
        _profileNote.text = [NSString stringWithFormat:T(@"自定义：%@。在“进阶”里修改。", @"Custom: %@. Change it under Advanced."),
                             [parts componentsJoinedByString:@" · "]];
    } }
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
    if (!_hasProfiles) [self changed];   // the profile and MetalFX are saved as they change
    void (^go)(void) = _onStart; _onStart = nil;
    [UIView animateWithDuration:0.25 animations:^{ self.alpha = 0; } completion:^(BOOL f) { (void)f; [self removeFromSuperview]; }];
    if (go) go();
}

/* The controller's focus: a ring around the row it moves. */
- (void)focus {
    for (NSUInteger i = 0; i < _controls.count; i++) {
        _controls[i].layer.borderWidth = (NSInteger)i == _row ? 2 : 0;
        _controls[i].layer.borderColor = [UIColor colorWithRed:0.10 green:0.63 blue:1.0 alpha:1].CGColor;
        _controls[i].layer.cornerRadius = _controls[i] == _start ? 26 : 9;
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
    NSInteger last = (NSInteger)_controls.count - 1;   // the Play button
    UIView *at = _controls[_row];
    if (up) _row = MAX(0, _row - 1);
    else if (down) _row = MIN(last, _row + 1);
    else if ((left || right) && [at isKindOfClass:UISegmentedControl.class]) {
        UISegmentedControl *s = (UISegmentedControl *)at;
        s.selectedSegmentIndex = MAX(0, MIN((NSInteger)s.numberOfSegments - 1, s.selectedSegmentIndex + (right ? 1 : -1)));
        if (s == _profile) [self profileChanged];
        else if (s == _metalfx) [self metalfxChanged];
        else if (_hasResolution && s == _picture) [self resolutionChanged:s];
        else [self changed];
    } else if (confirm && at == _advanced) { [self openAdvanced]; return; }
    else if (confirm && _row == last) { [self start]; return; }
    else if (confirm) _row = last;
    [self focus];
}
@end
