#import "touch_pad.h"
#import <GameController/GameController.h>
#include <math.h>
#include "runtime.h"

/* Layout inside the safe area (clear of the iPhone's notch, rounded corners and home indicator), like a Switch
   controller: ZL/L and R/ZR along the top with − and + between them, and two columns mirroring each other: on the left
   the d-pad above the left stick, on the right A/B/X/Y above the camera stick. Each stick is drawn at its resting place
   until a finger takes it; the sticks sit a little toward the middle. L3 / R3 (stick press) are in the bottom corners,
   under ZL / ZR.
   A touch that misses every button is a stick, centred where the finger went down: the left stick on the left half,
   the camera (right stick) on the right half. Bits are the Npad buttons. While a game controller is connected the layer
   is hidden; a touch shows it and works as usual, and it hides again 3 seconds after the last finger lifts. */
typedef struct { CGPoint at; uint64_t bit; NSString *label; CGFloat size; } TouchButton;
enum { kButtonCount = 16 };
static const CGFloat kButtonSize = 46;
static const NSTimeInterval kHideAfter = 3;   // seconds after the last finger lifts, with a controller connected

@implementation TOTKTouchPad {
    NSMapTable<UITouch *, NSValue *> *points;
    NSMapTable<UITouch *, NSValue *> *leftOrigins;    // where each left-stick touch went down
    NSMapTable<UITouch *, NSValue *> *rightOrigins;   // the same for the camera stick
    BOOL gamepad, concealed;                          // a controller is connected; the layer is hidden for it
    NSTimer *hideTimer;
    UIButton *gearButton;
    UIView *settingsOverlay;
    UIScrollView *settingsCard;
    UIScrollView *mappingCard;
    UIButton *mappingButton;
    UIButton *keyboardButton;
    BOOL keyboardPage;
    int keyboardEditSlot;
    int controllerEditSlot;
    UISegmentedControl *fpsSeg;
    UISegmentedControl *padSeg;
    UISegmentedControl *hudSeg;
    UILabel *padStatusLabel;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor; self.contentMode = UIViewContentModeRedraw;
        points = [NSMapTable strongToStrongObjectsMapTable];
        leftOrigins = [NSMapTable strongToStrongObjectsMapTable];
        rightOrigins = [NSMapTable strongToStrongObjectsMapTable];
        [self setupSettingsUI];
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidConnectNotification object:nil];
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidDisconnectNotification object:nil];
        [self controllersChanged:nil];
        [self updateState];
    }
    return self;
}

- (void)setupSettingsUI {
    gearButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [gearButton setTitle:@"⚙" forState:UIControlStateNormal];
    gearButton.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
    [gearButton setTitleColor:[UIColor colorWithWhite:1.0 alpha:0.85] forState:UIControlStateNormal];
    gearButton.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.45];
    gearButton.layer.cornerRadius = 19;
    gearButton.layer.borderWidth = 1.0;
    gearButton.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.28].CGColor;
    [gearButton addTarget:self action:@selector(toggleSettingsPanel) forControlEvents:UIControlEventTouchUpInside];
    [self addSubview:gearButton];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect a = self.area;
    gearButton.frame = CGRectMake(CGRectGetMidX(a) - 19, CGRectGetMinY(a) + a.size.height * .12 - 19, 38, 38);
    if (settingsOverlay) {
        settingsOverlay.frame = self.bounds;
        CGFloat cw = MIN(460, self.bounds.size.width - 32);
        CGFloat ch = MIN(290, self.area.size.height - 20);
        settingsCard.frame = CGRectMake((self.bounds.size.width - cw) / 2.0, (self.bounds.size.height - ch) / 2.0, cw, ch);
        CGFloat segX = 104, segW = MAX(1, cw - segX - 18);
        UIView *closeBtn = [settingsCard viewWithTag:1001];
        if (closeBtn) closeBtn.frame = CGRectMake(cw - 48, 8, 40, 30);
        CGFloat y = 46;
        UIView *fpsLbl = [settingsCard viewWithTag:1002];
        if (fpsLbl) fpsLbl.frame = CGRectMake(18, y, 82, 30);
        if (fpsSeg) fpsSeg.frame = CGRectMake(segX, y, segW, 30);
        y += 42;
        UIView *padLbl = [settingsCard viewWithTag:1003];
        if (padLbl) padLbl.frame = CGRectMake(18, y, 82, 30);
        if (padSeg) padSeg.frame = CGRectMake(segX, y, segW, 30);
        y += 32;
        if (padStatusLabel) padStatusLabel.frame = CGRectMake(segX, y, segW, 18);
        y += 28;
        UIView *hudLbl = [settingsCard viewWithTag:1005];
        if (hudLbl) hudLbl.frame = CGRectMake(18, y, 82, 30);
        if (hudSeg) hudSeg.frame = CGRectMake(segX, y, segW, 30);
        mappingButton.frame = CGRectMake(18, y + 38, cw - 36, 36);
        keyboardButton.frame = CGRectMake(18, y + 78, cw - 36, 36);
        settingsCard.contentSize = CGSizeMake(cw, y + 126);
        if (mappingCard && !CGRectEqualToRect(mappingCard.frame, settingsCard.frame)) {
            CGPoint offset = mappingCard.contentOffset;
            if (keyboardPage) [self openKeyboardMapping]; else [self openMapping]; mappingCard.contentOffset = offset;
        }
    }
}

