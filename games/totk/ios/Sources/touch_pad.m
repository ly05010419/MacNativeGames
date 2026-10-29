#import "touch_pad.h"
#import <GameController/GameController.h>
#include <math.h>
#include <stdlib.h>
#include "runtime.h"
#include "settings_map.h"

/* Layout inside the safe area (clear of the iPhone's notch, rounded corners and home indicator), like a Switch
   controller: ZL/L and R/ZR along the top with − and + between them, and two columns mirroring each other: on the left
   the d-pad above the left stick, on the right A/B/X/Y above the camera stick. Each stick is drawn at its resting place
   until a finger takes it; the sticks sit a little toward the middle. L3 / R3 (stick press) are in the bottom corners,
   under ZL / ZR.
   A touch that misses every button is a stick, centred where the finger went down: the left stick on the left half,
   the camera (right stick) on the right half. Bits are the Npad buttons. While a game controller is connected the layer
   is hidden; a touch shows it and works as usual, and it hides again 3 seconds after the last finger lifts.
   Between − and + a smaller button shows the frame rate the game is held to; a tap switches it at once. With the
   custom layer off it saves the choice in the pane's preset row (TOTK_PHONE_FPS) for the next start; with it on the
   pane owns the cap, so the button only holds 30 until the next launch. */
typedef struct { CGPoint at; uint64_t bit; NSString *label; CGFloat size; } TouchButton;
enum { kButtonCount = 16 };
static const CGFloat kButtonSize = 46;
static const NSTimeInterval kHideAfter = 3;   // seconds after the last finger lifts, with a controller connected
static const CGFloat kFpsButtonSize = 34;

@implementation TOTKTouchPad {
    NSMapTable<UITouch *, NSValue *> *points;
    NSMapTable<UITouch *, NSValue *> *leftOrigins;    // where each left-stick touch went down
    NSMapTable<UITouch *, NSValue *> *rightOrigins;   // the same for the camera stick
    BOOL gamepad, concealed;                          // a controller is connected; the layer is hidden for it
    NSTimer *hideTimer;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor; self.contentMode = UIViewContentModeRedraw;
        points = [NSMapTable strongToStrongObjectsMapTable];
        leftOrigins = [NSMapTable strongToStrongObjectsMapTable];
        rightOrigins = [NSMapTable strongToStrongObjectsMapTable];
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidConnectNotification object:nil];
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidDisconnectNotification object:nil];
        [center addObserver:self selector:@selector(controllersChanged:) name:GCKeyboardDidConnectNotification object:nil];
        [center addObserver:self selector:@selector(controllersChanged:) name:GCKeyboardDidDisconnectNotification object:nil];
        [self controllersChanged:nil];
    }
    return self;
}

- (void)controllersChanged:(NSNotification *)note {
    BOOL connected = NO;
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad && !([note.name isEqualToString:GCControllerDidDisconnectNotification] && controller == note.object)) connected = YES;
    /* A connect notification carries the keyboard itself; coalescedKeyboard is the one that stays. A disconnect's object
       is already gone, so ask for the remaining keyboard instead. */
    GCKeyboard *keyboard = [note.name isEqualToString:GCKeyboardDidConnectNotification] && [note.object isKindOfClass:GCKeyboard.class]
                         ? note.object : GCKeyboard.coalescedKeyboard;
    const char *keyboardMode = getenv("TOTK_KEYBOARD");
    /* The pane's switch arrives as "1"/"0" (the generic dispatch in main.m); "off" is the older spelling. */
    BOOL keyboardOn = !(keyboardMode && (!strcmp(keyboardMode, "off") || !strcmp(keyboardMode, "0"))) && keyboard != nil;
    const char *touchWithKeyboard = getenv("TOTK_TOUCH_WITH_KEYBOARD");
    BOOL keepTouch = touchWithKeyboard && !strcmp(touchWithKeyboard, "1");
    BOOL hidden = connected || (keyboardOn && !keepTouch);
    /* coalescedKeyboard can still name the departing keyboard when the notification arrives; look again when the
       system has settled, so the touch layer cannot stay hidden with no keyboard attached. */
    if ([note.name isEqualToString:GCKeyboardDidDisconnectNotification])
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            [self controllersChanged:nil];
        });
    if (hidden == gamepad) return;
    gamepad = hidden; [hideTimer invalidate]; hideTimer = nil;
    if (gamepad) [self conceal]; else { concealed = NO; [self setNeedsDisplay]; }
}

- (void)conceal {
    concealed = YES;
    // Fingers on the screen when it hides would otherwise hold their buttons and sticks.
    [points removeAllObjects]; [leftOrigins removeAllObjects]; [rightOrigins removeAllObjects]; [self updateState];
}

- (void)hideLater {
    [hideTimer invalidate]; hideTimer = nil;
    if (!gamepad || points.count) return;
    __weak TOTKTouchPad *weakSelf = self;
    hideTimer = [NSTimer scheduledTimerWithTimeInterval:kHideAfter repeats:NO block:^(NSTimer *timer) {
        (void)timer; TOTKTouchPad *pad = weakSelf; if (pad) [pad concealIfIdle];
    }];
}
- (void)concealIfIdle { if (gamepad && !points.count) [self conceal]; }

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

- (CGPoint)fpsButton { CGRect a = self.area; return CGPointMake(CGRectGetMidX(a), CGRectGetMinY(a) + a.size.height * .12); }
- (BOOL)fpsButtonAt:(CGPoint)p { CGPoint f = self.fpsButton; return hypot(p.x - f.x, p.y - f.y) < kFpsButtonSize * .75; }
- (void)switchFrameRate {
    /* The pane's two layers (settings_map.c): the preset layer saves 30 / 60 in its own row; the custom layer keeps the
       cap in the pane, so this only holds 30 until the next launch and saves nothing. */
    BOOL custom = [NSUserDefaults.standardUserDefaults boolForKey:@"TOTK_CUSTOM_PICTURE"];
    int fps = totk_frame_button_target(runtime_phone_fps() == 30);
    runtime_phone_fps_set(fps);
    const char *key = totk_frame_button_key(custom);
    if (key) [NSUserDefaults.standardUserDefaults setObject:fps == 30 ? @"30" : @"60" forKey:@(key)];
    [self setNeedsDisplay];
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
    (void)event; concealed = NO; [hideTimer invalidate]; hideTimer = nil;
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self]; NSValue *v = [NSValue valueWithCGPoint:p]; [points setObject:v forKey:t];
        if ([self fpsButtonAt:p]) { [self switchFrameRate]; continue; }   // pressed no Npad button and moves no stick
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
    // The frame rate button: a smaller rounded square, its label the rate the game is held to now.
    CGPoint f = self.fpsButton; CGRect box = CGRectMake(f.x - kFpsButtonSize / 2, f.y - kFpsButtonSize / 2, kFpsButtonSize, kFpsButtonSize);
    UIBezierPath *key = [UIBezierPath bezierPathWithRoundedRect:box cornerRadius:8];
    CGContextSetFillColorWithColor(c, fill); [key fill]; [key stroke];
    /* The runtime only knows "held at 30" or not, so the label shows that state: after a tap that lifts the pane's cap
       it agrees with the pacing instead of repeating the stale pane value. */
    NSString *rate = runtime_phone_fps() == 30 ? @"30" : @"60";
    NSDictionary *small = @{NSFontAttributeName: [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightSemibold], NSForegroundColorAttributeName: ink};
    CGSize z = [rate sizeWithAttributes:small]; [rate drawAtPoint:CGPointMake(f.x - z.width / 2, f.y - z.height / 2) withAttributes:small];
}
@end
