#import "advanced_view.h"
#import <GameController/GameController.h>
#include "picture_settings.h"
#include "totk_ui_lang.h"

static NSString *T(NSString *z, NSString *e) { return totk_ui_zh() ? z : e; }
static const unsigned SCENE_WIDTHS[] = {0, 1920, 1600, 1280, 960, 800, 640};   /* 0: Automática */
static const unsigned SCENE_COUNT = sizeof SCENE_WIDTHS / sizeof *SCENE_WIDTHS;

@implementation TOTKAdvancedView {
    void (^_onBack)(void), (^_onStart)(void);
    pp_picture _pic;
    pp_metalfx _mfx;
    BOOL _pad;
    UILabel *_profileLabel;
    UISegmentedControl *_mode, *_fps, *_aspect;
    UIButton *_scene, *_metalfx, *_play;
    UISwitch *_taa, *_switchSize;
    NSMutableDictionary<NSValue *, UILabel *> *_effects;   // control -> the line under it
    NSArray<UIView *> *_controls;   // the controller's focus, top to bottom; Play last
    NSInteger _row;
    NSTimer *_poll;
    BOOL _held;
}

- (instancetype)initWithFrame:(CGRect)frame onBack:(void (^)(void))onBack onStart:(void (^)(void))onStart {
    if (!(self = [super initWithFrame:frame])) return nil;
    _onBack = [onBack copy]; _onStart = [onStart copy];
    _pad = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    _pic = totk_current_picture(NULL, &_mfx);
    _held = YES;   // the press that opened this screen is still down: wait for every button to be let go first
    _effects = [NSMutableDictionary dictionary];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundColor = [UIColor colorWithRed:0.06 green:0.07 blue:0.10 alpha:1];

    UIButtonConfiguration *bc = [UIButtonConfiguration plainButtonConfiguration];
    bc.title = T(@"‹ 返回", @"‹ Back"); bc.baseForegroundColor = [UIColor colorWithRed:0.45 green:0.75 blue:1.0 alpha:1];
    UIButton *back = [UIButton buttonWithConfiguration:bc primaryAction:[UIAction actionWithHandler:^(UIAction *a) { (void)a; [self back]; }]];
    UILabel *title = [UILabel new];
    title.text = T(@"进阶", @"Advanced"); title.font = [UIFont systemFontOfSize:22 weight:UIFontWeightBold]; title.textColor = UIColor.whiteColor;
    _profileLabel = [UILabel new];
    _profileLabel.font = [UIFont systemFontOfSize:14]; _profileLabel.textColor = [UIColor colorWithWhite:0.7 alpha:1];
    UIStackView *bar = [[UIStackView alloc] initWithArrangedSubviews:@[back, title, _profileLabel]];
    bar.distribution = UIStackViewDistributionEqualCentering; bar.alignment = UIStackViewAlignmentCenter;

    _mode = [self segments:@[T(@"掌机 720p", @"Handheld 720p"), T(@"底座 1080p", @"Docked 1080p")] action:@selector(modeChanged)];
    _scene = [self menuButton];
    _metalfx = [self menuButton];
    _taa = [UISwitch new]; [_taa addTarget:self action:@selector(taaChanged) forControlEvents:UIControlEventValueChanged];
    _fps = [self segments:@[@"30 fps", @"60 fps"] action:@selector(fpsChanged)];
    NSMutableArray<UIView *> *rows = [NSMutableArray arrayWithObject:bar];
    NSMutableArray<UIView *> *controls = [NSMutableArray array];
    [self add:T(@"模式", @"Mode") control:_mode rows:rows controls:controls];
    [self add:T(@"场景", @"Scene") control:_scene rows:rows controls:controls];
    [self add:@"MetalFX" control:_metalfx rows:rows controls:controls];
    [self add:T(@"边缘平滑", @"Edge smoothing") control:_taa rows:rows controls:controls];
    [self add:T(@"帧率", @"Frame rate") control:_fps rows:rows controls:controls];
    if (_pad) {   // an iPhone's screen is already smaller than the Switch's
        _switchSize = [UISwitch new]; [_switchSize addTarget:self action:@selector(switchSizeChanged) forControlEvents:UIControlEventValueChanged];
        [self add:T(@"Switch 屏幕大小", @"Switch screen size") control:_switchSize rows:rows controls:controls];
    }
#ifndef GAME_BOTW   // Breath of the Wild's aspect key means ultrawide (a separate change)
    _aspect = [self segments:@[T(@"填满", @"Fill"), @"16:9"] action:@selector(aspectChanged)];
    [self add:T(@"画面比例", @"Aspect ratio") control:_aspect rows:rows controls:controls];
#endif

    UIButtonConfiguration *pc = [UIButtonConfiguration filledButtonConfiguration];
    pc.title = T(@"开始游戏", @"Play"); pc.image = [UIImage systemImageNamed:@"play.fill"]; pc.imagePadding = 8;
    pc.baseBackgroundColor = [UIColor colorWithRed:0.10 green:0.63 blue:1.0 alpha:1]; pc.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    pc.contentInsets = NSDirectionalEdgeInsetsMake(14, 40, 14, 40);
    _play = [UIButton buttonWithConfiguration:pc primaryAction:[UIAction actionWithHandler:^(UIAction *a) { (void)a; [self play]; }]];
    [rows addObject:_play]; [controls addObject:_play];
    _controls = controls; _row = (NSInteger)controls.count - 1;

    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:rows];
    stack.axis = UILayoutConstraintAxisVertical; stack.alignment = UIStackViewAlignmentFill; stack.spacing = 14;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    UIScrollView *scroll = [UIScrollView new];   // an iPhone in landscape has room for a few rows only
    scroll.translatesAutoresizingMaskIntoConstraints = NO; scroll.alwaysBounceVertical = YES;
    [scroll addSubview:stack]; [self addSubview:scroll];
    UILayoutGuide *safe = self.safeAreaLayoutGuide, *content = scroll.contentLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:20], [stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
        [stack.centerXAnchor constraintEqualToAnchor:scroll.centerXAnchor],
        [stack.widthAnchor constraintEqualToConstant:MIN(560, frame.size.width - 48)]]];
    [self show];
    [self focus];
    __weak TOTKAdvancedView *weak = self;
    _poll = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; [weak pollController]; }];
    return self;
}