- (void)refreshSettingsValues {
    if (!settingsCard) return;
    int hz = runtime_fps_cap_now();
    fpsSeg.selectedSegmentIndex = (hz == 30) ? 0 : (hz == 90) ? 2 : (hz == 120) ? 3 : 1;
    int detected = -1;
    int ptype = runtime_gamepad_type(&detected);
    if (ptype >= 0 && ptype <= 4) padSeg.selectedSegmentIndex = ptype;
    static NSString * const names[] = {@"自动", @"Xbox", @"PS", @"Switch", @"第三方Pro"};
    NSString *detName = (detected >= 1 && detected <= 4) ? names[detected] : @"未检测到手柄";
    NSString *curMode = (ptype == 0) ? [NSString stringWithFormat:@"自动 (%@)", detName] : names[ptype];
    GCController *first = GCController.controllers.firstObject;
    NSString *vendor = first.vendorName ?: @"无外接手柄";
    padStatusLabel.text = [NSString stringWithFormat:@"当前手柄: %@  |  当前模式: %@", vendor, curMode];
    if (hudSeg) {
        int hmode = fps_overlay_hud_mode();
        hudSeg.selectedSegmentIndex = (hmode == 0) ? 2 : (hmode == 2) ? 1 : 0;
    }
}

- (void)toggleSettingsPanel {
    if (settingsOverlay && !settingsOverlay.hidden) {
        [self closeMapping];
        settingsOverlay.hidden = YES;
        runtime_input_settings_suspend(0);
        if (runtime_input_source(0) == INPUT_SOURCE_TOUCH) { concealed = NO; [self setNeedsDisplay]; }
        [self updateState]; [self hideLater];
        return;
    }
    [hideTimer invalidate]; hideTimer = nil;
    runtime_input_settings_suspend(1);
    gearButton.alpha = 0.9;
    if (!settingsOverlay) {
        settingsOverlay = [[UIView alloc] initWithFrame:self.bounds];
        settingsOverlay.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
        UITapGestureRecognizer *bgTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(closeSettingsOnBg:)];
        bgTap.cancelsTouchesInView = NO;
        [settingsOverlay addGestureRecognizer:bgTap];

        settingsCard = [[UIScrollView alloc] initWithFrame:CGRectZero];
        settingsCard.clipsToBounds = YES;
        settingsCard.backgroundColor = [UIColor colorWithRed:0.11 green:0.12 blue:0.15 alpha:0.95];
        settingsCard.layer.cornerRadius = 16;
        settingsCard.layer.borderWidth = 1.0;
        settingsCard.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.18].CGColor;
        [settingsOverlay addSubview:settingsCard];

        UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(18, 10, 220, 26)];
        title.text = @"游戏设置";
        title.font = [UIFont systemFontOfSize:17 weight:UIFontWeightBold];
        title.textColor = UIColor.whiteColor;
        [settingsCard addSubview:title];

        UIButton *closeBtn = [UIButton buttonWithType:UIButtonTypeSystem];
        closeBtn.tag = 1001;
        [closeBtn setTitle:@"✕" forState:UIControlStateNormal];
        closeBtn.titleLabel.font = [UIFont systemFontOfSize:18 weight:UIFontWeightBold];
        [closeBtn setTitleColor:[UIColor colorWithWhite:0.85 alpha:1] forState:UIControlStateNormal];
        [closeBtn addTarget:self action:@selector(toggleSettingsPanel) forControlEvents:UIControlEventTouchUpInside];
        [settingsCard addSubview:closeBtn];

        UILabel *fpsLbl = [[UILabel alloc] initWithFrame:CGRectZero];
        fpsLbl.tag = 1002;
        fpsLbl.text = @"最高帧率";
        fpsLbl.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
        fpsLbl.textColor = [UIColor colorWithWhite:0.9 alpha:1];
        [settingsCard addSubview:fpsLbl];

        fpsSeg = [[UISegmentedControl alloc] initWithItems:@[@"30 FPS", @"60 FPS", @"90 FPS", @"120 FPS"]];
        fpsSeg.selectedSegmentTintColor = [UIColor colorWithRed:0.20 green:0.52 blue:0.95 alpha:1];
        [fpsSeg setTitleTextAttributes:@{NSForegroundColorAttributeName: UIColor.whiteColor, NSFontAttributeName: [UIFont systemFontOfSize:13 weight:UIFontWeightMedium]} forState:UIControlStateNormal];
        [fpsSeg addTarget:self action:@selector(onFpsChanged:) forControlEvents:UIControlEventValueChanged];
        [settingsCard addSubview:fpsSeg];

        UILabel *padLbl = [[UILabel alloc] initWithFrame:CGRectZero];
        padLbl.tag = 1003;
        padLbl.text = @"手柄类型";
        padLbl.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
        padLbl.textColor = [UIColor colorWithWhite:0.9 alpha:1];
        [settingsCard addSubview:padLbl];

        padSeg = [[UISegmentedControl alloc] initWithItems:@[@"自动", @"Xbox", @"PS", @"Switch", @"第三方Pro"]];
        padSeg.selectedSegmentTintColor = [UIColor colorWithRed:0.20 green:0.52 blue:0.95 alpha:1];
        [padSeg setTitleTextAttributes:@{NSForegroundColorAttributeName: UIColor.whiteColor, NSFontAttributeName: [UIFont systemFontOfSize:12 weight:UIFontWeightMedium]} forState:UIControlStateNormal];
        [padSeg addTarget:self action:@selector(onPadChanged:) forControlEvents:UIControlEventValueChanged];
        [settingsCard addSubview:padSeg];

        padStatusLabel = [[UILabel alloc] initWithFrame:CGRectZero];
        padStatusLabel.font = [UIFont systemFontOfSize:11.5 weight:UIFontWeightRegular];
        padStatusLabel.textColor = [UIColor colorWithWhite:0.68 alpha:1];
        [settingsCard addSubview:padStatusLabel];

        UILabel *hudLbl = [[UILabel alloc] initWithFrame:CGRectZero];
        hudLbl.tag = 1005;
        hudLbl.text = @"状态显示";
        hudLbl.font = [UIFont systemFontOfSize:14 weight:UIFontWeightMedium];
        hudLbl.textColor = [UIColor colorWithWhite:0.9 alpha:1];
        [settingsCard addSubview:hudLbl];

        hudSeg = [[UISegmentedControl alloc] initWithItems:@[@"完整信息", @"仅帧率", @"隐藏"]];
        hudSeg.selectedSegmentTintColor = [UIColor colorWithRed:0.20 green:0.52 blue:0.95 alpha:1];
        [hudSeg setTitleTextAttributes:@{NSForegroundColorAttributeName: UIColor.whiteColor, NSFontAttributeName: [UIFont systemFontOfSize:12 weight:UIFontWeightMedium]} forState:UIControlStateNormal];
        [hudSeg addTarget:self action:@selector(onHudChanged:) forControlEvents:UIControlEventValueChanged];
        [settingsCard addSubview:hudSeg];

        mappingButton = [UIButton buttonWithType:UIButtonTypeSystem];
        [mappingButton setTitle:@"手柄按键映射  ›" forState:UIControlStateNormal];
        [mappingButton addTarget:self action:@selector(openMapping) forControlEvents:UIControlEventTouchUpInside];
        [settingsCard addSubview:mappingButton];
        keyboardButton = [UIButton buttonWithType:UIButtonTypeSystem];
        [keyboardButton setTitle:@"键盘绑定与映射  ›" forState:UIControlStateNormal];
        [keyboardButton addTarget:self action:@selector(openKeyboardMapping) forControlEvents:UIControlEventTouchUpInside];
        [settingsCard addSubview:keyboardButton];
        [self addSubview:settingsOverlay];
    }
    [self setNeedsLayout];
    [self layoutIfNeeded];
    [self refreshSettingsValues];
    settingsOverlay.hidden = NO;
}

- (void)closeSettingsOnBg:(UITapGestureRecognizer *)tap {
    CGPoint p = [tap locationInView:settingsCard];
    if (!CGRectContainsPoint(settingsCard.bounds, p)) [self toggleSettingsPanel];
}


- (NSArray<NSString *> *)mappingTargets {
    return @[@"A", @"B", @"X", @"Y", @"L3", @"R3", @"L", @"R", @"ZL", @"ZR", @"+", @"−", @"左", @"上", @"右", @"下"];
}

- (void)closeMapping {
    keyboardPage = NO;
    [mappingCard removeFromSuperview]; mappingCard = nil;
    [self refreshSettingsValues];
}

- (void)mappingSaveFailed {
    UILabel *status = (UILabel *)[mappingCard viewWithTag:2100];
    status.text = @"保存失败：未更改映射，请检查可用空间。";
    status.textColor = UIColor.systemRedColor;
}

#include "keyboard_settings.inc"

- (void)controllerPlayerChanged:(UISegmentedControl *)sender {
    controllerEditSlot = (int)sender.selectedSegmentIndex;
    [self openMapping];
}

- (void)openMapping {
    keyboardPage = NO;
    [mappingCard removeFromSuperview];
    mappingCard = [[UIScrollView alloc] initWithFrame:settingsCard.frame];
    mappingCard.backgroundColor = [UIColor colorWithRed:0.11 green:0.12 blue:0.15 alpha:1];
    mappingCard.layer.cornerRadius = 16;
    mappingCard.clipsToBounds = YES;
    mappingCard.alwaysBounceVertical = YES;
    [settingsOverlay addSubview:mappingCard];
    CGFloat w = mappingCard.bounds.size.width;
    UIButton *back = [UIButton buttonWithType:UIButtonTypeSystem];
    back.frame = CGRectMake(12, 8, 70, 32);
    [back setTitle:@"‹ 返回" forState:UIControlStateNormal];
    [back addTarget:self action:@selector(closeMapping) forControlEvents:UIControlEventTouchUpInside];
    [mappingCard addSubview:back];
    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(92, 8, w - 104, 32)];
    title.text = @"玩家手柄映射"; title.textColor = UIColor.whiteColor;
    title.font = [UIFont boldSystemFontOfSize:17]; [mappingCard addSubview:title];
    UILabel *help = [[UILabel alloc] initWithFrame:CGRectMake(16, 44, w - 32, 64)];
    help.text = @"左：实体位置（Xbox 标签） → 右：游戏按键\n更改立即保存；不影响摇杆和触屏。\n输入来源决定是否合并键盘、手柄和触屏。";
    help.numberOfLines = 3; help.font = [UIFont systemFontOfSize:12];
    help.textColor = UIColor.lightGrayColor; [mappingCard addSubview:help];
    UILabel *status = [[UILabel alloc] initWithFrame:CGRectMake(16, 110, w - 32, 48)];
    status.tag = 2100; status.numberOfLines = 2; status.font = [UIFont systemFontOfSize:12];
    status.textColor = UIColor.lightGrayColor;
    status.text = runtime_gamepad_slot_count() == 1 ? @"掌机模式仅启用玩家 1。类型与映射按玩家保存。" :
        @"支持玩家 1–4：分别绑定设备，再按游戏提示加入。触屏仅控制玩家 1；系统键盘可绑定一个玩家。重连可能需重新选择。";
    NSUInteger connectedPads = runtime_gamepad_devices().count;
    status.text = [NSString stringWithFormat:@"系统当前识别 %lu 个手柄。玩家1可用触屏，玩家2–4用独立设备；关闭设置后按游戏提示加入。", (unsigned long)connectedPads];
    [mappingCard addSubview:status];
    int count = runtime_gamepad_slot_count();
    if (controllerEditSlot >= count) controllerEditSlot = 0;
    int slot = controllerEditSlot;
    NSMutableArray *players = [NSMutableArray array];
    for (int i = 0; i < count; ++i) [players addObject:[NSString stringWithFormat:@"玩家 %d", i + 1]];
    UISegmentedControl *select = [[UISegmentedControl alloc] initWithItems:players];
    select.frame = CGRectMake(16, 162, w - 32, 32); select.selectedSegmentIndex = slot;
    [select addTarget:self action:@selector(controllerPlayerChanged:) forControlEvents:UIControlEventValueChanged];
    [mappingCard addSubview:select];
    UIButton *typeButton = [UIButton buttonWithType:UIButtonTypeSystem];
    typeButton.frame = CGRectMake(16, 198, w - 32, 36);
    NSArray *types = @[@"自动识别", @"Xbox", @"PlayStation", @"Nintendo Switch", @"第三方 Pro"];
    int currentType = runtime_gamepad_slot_type(slot);
    [typeButton setTitle:[NSString stringWithFormat:@"手柄类型：%@ ▾", types[currentType]] forState:UIControlStateNormal];
    NSMutableArray *typeActions = [NSMutableArray array];
    __weak TOTKTouchPad *typeWeakSelf = self;
    for (int type = 0; type < (int)types.count; ++type) {
        UIAction *action = [UIAction actionWithTitle:types[type] image:nil identifier:nil handler:^(UIAction *a) {
            TOTKTouchPad *pad = typeWeakSelf; if (!pad) return;
            if (!runtime_gamepad_slot_type_set(slot, type)) { [pad mappingSaveFailed]; return; }
            [pad openMapping];
        }];
        action.state = type == currentType ? UIMenuElementStateOn : UIMenuElementStateOff;
        [typeActions addObject:action];
    }
    typeButton.menu = [UIMenu menuWithTitle:@"仅更改此玩家的手柄类型" children:typeActions];
    typeButton.showsMenuAsPrimaryAction = YES; [mappingCard addSubview:typeButton];
    UIButton *deviceButton = [UIButton buttonWithType:UIButtonTypeSystem];
    deviceButton.frame = CGRectMake(16, 236, w - 32, 36);
    deviceButton.titleLabel.adjustsFontSizeToFitWidth = YES;
    [deviceButton setTitle:runtime_gamepad_device_label(slot) forState:UIControlStateNormal];
    NSMutableArray *deviceActions = [NSMutableArray array];
    NSMutableArray *devices = [NSMutableArray arrayWithArray:@[
        @{@"id":@"auto",@"name":@"自动分配手柄"}, @{@"id":@"none",@"name":@"不使用实体手柄"}]];
    [devices addObjectsFromArray:runtime_gamepad_devices()];
    NSString *currentDevice = runtime_gamepad_device_binding(slot);
    for (NSDictionary *entry in devices) {
        NSString *key = entry[@"id"];
        UIAction *action = [UIAction actionWithTitle:entry[@"name"] image:nil identifier:nil handler:^(UIAction *a) {
            TOTKTouchPad *pad = typeWeakSelf; if (!pad) return;
            if (!runtime_input_source_set(slot, [key isEqual:@"none"] ? INPUT_SOURCE_OFF : INPUT_SOURCE_PAD)) { [pad mappingSaveFailed]; return; }
            if (!runtime_gamepad_device_bind(slot, key)) {
                UILabel *label = (UILabel *)[pad->mappingCard viewWithTag:2100];
                label.text = @"输入来源已切换；设备绑定未更改：设备离线、已被预留，或保存失败。";
                label.textColor = UIColor.systemRedColor; return;
            }
            [pad openMapping];
        }];
        action.state = [key isEqual:currentDevice] ? UIMenuElementStateOn : UIMenuElementStateOff;
        [deviceActions addObject:action];
    }
    [deviceActions addObject:[UIAction actionWithTitle:@"刷新设备列表" image:nil identifier:nil handler:^(UIAction *a) {
        [typeWeakSelf openMapping];
    }]];
    deviceButton.menu = [UIMenu menuWithTitle:@"选择此玩家的实体手柄" children:deviceActions];
    deviceButton.showsMenuAsPrimaryAction = YES; [mappingCard addSubview:deviceButton];
    UIButton *sourceButton = [UIButton buttonWithType:UIButtonTypeSystem];
    sourceButton.frame = CGRectMake(16,274,w-32,36);
    NSArray *sourceNames = @[@"兼容：混合输入",@"仅手柄",@"仅键盘",@"键盘＋鼠标（Mac）",@"仅触屏",@"关闭此玩家输入"];
    int currentSource = runtime_input_source(slot);
    [sourceButton setTitle:[@"输入来源：" stringByAppendingString:sourceNames[currentSource]] forState:UIControlStateNormal];
    NSMutableArray *sourceActions = [NSMutableArray array];
    for (int source = 0; source < INPUT_SOURCE_COUNT; ++source) {
        if (source == INPUT_SOURCE_KEYBOARD_MOUSE || (source == INPUT_SOURCE_TOUCH && slot != 0)) continue;
        UIAction *action = [UIAction actionWithTitle:sourceNames[source] image:nil identifier:nil handler:^(UIAction *a) {
            TOTKTouchPad *pad = typeWeakSelf; if (!pad) return;
            if (!runtime_input_source_set(slot,source)) { [pad mappingSaveFailed]; return; }
            if (source == INPUT_SOURCE_KEYBOARD && !runtime_keyboard_bind(slot)) {
                UILabel *status = (UILabel *)[mappingCard viewWithTag:2100];
                status.text = @"输入来源已保存，但键盘归属保存失败；原归属未更改。请在键盘页重新绑定。";
                status.textColor = UIColor.systemRedColor; return;
            }
            [pad openMapping];
        }];
        action.state = source == currentSource ? UIMenuElementStateOn : UIMenuElementStateOff;
        [sourceActions addObject:action];
    }
    sourceButton.menu = [UIMenu menuWithTitle:@"独占模式不会回退到其他输入源" children:sourceActions];
    sourceButton.showsMenuAsPrimaryAction = YES; [mappingCard addSubview:sourceButton];
    NSArray *physical = @[@"下方键（Xbox A）", @"右方键（Xbox B）", @"左方键（Xbox X）", @"上方键（Xbox Y）",
        @"L3 左摇杆按下", @"R3 右摇杆按下", @"L / LB", @"R / RB", @"ZL / LT", @"ZR / RT",
        @"+ / Menu", @"− / View", @"方向左", @"方向上", @"方向右", @"方向下"];
    const int order[] = {8, 6, 11, 10, 7, 9, 13, 15, 12, 14, 0, 1, 2, 3, 4, 5};
    NSArray<NSString *> *targets = [self mappingTargets];
    __weak TOTKTouchPad *weakSelf = self;
    for (int row = 0; row < 16; ++row) {
        int input = order[row]; CGFloat y = 316 + row * 42;
        UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(16, y, w * .58 - 16, 36)];
        label.text = physical[input]; label.font = [UIFont systemFontOfSize:14];
        label.adjustsFontSizeToFitWidth = YES; label.textColor = UIColor.whiteColor;
        [mappingCard addSubview:label];
        UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
        button.frame = CGRectMake(w * .58, y, w * .42 - 16, 36);
        int current = runtime_gamepad_slot_mapping_get(slot, input);
        [button setTitle:[NSString stringWithFormat:@"→ %@ ▾", current < 0 ? @"不绑定" : targets[current]] forState:UIControlStateNormal];
        NSMutableArray<UIMenuElement *> *actions = [NSMutableArray array];
        for (int target = -1; target < 16; ++target) {
            UIAction *action = [UIAction actionWithTitle:target < 0 ? @"不绑定" : targets[target] image:nil identifier:nil handler:^(UIAction *a) {
                TOTKTouchPad *pad = weakSelf; if (!pad) return;
                if (!runtime_gamepad_slot_mapping_set(slot, input, target)) { [pad mappingSaveFailed]; return; }
                CGPoint offset = pad->mappingCard.contentOffset;
                [pad openMapping]; pad->mappingCard.contentOffset = offset;
            }];
            action.state = target == current ? UIMenuElementStateOn : UIMenuElementStateOff;
            [actions addObject:action];
        }
        button.menu = [UIMenu menuWithTitle:@"选择游戏里的 Switch 按键" children:actions];
        button.showsMenuAsPrimaryAction = YES;
        [mappingCard addSubview:button];
    }
    UIButton *reset = [UIButton buttonWithType:UIButtonTypeSystem];
    reset.frame = CGRectMake(16, 994, w - 32, 40);
    [reset setTitle:@"恢复默认映射…" forState:UIControlStateNormal];
    reset.menu = [UIMenu menuWithTitle:@"仅重置这 16 个实体按键" children:@[
        [UIAction actionWithTitle:@"确认恢复默认" image:nil identifier:nil handler:^(UIAction *a) {
            TOTKTouchPad *pad = weakSelf; if (!pad) return;
            if (runtime_gamepad_slot_mapping_reset(slot)) [pad openMapping]; else [pad mappingSaveFailed];
        }]]];
    reset.showsMenuAsPrimaryAction = YES; [mappingCard addSubview:reset];
    mappingCard.contentSize = CGSizeMake(w, 1050);
}