- (UISegmentedControl *)segments:(NSArray *)items action:(SEL)action {
    UISegmentedControl *s = [[UISegmentedControl alloc] initWithItems:items];
    s.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    [s addTarget:self action:action forControlEvents:UIControlEventValueChanged];
    return s;
}
- (UIButton *)menuButton {
    UIButtonConfiguration *c = [UIButtonConfiguration grayButtonConfiguration];
    c.image = [UIImage systemImageNamed:@"chevron.up.chevron.down"]; c.imagePlacement = NSDirectionalRectEdgeTrailing; c.imagePadding = 6;
    UIButton *b = [UIButton buttonWithConfiguration:c primaryAction:nil];
    b.showsMenuAsPrimaryAction = YES; b.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    return b;
}
/* A row: its label and control, then the line saying what the choice does. */
- (void)add:(NSString *)label control:(UIView *)control rows:(NSMutableArray *)rows controls:(NSMutableArray *)controls {
    UILabel *l = [UILabel new];
    l.text = label; l.font = [UIFont systemFontOfSize:17 weight:UIFontWeightMedium]; l.textColor = [UIColor colorWithWhite:0.9 alpha:1];
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[l, control]];
    row.distribution = UIStackViewDistributionEqualSpacing; row.alignment = UIStackViewAlignmentCenter;
    UILabel *effect = [UILabel new];
    effect.font = [UIFont systemFontOfSize:13]; effect.textColor = [UIColor colorWithWhite:0.6 alpha:1]; effect.numberOfLines = 0;
    _effects[[NSValue valueWithNonretainedObject:control]] = effect;
    [rows addObjectsFromArray:@[row, effect]]; [controls addObject:control];
}
- (void)effect:(UIView *)control text:(NSString *)text { _effects[[NSValue valueWithNonretainedObject:control]].text = text; }

static NSString *scene_title(unsigned w) {
    return w ? [NSString stringWithFormat:@"%u×%u", w, w * 9 / 16] : T(@"自动", @"Automatic");
}
static NSString *metalfx_title(pp_metalfx m) {
    return m == PP_MFX_OFF ? T(@"关", @"Off") : m == PP_MFX_SCENE ? T(@"场景", @"Scene") : T(@"场景＋屏幕", @"Scene + screen");
}