- (void)onFpsChanged:(UISegmentedControl *)seg {
    static const int caps[] = {30, 60, 90, 120};
    NSInteger idx = seg.selectedSegmentIndex;
    if (idx < 0 || idx > 3) return;
    int hz = caps[idx];
    runtime_fps_cap_set(hz);
    [NSUserDefaults.standardUserDefaults setInteger:hz forKey:@"TOTK_IOS_FPS_CAP"];
}

- (void)onPadChanged:(UISegmentedControl *)seg {
    NSInteger idx = seg.selectedSegmentIndex;
    if (idx < 0 || idx > 4) return;
    runtime_gamepad_type_set((int)idx);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(100 * NSEC_PER_MSEC)), dispatch_get_main_queue(), ^{
        [self refreshSettingsValues];
    });
}

- (void)onHudChanged:(UISegmentedControl *)seg {
    NSInteger idx = seg.selectedSegmentIndex;
    int target = (idx == 2) ? 0 : (idx == 1) ? 2 : 1;
    fps_overlay_set_hud_mode(target);
}

- (void)controllersChanged:(NSNotification *)note {
    BOOL connected = NO;
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad && !([note.name isEqualToString:GCControllerDidDisconnectNotification] && controller == note.object)) connected = YES;
    /* Rebuild menu snapshots even when the number of connected devices stays
       nonzero (a different controller may have connected/disconnected). */
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->mappingCard && !self->keyboardPage) [self openMapping];
        [self refreshSettingsValues];
    });
    if (connected == gamepad) return;
    gamepad = connected; [hideTimer invalidate]; hideTimer = nil;
    if (gamepad && runtime_input_source(0) != INPUT_SOURCE_TOUCH) [self conceal]; else { concealed = NO; [self setNeedsDisplay]; }
}

- (void)conceal {
    concealed = YES;
    gearButton.alpha = 0.28;
    // Fingers on the screen when it hides would otherwise hold their buttons and sticks.
    [points removeAllObjects]; [leftOrigins removeAllObjects]; [rightOrigins removeAllObjects]; [self updateState];
}

- (void)hideLater {
    [hideTimer invalidate]; hideTimer = nil;
    if (!gamepad || points.count || runtime_input_source(0) == INPUT_SOURCE_TOUCH) return;
    __weak TOTKTouchPad *weakSelf = self;
    hideTimer = [NSTimer scheduledTimerWithTimeInterval:kHideAfter repeats:NO block:^(NSTimer *timer) {
        (void)timer; TOTKTouchPad *pad = weakSelf; if (pad) [pad concealIfIdle];
    }];
}
- (void)concealIfIdle { if (gamepad && !points.count && runtime_input_source(0) != INPUT_SOURCE_TOUCH) [self conceal]; }

- (CGRect)area { return UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets); }
- (CGFloat)stickRadius { return MIN(70, self.area.size.height * .16); }
/* Distance between the centre of a button diamond (A/B/X/Y, the d-pad) and each of its buttons. */
- (CGFloat)diamond { return MIN(52, self.area.size.height * .14); }
- (CGFloat)hitRadius { return MIN(36, self.diamond * .68); }
/* How far the two columns sit inside the safe area: 14% of its width. On an iPhone they move halfway from there toward
   the screen edge, so the gap between the screen edge and the d-pad or A/B/X/Y is half as wide (the user's choice,
   2026-10-19; about 38 points on an iPhone 14, clear of the notch); the stick rings stay inside the safe area. */