/* Every control and line from _pic / _mfx. */
- (void)show {
    pp_profile profile; totk_current_picture(&profile, NULL);
    NSString *names[] = {T(@"品质", @"Quality"), T(@"性能", @"Performance"), T(@"省电", @"Economy"), T(@"自定义", @"Custom")};
    _profileLabel.text = [NSString stringWithFormat:T(@"画面：%@", @"Profile: %@"), names[profile]];
    _mode.selectedSegmentIndex = _pic.mode == PP_DOCKED ? 1 : 0;
    _fps.selectedSegmentIndex = _pic.fps == 30 ? 0 : 1;
    _taa.on = _pic.taa != 0;
    _switchSize.on = _pic.switch_size != 0;
    _aspect.selectedSegmentIndex = _pic.aspect_169 ? 1 : 0;
    __weak TOTKAdvancedView *weak = self;
    NSMutableArray<UIAction *> *scenes = [NSMutableArray array];
    for (unsigned i = 0; i < SCENE_COUNT; i++) {
        unsigned w = SCENE_WIDTHS[i];
        UIAction *a = [UIAction actionWithTitle:scene_title(w) image:nil identifier:nil handler:^(UIAction *x) { (void)x; [weak setScene:w]; }];
        a.state = w == _pic.scene_w ? UIMenuElementStateOn : UIMenuElementStateOff;
        [scenes addObject:a];
    }
    _scene.menu = [UIMenu menuWithChildren:scenes];
    [self title:_scene text:scene_title(_pic.scene_w)];
    NSMutableArray<UIAction *> *mfx = [NSMutableArray array];
    for (int m = PP_MFX_OFF; m <= PP_MFX_SCREEN; m++) {
        UIAction *a = [UIAction actionWithTitle:metalfx_title((pp_metalfx)m) image:nil identifier:nil handler:^(UIAction *x) { (void)x; [weak setMetalfx:(pp_metalfx)m]; }];
        a.state = m == (int)_mfx ? UIMenuElementStateOn : UIMenuElementStateOff;
        [mfx addObject:a];
    }
    _metalfx.menu = [UIMenu menuWithChildren:mfx];
    [self title:_metalfx text:metalfx_title(_mfx)];

    unsigned w = pp_scene_effective(&_pic, PP_THIS_GAME), limit = pp_scene_limit(&_pic, PP_THIS_GAME);
    BOOL docked = _pic.mode == PP_DOCKED, small = _pad && _pic.switch_size;
    [self effect:_mode text:docked
        ? [T(@"游戏的底座设置，输出 1920×1080。", @"The game's docked settings, 1920×1080 output.") stringByAppendingString:small
           ? T(@"在 Switch 屏幕大小下画面会缩小，多出的像素白白耗费 GPU。", @" At the Switch's screen size the picture is scaled down: the extra pixels cost GPU for nothing.") : @""]
        : T(@"掌机设置，输出 1280×720。", @"Handheld settings, 1280×720 output.")];
    [self effect:_scene text:_pic.scene_w > w
        ? [NSString stringWithFormat:T(@"掌机模式最高 %u×%u：场景用 %u×%u。", @"Handheld goes up to %u×%u: the scene runs at %u×%u."), limit, limit * 9 / 16, w, w * 9 / 16]
        : [NSString stringWithFormat:T(@"场景 %u×%u。只影响 3D 世界：越低越凉、越省电，画面也越软。", @"Scene %u×%u. The 3D world only: lower runs cooler and saves battery, with a softer picture."), w, w * 9 / 16]];
    NSString *mfxText = _mfx == PP_MFX_OFF ? T(@"游戏自己的放大。", @"The game's own upscale.")
        : !pp_mfx_scene_acts(&_pic, PP_THIS_GAME) ? T(@"场景不比画布小：MetalFX 没有场景要放大。", @"The scene is not smaller than the canvas: MetalFX has no scene to upscale.")
        : T(@"MetalFX 把场景放大到画布：更清晰，几乎不耗性能。", @"MetalFX upscales the scene into the canvas: sharper, at almost no cost.");
    if (_mfx == PP_MFX_SCREEN) mfxText = [mfxText stringByAppendingString:small
        ? T(@"再把画布放大到 Switch 大小的画面：最清晰，最耗 GPU。", @" It also enlarges the canvas to the Switch-sized picture: the sharpest, the heaviest on the GPU.")
        : T(@"再把画布放大到屏幕：最清晰，最耗 GPU。", @" It also enlarges the canvas to the screen: the sharpest, the heaviest on the GPU.")];
    [self effect:_metalfx text:[mfxText stringByAppendingString:T(@"（独立于画面设置）", @" (a setting of its own, kept by every profile)")]];
    [self effect:_taa text:T(@"MetalFX 时间抗锯齿：边缘更稳，可能有轻微拖影。场景越大越吃 GPU：960×540 约 1 ms，1600×900 约 11 ms（iPad A16）。", @"MetalFX temporal anti-aliasing: steadier edges, slight ghosting possible. Its GPU cost grows with the scene: about 1 ms at 960×540, about 11 ms at 1600×900 (iPad A16).")];
    [self effect:_fps text:_pic.fps == 30 ? T(@"30 fps：更凉、更省电。", @"30 fps: cooler, easier on the battery.")
                                          : T(@"60 fps：不固定上限，最高到屏幕的刷新率。", @"60 fps: no fixed cap, up to the screen's rate.")];
    if (_switchSize) [self effect:_switchSize text:T(@"画面显示成 Switch 屏幕的大小（6.2 英寸），居中、四周黑边：更小、更清晰。",
                                                     @"The picture at the Switch's screen size (6.2 inches), centred on black: smaller and sharper.")];
    if (_aspect) [self effect:_aspect text:_pic.aspect_169 ? T(@"16:9，上下或左右加黑边。", @"16:9 with bars.")
                                                           : T(@"填满屏幕，不拉伸：镜头按屏幕比例取景。", @"Fills the screen without stretching: the camera takes the screen's shape.")];
}
- (void)title:(UIButton *)b text:(NSString *)text { UIButtonConfiguration *c = b.configuration; c.title = text; b.configuration = c; }