- (CGFloat)columnInset {
    CGFloat full = self.area.size.width * .14;
    if (self.traitCollection.userInterfaceIdiom != UIUserInterfaceIdiomPhone) return full;
    CGFloat side = MAX(self.safeAreaInsets.left, self.safeAreaInsets.right), edgeGap = side + full - (self.diamond + kButtonSize / 2);
    return MAX(full - edgeGap / 2, self.stickRadius + 4);
}
/* The shoulder buttons move outward the same way on an iPhone: the gap between the screen edge and ZL / ZR is halved,
   and L / R move with them. */
- (CGFloat)shoulderShift {
    if (self.traitCollection.userInterfaceIdiom != UIUserInterfaceIdiomPhone) return 0;
    CGFloat side = MAX(self.safeAreaInsets.left, self.safeAreaInsets.right);
    return (side + self.area.size.width * .05 - kButtonSize / 2) / 2;
}
- (CGPoint)restingStick:(BOOL)right {
    CGRect a = self.area; CGFloat inset = self.columnInset + self.stickRadius * 1.9;
    return CGPointMake(right ? CGRectGetMaxX(a) - inset : CGRectGetMinX(a) + inset, CGRectGetMinY(a) + a.size.height * .80);
}

- (void)buttons:(TouchButton *)b {
    CGRect a = self.area; CGFloat x = CGRectGetMinX(a), y = CGRectGetMinY(a), w = a.size.width, h = a.size.height, d = self.diamond;
    CGFloat top = y + h * .12, bottom = y + h * .88;
    CGFloat inset = self.columnInset, shift = self.shoulderShift;
    CGPoint face = CGPointMake(x + w - inset, y + h * .42), pad = CGPointMake(x + inset, y + h * .42);
    TouchButton all[kButtonCount] = {
        {{face.x + d, face.y}, 1, @"A", kButtonSize}, {{face.x, face.y + d}, 2, @"B", kButtonSize}, {{face.x, face.y - d}, 4, @"X", kButtonSize}, {{face.x - d, face.y}, 8, @"Y", kButtonSize},
        {{x + w * .05 - shift, top}, 256, @"ZL", kButtonSize}, {{x + w * .16 - shift, top}, 64, @"L", kButtonSize}, {{x + w * .84 + shift, top}, 128, @"R", kButtonSize}, {{x + w * .95 + shift, top}, 512, @"ZR", kButtonSize},
        {{x + w * .40, top}, 2048, @"−", kButtonSize}, {{x + w * .60, top}, 1024, @"+", kButtonSize},
        {{pad.x - d, pad.y}, 4096, @"◀", kButtonSize}, {{pad.x, pad.y - d}, 8192, @"▲", kButtonSize}, {{pad.x + d, pad.y}, 16384, @"▶", kButtonSize}, {{pad.x, pad.y + d}, 32768, @"▼", kButtonSize},
        {{x + w * .05 - shift, bottom}, 16, @"L3", kButtonSize}, {{x + w * .95 + shift, bottom}, 32, @"R3", kButtonSize}};
    for (int i = 0; i < kButtonCount; i++) b[i] = all[i];
}

- (uint64_t)buttonAt:(CGPoint)p {
    TouchButton b[kButtonCount]; [self buttons:b]; CGFloat hit = self.hitRadius; uint64_t bits = 0;
    for (int i = 0; i < kButtonCount; i++) if (hypot(p.x - b[i].at.x, p.y - b[i].at.y) < MIN(hit, b[i].size * .75)) bits |= b[i].bit;
    return bits;
}

static void stick_axes(CGPoint from, CGPoint to, CGFloat radius, int32_t *x, int32_t *y) {
    CGFloat dx = to.x - from.x, dy = from.y - to.y, dist = hypot(dx, dy);
    if (dist > radius) { dx *= radius / dist; dy *= radius / dist; }
    *x = (int32_t)(dx / radius * 32767); *y = (int32_t)(dy / radius * 32767);
}

- (void)updateState {
    RuntimeGamepadState s = {.connected = 1}; CGFloat radius = self.stickRadius;
    for (UITouch *t in points.keyEnumerator) {
        CGPoint p = [points objectForKey:t].CGPointValue; NSValue *left = [leftOrigins objectForKey:t], *right = [rightOrigins objectForKey:t];
        if (left) stick_axes(left.CGPointValue, p, radius, &s.lx, &s.ly);
        else if (right) stick_axes(right.CGPointValue, p, radius, &s.rx, &s.ry);
        else s.buttons |= [self buttonAt:p];
    }
    runtime_touch_pad_set(&s); [self setNeedsDisplay];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    if (settingsOverlay && !settingsOverlay.hidden) return;
    concealed = NO; gearButton.alpha = 0.85; [hideTimer invalidate]; hideTimer = nil;
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self]; NSValue *v = [NSValue valueWithCGPoint:p]; [points setObject:v forKey:t];
        if ([self buttonAt:p]) continue;
        [(p.x < CGRectGetMidX(self.bounds) ? leftOrigins : rightOrigins) setObject:v forKey:t];
    }
    [self updateState];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *t in touches) [points setObject:[NSValue valueWithCGPoint:[t locationInView:self]] forKey:t];
    [self updateState];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *t in touches) { [points removeObjectForKey:t]; [leftOrigins removeObjectForKey:t]; [rightOrigins removeObjectForKey:t]; }
    [self updateState]; [self hideLater];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self touchesEnded:touches withEvent:event]; }
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsDisplay]; }