/* Any control but MetalFX: the combination becomes Custom. */
- (void)changed { totk_save_custom_picture(&_pic); [self show]; }
- (void)modeChanged { _pic.mode = _mode.selectedSegmentIndex == 1 ? PP_DOCKED : PP_HANDHELD; [self changed]; }
- (void)setScene:(unsigned)w { _pic.scene_w = w; [self changed]; }
- (void)taaChanged { _pic.taa = _taa.on; [self changed]; }
- (void)fpsChanged { _pic.fps = _fps.selectedSegmentIndex == 0 ? 30 : 60; [self changed]; }
- (void)switchSizeChanged { _pic.switch_size = _switchSize.on; [self changed]; }
- (void)aspectChanged { _pic.aspect_169 = _aspect.selectedSegmentIndex == 1; [self changed]; }
- (void)setMetalfx:(pp_metalfx)m {
    _mfx = m;
    totk_choose_metalfx(m);   // its own setting: the profile stays
    [self show];
}

- (void)finish { [_poll invalidate]; _poll = nil; }
- (void)back { [self finish]; void (^go)(void) = _onBack; _onBack = nil; if (go) go(); }
- (void)play { [self finish]; void (^go)(void) = _onStart; _onStart = nil; if (go) go(); }

- (void)focus {
    for (NSUInteger i = 0; i < _controls.count; i++) {
        _controls[i].layer.borderWidth = (NSInteger)i == _row ? 2 : 0;
        _controls[i].layer.borderColor = [UIColor colorWithRed:0.10 green:0.63 blue:1.0 alpha:1].CGColor;
        _controls[i].layer.cornerRadius = _controls[i] == _play ? 26 : 9;
    }
}

/* Left / right: a segmented control's neighbour, a menu's next value, a switch on or off. */
- (void)step:(UIView *)at by:(int)d {
    if (at == _scene) {
        unsigned i = 0; while (i < SCENE_COUNT && SCENE_WIDTHS[i] != _pic.scene_w) i++;
        int n = (int)(i % SCENE_COUNT) + d; n = n < 0 ? 0 : n >= (int)SCENE_COUNT ? (int)SCENE_COUNT - 1 : n;
        [self setScene:SCENE_WIDTHS[n]];
    } else if (at == _metalfx) {
        int n = (int)_mfx + d; n = n < PP_MFX_OFF ? PP_MFX_OFF : n > PP_MFX_SCREEN ? PP_MFX_SCREEN : n;
        [self setMetalfx:(pp_metalfx)n];
    } else if ([at isKindOfClass:UISwitch.class]) {
        UISwitch *s = (UISwitch *)at; s.on = d > 0;
        if (s == _taa) [self taaChanged]; else [self switchSizeChanged];
    } else if ([at isKindOfClass:UISegmentedControl.class]) {
        UISegmentedControl *s = (UISegmentedControl *)at;
        s.selectedSegmentIndex = MAX(0, MIN((NSInteger)s.numberOfSegments - 1, s.selectedSegmentIndex + d));
        [s sendActionsForControlEvents:UIControlEventValueChanged];
    }
}

- (void)pollController {
    GCExtendedGamepad *g = GCController.current.extendedGamepad ?: GCController.controllers.firstObject.extendedGamepad;
    if (!g) return;
    float x = g.leftThumbstick.xAxis.value, y = g.leftThumbstick.yAxis.value;
    BOOL up = g.dpad.up.isPressed || y > 0.5, down = g.dpad.down.isPressed || y < -0.5;
    BOOL left = g.dpad.left.isPressed || x < -0.5, right = g.dpad.right.isPressed || x > 0.5;
    BOOL confirm = g.buttonA.isPressed || g.buttonMenu.isPressed, cancel = g.buttonB.isPressed;
    if (!(up || down || left || right || confirm || cancel)) { _held = NO; return; }
    if (_held) return;
    _held = YES;
    NSInteger last = (NSInteger)_controls.count - 1;
    if (cancel) { [self back]; return; }
    if (up) _row = MAX(0, _row - 1);
    else if (down) _row = MIN(last, _row + 1);
    else if ((left || right) && _row < last) [self step:_controls[_row] by:right ? 1 : -1];
    else if (confirm && _row == last) { [self play]; return; }
    else if (confirm) _row = last;
    [self focus];
}
@end