static void draw_stick(CGContextRef c, CGPoint o, CGPoint p, CGFloat r) {
    int32_t x, y; stick_axes(o, p, r, &x, &y); CGContextStrokeEllipseInRect(c, CGRectMake(o.x - r, o.y - r, 2 * r, 2 * r));
    CGPoint k = CGPointMake(o.x + x / 32767.0 * r, o.y - y / 32767.0 * r);
    CGRect knob = CGRectMake(k.x - kButtonSize / 2, k.y - kButtonSize / 2, kButtonSize, kButtonSize);
    CGContextFillEllipseInRect(c, knob); CGContextStrokeEllipseInRect(c, knob);
}

- (void)drawRect:(CGRect)rect {
    if (concealed) return;
    (void)rect; CGContextRef c = UIGraphicsGetCurrentContext(); UIColor *ink = [UIColor colorWithWhite:1 alpha:.32];
    // One fill for every button and knob, set before each fill: drawing a label leaves the context's fill colour at the
    // label's, which once made every button after A (the first drawn) brighter than A and the stick knobs.
    CGColorRef fill = ink.CGColor; CGContextSetStrokeColorWithColor(c, ink.CGColor); CGContextSetFillColorWithColor(c, fill); CGContextSetLineWidth(c, 2);
    CGFloat r = self.stickRadius;
    // Each stick is drawn where its finger went down, or at its resting place while idle.
    if (!leftOrigins.count) { CGPoint rest = [self restingStick:NO]; draw_stick(c, rest, rest, r); }
    if (!rightOrigins.count) { CGPoint rest = [self restingStick:YES]; draw_stick(c, rest, rest, r); }
    for (UITouch *t in leftOrigins.keyEnumerator) draw_stick(c, [leftOrigins objectForKey:t].CGPointValue, [points objectForKey:t].CGPointValue, r);
    for (UITouch *t in rightOrigins.keyEnumerator) draw_stick(c, [rightOrigins objectForKey:t].CGPointValue, [points objectForKey:t].CGPointValue, r);
    NSDictionary *attrs = @{NSFontAttributeName: [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold], NSForegroundColorAttributeName: ink};
    TouchButton b[kButtonCount]; [self buttons:b];
    for (int i = 0; i < kButtonCount; i++) {
        CGFloat size = b[i].size; CGRect box = CGRectMake(b[i].at.x - size / 2, b[i].at.y - size / 2, size, size);
        CGContextSetFillColorWithColor(c, fill); CGContextFillEllipseInRect(c, box); CGContextStrokeEllipseInRect(c, box);
        CGSize z = [b[i].label sizeWithAttributes:attrs]; [b[i].label drawAtPoint:CGPointMake(b[i].at.x - z.width / 2, b[i].at.y - z.height / 2) withAttributes:attrs];
    }
}
@end
