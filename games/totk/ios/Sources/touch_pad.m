#import "touch_pad.h"
#import "device_motion.h"
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
   is hidden; a touch shows it and works as usual, and it hides again 3 seconds after the last finger lifts.
   A stick's ring is drawn only while a finger holds it (the user's request 2026-10-23: like a mobile game, nothing
   shows until it is touched); the rings show at their resting places only while the editor is open.

   Custom layout (arrange and size the buttons like a mobile game's control settings, the user's request 2026-10-23):
   a small gear handle at the top centre of the layer opens an editor - drag any button or stick's resting place to
   move it, tap it to select it (then its slider sets size and opacity), hold one to reset it to the default,
   and the round arrow resets all. While the editor is open the layer sends no game input. The layout lives in
   Documents/save/touch-layout.json, next to the game's own saves (Documents/save is the profile folder main.m sets as
   TOTK_PROFILE_DIR, and <profile>/save/<uid> holds the saves): backing up or moving the saves carries the button layout
   with them (the user's request 2026-10-24; a file left at the Documents root by an early build is read and moved once).
   Each item's id maps to its position as fractions of the safe area, plus an optional size
   in points and an optional opacity. Fractions survive rotating to the other landscape direction (the notch changes
   side) and map across devices; the defaults above stay the fallback, and saved values are clamped when read. The two
   editor handles stay fixed: a layout one could hide the way back into the editor. */
typedef struct { CGPoint at; uint64_t bit; NSString *label; CGFloat size; } TouchButton;
enum { kButtonCount = 16 };
/* The two sticks' resting places and the two editor handles are not game buttons: ids clear of every Npad bit. */
enum { kStickLeftID = 1ull << 40, kStickRightID = 1ull << 41, kEditID = 1ull << 42, kResetID = 1ull << 43 };
static const CGFloat kButtonSize = 46;
static const NSTimeInterval kHideAfter = 3;   // seconds after the last finger lifts, with a controller connected
static const NSTimeInterval kHoldReset = .6;
static const CGFloat kInkAlpha = .32;   // a button's opacity unless the player set one

@implementation TOTKTouchPad {
    NSMapTable<UITouch *, NSValue *> *points;
    NSMapTable<UITouch *, NSValue *> *leftOrigins;    // where each left-stick touch went down
    NSMapTable<UITouch *, NSValue *> *rightOrigins;   // the same for the camera stick
    BOOL gamepad, concealed;                          // a controller is connected; the layer is hidden for it
    NSTimer *hideTimer;
    NSMutableDictionary<NSString *, NSMutableDictionary *> *layout;   // item id -> {"x","y"} fractions, optional {"s"} points
    BOOL editMode;
    NSMapTable<UITouch *, NSString *> *drags;         // the finger moving this item
    NSMapTable<UITouch *, NSValue *> *dragOrigins;    // where that finger went down; moving cancels the hold-reset
    NSString *selected;                               // the item the editor's sliders set (a tap picks it)
    NSMapTable<UITouch *, NSNumber *> *sliderTouches; // a finger on a slider: the slider's tag (size or opacity)
    NSTimer *holdTimer;                               // holding an item without moving resets it
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor; self.contentMode = UIViewContentModeRedraw;
        points = [NSMapTable strongToStrongObjectsMapTable];
        leftOrigins = [NSMapTable strongToStrongObjectsMapTable];
        rightOrigins = [NSMapTable strongToStrongObjectsMapTable];
        drags = [NSMapTable strongToStrongObjectsMapTable];
        dragOrigins = [NSMapTable strongToStrongObjectsMapTable];
        sliderTouches = [NSMapTable strongToStrongObjectsMapTable];
        layout = [self readLayout];
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidConnectNotification object:nil];
        [center addObserver:self selector:@selector(controllersChanged:) name:GCControllerDidDisconnectNotification object:nil];
        [self controllersChanged:nil];
    }
    return self;
}

- (void)controllersChanged:(NSNotification *)note {
    BOOL connected = NO;
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad && !([note.name isEqualToString:GCControllerDidDisconnectNotification] && controller == note.object)) connected = YES;
    if (connected == gamepad) return;
    gamepad = connected; [hideTimer invalidate]; hideTimer = nil;
    if (gamepad && !editMode) [self conceal]; else { concealed = NO; [self setNeedsDisplay]; }
}

- (void)conceal {
    concealed = YES;
    // Fingers on the screen when it hides would otherwise hold their buttons and sticks.
    [points removeAllObjects]; [leftOrigins removeAllObjects]; [rightOrigins removeAllObjects]; [self updateState];
}

- (void)hideLater {
    [hideTimer invalidate]; hideTimer = nil;
    if (!gamepad || points.count || editMode) return;   // the editor stays up while it is being used
    __weak TOTKTouchPad *weakSelf = self;
    hideTimer = [NSTimer scheduledTimerWithTimeInterval:kHideAfter repeats:NO block:^(NSTimer *timer) {
        (void)timer; TOTKTouchPad *pad = weakSelf; if (pad) [pad concealIfIdle];
    }];
}
- (void)concealIfIdle { if (gamepad && !points.count && !editMode) [self conceal]; }

/* The saved layout: positions are fractions of the safe area (see the file comment), sizes are points.
   Anything malformed is dropped, everything kept is clamped, so an edited-by-hand file cannot wedge the layer. */
- (NSString *)layoutPath {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [[docs stringByAppendingPathComponent:@"save"] stringByAppendingPathComponent:@"touch-layout.json"];
}
- (NSMutableDictionary *)readLayout {
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    NSString *path = [self layoutPath]; NSData *raw = [NSData dataWithContentsOfFile:path];
    if (!raw) {   // a layout left at the Documents root by an early build: read it and move it next to the saves
        NSString *legacy = [[NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject
                            stringByAppendingPathComponent:@"touch-layout.json"] copy];
        if ((raw = [NSData dataWithContentsOfFile:legacy])) {
            [NSFileManager.defaultManager createDirectoryAtPath:[path stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
            if ([raw writeToFile:path atomically:YES]) [NSFileManager.defaultManager removeItemAtPath:legacy error:nil];
        }
    }
    NSDictionary *got = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:NULL] : nil;
    if (![got isKindOfClass:NSDictionary.class]) return out;
    for (NSString *item in got) {
        NSDictionary *d = got[item];
        if (![d isKindOfClass:NSDictionary.class] || ![d[@"x"] isKindOfClass:NSNumber.class] || ![d[@"y"] isKindOfClass:NSNumber.class]) continue;
        NSMutableDictionary *clean = [@{@"x": @(MIN(MAX([d[@"x"] doubleValue], 0.), 1.)), @"y": @(MIN(MAX([d[@"y"] doubleValue], 0.), 1.))} mutableCopy];
        if ([d[@"s"] isKindOfClass:NSNumber.class]) clean[@"s"] = @(MIN(MAX([d[@"s"] doubleValue], 24.), 110.));
        if ([d[@"a"] isKindOfClass:NSNumber.class]) clean[@"a"] = @(MIN(MAX([d[@"a"] doubleValue], .1), 1.));
        out[item] = clean;
    }
    return out;
}
- (void)saveLayout {
    NSData *json = [NSJSONSerialization dataWithJSONObject:layout options:0 error:NULL];
    if (json) [json writeToFile:[self layoutPath] atomically:YES];
}
- (CGPoint)pointOf:(NSDictionary *)d inArea:(CGRect)a {
    return CGPointMake(a.origin.x + [d[@"x"] doubleValue] * a.size.width, a.origin.y + [d[@"y"] doubleValue] * a.size.height);
}
- (void)moveItem:(NSString *)item to:(CGPoint)p {
    CGRect a = self.area;
    NSMutableDictionary *d = layout[item] ?: (layout[item] = [NSMutableDictionary dictionary]);
    d[@"x"] = @((p.x - a.origin.x) / a.size.width);
    d[@"y"] = @((p.y - a.origin.y) / a.size.height);
}
- (void)resetItem:(NSString *)item { [layout removeObjectForKey:item]; [self saveLayout]; }
- (NSMutableDictionary *)seededItem:(NSString *)item {   // the item's saved entry, with its position written in if not yet
    NSMutableDictionary *d = layout[item] ?: (layout[item] = [NSMutableDictionary dictionary]);
    if (!d[@"x"]) { CGPoint at = [self itemCenter:item]; d[@"x"] = @((at.x - self.area.origin.x) / self.area.size.width); d[@"y"] = @((at.y - self.area.origin.y) / self.area.size.height); }
    return d;
}
- (CGFloat)alphaFor:(NSString *)item { CGFloat a = [layout[item][@"a"] doubleValue]; return a ? a : kInkAlpha; }

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
- (CGPoint)restingStickID:(uint64_t)which {
    NSDictionary *d = layout[@(which).stringValue];
    CGRect a = self.area;
    if (d[@"x"]) return [self pointOf:d inArea:a];
    CGFloat inset = self.columnInset + self.stickRadius * 1.9;
    return CGPointMake(which == kStickRightID ? CGRectGetMaxX(a) - inset : CGRectGetMinX(a) + inset,
                       CGRectGetMinY(a) + a.size.height * .80);
}
- (CGFloat)radiusFor:(NSString *)item { CGFloat s = [layout[item][@"s"] doubleValue]; return s ? s : self.stickRadius; }
/* Where an item currently sits, by its id; for the editor (double-tap needs no drag first). */
- (CGPoint)itemCenter:(NSString *)item {
    uint64_t id = item.longLongValue;
    if (id == kStickLeftID || id == kStickRightID) return [self restingStickID:id];
    TouchButton b[kButtonCount]; [self buttons:b];
    for (int i = 0; i < kButtonCount; i++) if (b[i].bit == id) return b[i].at;
    return CGPointZero;
}
- (CGPoint)handleAt:(uint64_t)which {   // the editor's gear / reset, fixed at the top centre (see the file comment)
    CGRect a = self.area;
    CGPoint c = CGPointMake(a.origin.x + a.size.width * .5, a.origin.y + a.size.height * .04);
    return which == kResetID ? CGPointMake(c.x + 64, c.y) : c;
}

/* The default layout, then whatever the player moved or resized over it. */
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
    for (int i = 0; i < kButtonCount; i++) {
        b[i] = all[i];
        NSDictionary *saved = layout[@(b[i].bit).stringValue];
        if (!saved) continue;
        if (saved[@"x"]) b[i].at = [self pointOf:saved inArea:a];
        if (saved[@"s"]) b[i].size = [saved[@"s"] doubleValue];
    }
}

- (uint64_t)buttonAt:(CGPoint)p {
    TouchButton b[kButtonCount]; [self buttons:b]; CGFloat hit = self.hitRadius; uint64_t bits = 0;
    for (int i = 0; i < kButtonCount; i++) if (hypot(p.x - b[i].at.x, p.y - b[i].at.y) < MIN(hit, b[i].size * .75)) bits |= b[i].bit;
    return bits;
}

/* The item the editor would grab: buttons by their box, sticks by their resting ring; generous, the editor is for
   setting the layout by feel. */
- (NSString *)itemAt:(CGPoint)p {
    TouchButton b[kButtonCount]; [self buttons:b];
    for (int i = 0; i < kButtonCount; i++)
        if (hypot(p.x - b[i].at.x, p.y - b[i].at.y) < b[i].size * .75 + 12) return @(b[i].bit).stringValue;
    static const uint64_t sticks[] = {kStickLeftID, kStickRightID};
    for (int i = 0; i < 2; i++) {
        NSString *item = @(sticks[i]).stringValue;
        CGPoint rest = [self restingStickID:sticks[i]];
        if (hypot(p.x - rest.x, p.y - rest.y) <= [self radiusFor:item] + 18) return item;
    }
    return nil;
}

/* The editor's two sliders for the selected item, along the bottom of the layer above the hint: size on the upper
   row, opacity on the lower, both centred under the gear. Each is a track with a knob at the current value. */
static const CGFloat kSliderRowGap = 34;
- (CGRect)sliderTrack:(int)row {   // row 0 = size, row 1 = opacity
    CGRect a = self.area;
    CGFloat y = a.origin.y + a.size.height * .04 + 44 + kSliderRowGap * row;
    return CGRectMake(a.origin.x + a.size.width * .2, y, a.size.width * .6, 0);
}
- (CGFloat)sizeOf:(NSString *)item {   // the item's size in points: its saved size, else the default for its kind
    CGFloat s = [layout[item][@"s"] doubleValue];
    if (s) return s;
    uint64_t id = item.longLongValue;
    return id == kStickLeftID || id == kStickRightID ? self.stickRadius : kButtonSize;
}
- (CGFloat)sliderValue:(int)row {   // the selected item's size (row 0) or opacity (row 1), as 0..1 along the track
    if (row == 2) return (totk_device_motion_sensitivity() * 100 - 10) / (200 - 10);   // the gyro's, in percent 10-200
    if (!selected) return 0;
    if (row == 0) return ([self sizeOf:selected] - 24) / (110 - 24);
    return ([self alphaFor:selected] - .1) / (1 - .1);
}
- (int)sliderAt:(CGPoint)p {   // which slider a touch is on (0 size, 1 opacity, 2 gyro sensitivity), or -1
    for (int row = 2; row >= 0; row--) {
        if (row < 2 && !selected) continue;
        CGRect track = [self sliderTrack:row];
        if (fabs(p.y - track.origin.y) < 24 && p.x > track.origin.x - 16 && p.x < track.origin.x + track.size.width + 16) return row;
    }
    return -1;
}
- (void)setSlider:(int)row toFraction:(CGFloat)f {   // writes the selected item's size or opacity from a 0..1 slider value
    f = MIN(MAX(f, 0), 1);
    if (row == 2) { totk_device_motion_set_sensitivity(10 + f * (200 - 10)); return; }
    if (!selected) return;
    NSMutableDictionary *d = [self seededItem:selected];
    if (row == 0) d[@"s"] = @(MIN(MAX(24 + f * (110 - 24), 24.), 110.));
    else d[@"a"] = @(MIN(MAX(.1 + f * (1 - .1), .1), 1.));
    [self saveLayout];
}

static void stick_axes(CGPoint from, CGPoint to, CGFloat radius, int32_t *x, int32_t *y) {
    CGFloat dx = to.x - from.x, dy = from.y - to.y, dist = hypot(dx, dy);
    if (dist > radius) { dx *= radius / dist; dy *= radius / dist; }
    *x = (int32_t)(dx / radius * 32767); *y = (int32_t)(dy / radius * 32767);
}

- (void)updateState {
    if (editMode) {   // the editor arranges, it does not play: drop anything a finger was holding
        RuntimeGamepadState quiet = {.connected = 1};
        runtime_touch_pad_set(&quiet); [self setNeedsDisplay]; return;
    }
    RuntimeGamepadState s = {.connected = 1};
    for (UITouch *t in points.keyEnumerator) {
        CGPoint p = [points objectForKey:t].CGPointValue; NSValue *left = [leftOrigins objectForKey:t], *right = [rightOrigins objectForKey:t];
        if (left) stick_axes(left.CGPointValue, p, [self radiusFor:@(kStickLeftID).stringValue], &s.lx, &s.ly);
        else if (right) stick_axes(right.CGPointValue, p, [self radiusFor:@(kStickRightID).stringValue], &s.rx, &s.ry);
        else s.buttons |= [self buttonAt:p];
    }
    runtime_touch_pad_set(&s); [self setNeedsDisplay];
}

- (void)holdResetLater:(UITouch *)touch item:(NSString *)item {
    [holdTimer invalidate];
    __weak TOTKTouchPad *weakSelf = self;
    holdTimer = [NSTimer scheduledTimerWithTimeInterval:kHoldReset repeats:NO block:^(NSTimer *timer) {
        (void)timer; TOTKTouchPad *pad = weakSelf; if (!pad) return;
        if ([[pad->drags objectForKey:touch] isEqualToString:item]) { [pad resetItem:item]; [pad->drags removeObjectForKey:touch]; [pad setNeedsDisplay]; }
    }];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event; concealed = NO; [hideTimer invalidate]; hideTimer = nil;
    totk_device_motion_note_touch();   // touching the screen means playing with touch: the phone's gyro takes the aiming (device_motion.m)
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        if (hypot(p.x - [self handleAt:kEditID].x, p.y - [self handleAt:kEditID].y) < 30) {   // the gear opens and closes the editor
            editMode = !editMode;
            [points removeAllObjects]; [leftOrigins removeAllObjects]; [rightOrigins removeAllObjects];
            [drags removeAllObjects]; [dragOrigins removeAllObjects];
            [self updateState]; [self setNeedsDisplay];
            continue;
        }
        if (editMode) {
            if (hypot(p.x - [self handleAt:kResetID].x, p.y - [self handleAt:kResetID].y) < 30) { [layout removeAllObjects]; [self saveLayout]; selected = nil; [self setNeedsDisplay]; continue; }
            int slider = [self sliderAt:p];
            if (slider >= 0) { [sliderTouches setObject:@(slider) forKey:t]; [self setSlider:slider toFraction:(p.x - [self sliderTrack:slider].origin.x) / [self sliderTrack:slider].size.width]; [self setNeedsDisplay]; continue; }
            NSString *item = [self itemAt:p];
            if (!item) { selected = nil; [self setNeedsDisplay]; continue; }   // a touch on empty space clears the selection
            [drags setObject:item forKey:t]; [dragOrigins setObject:[NSValue valueWithCGPoint:p] forKey:t];
            selected = item;
            [self holdResetLater:t item:item];
            [self setNeedsDisplay];
            continue;
        }
        NSValue *v = [NSValue valueWithCGPoint:p]; [points setObject:v forKey:t];
        if ([self buttonAt:p]) continue;
        [(p.x < CGRectGetMidX(self.bounds) ? leftOrigins : rightOrigins) setObject:v forKey:t];
    }
    [self updateState];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    for (UITouch *t in touches) {
        CGPoint p = [t locationInView:self];
        NSNumber *slider = [sliderTouches objectForKey:t];
        if (slider) { CGRect track = [self sliderTrack:slider.intValue]; [self setSlider:slider.intValue toFraction:(p.x - track.origin.x) / track.size.width]; [self setNeedsDisplay]; continue; }
        NSString *item = [drags objectForKey:t];
        if (item) {
            NSValue *origin = [dragOrigins objectForKey:t];
            if (origin && hypot(p.x - origin.CGPointValue.x, p.y - origin.CGPointValue.y) > 10) { [holdTimer invalidate]; holdTimer = nil; }
            [self moveItem:item to:p];
        } else if ([points objectForKey:t]) [points setObject:[NSValue valueWithCGPoint:p] forKey:t];
    }
    [self updateState];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    BOOL moved = NO;
    for (UITouch *t in touches) {
        if ([drags objectForKey:t]) { [drags removeObjectForKey:t]; moved = YES; }
        [sliderTouches removeObjectForKey:t];
        [dragOrigins removeObjectForKey:t];
        [points removeObjectForKey:t]; [leftOrigins removeObjectForKey:t]; [rightOrigins removeObjectForKey:t];
    }
    if (!drags.count) { [holdTimer invalidate]; holdTimer = nil; }
    if (moved) [self saveLayout];
    [self updateState]; [self hideLater];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { [self touchesEnded:touches withEvent:event]; }
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsDisplay]; }

static void draw_stick(CGContextRef c, CGPoint o, CGPoint p, CGFloat r, CGFloat alpha) {
    int32_t x, y; stick_axes(o, p, r, &x, &y);
    UIColor *ink = [UIColor colorWithWhite:1 alpha:alpha];
    CGContextSetStrokeColorWithColor(c, ink.CGColor); CGContextSetFillColorWithColor(c, ink.CGColor);
    CGContextStrokeEllipseInRect(c, CGRectMake(o.x - r, o.y - r, 2 * r, 2 * r));
    CGPoint k = CGPointMake(o.x + x / 32767.0 * r, o.y - y / 32767.0 * r);
    CGRect knob = CGRectMake(k.x - kButtonSize / 2, k.y - kButtonSize / 2, kButtonSize, kButtonSize);
    CGContextFillEllipseInRect(c, knob); CGContextStrokeEllipseInRect(c, knob);
}

- (void)drawRect:(CGRect)rect {
    if (concealed) return;
    (void)rect; CGContextRef c = UIGraphicsGetCurrentContext();
    // Each button is set with its own colour before its fill: drawing a label leaves the context's colour at the label's.
    UIColor *ink = [UIColor colorWithWhite:1 alpha:editMode ? .85 : kInkAlpha];
    CGContextSetLineWidth(c, 2);
    NSString *leftKey = @(kStickLeftID).stringValue, *rightKey = @(kStickRightID).stringValue;
    CGFloat rl = [self radiusFor:leftKey], rr = [self radiusFor:rightKey];
    // Each stick is drawn where its finger went down while it is held. Its resting ring shows only in the editor, for placing it.
    CGFloat stickAlpha = editMode ? .85 : kInkAlpha;
    if (editMode && !leftOrigins.count) { CGPoint rest = [self restingStickID:kStickLeftID]; draw_stick(c, rest, rest, rl, stickAlpha); }
    if (editMode && !rightOrigins.count) { CGPoint rest = [self restingStickID:kStickRightID]; draw_stick(c, rest, rest, rr, stickAlpha); }
    for (UITouch *t in leftOrigins.keyEnumerator) draw_stick(c, [leftOrigins objectForKey:t].CGPointValue, [points objectForKey:t].CGPointValue, rl, stickAlpha);
    for (UITouch *t in rightOrigins.keyEnumerator) draw_stick(c, [rightOrigins objectForKey:t].CGPointValue, [points objectForKey:t].CGPointValue, rr, stickAlpha);
    TouchButton b[kButtonCount]; [self buttons:b];
    for (int i = 0; i < kButtonCount; i++) {
        // The play layer and the editor show the same opacity, so the slider matches what the player sees.
        CGFloat alpha = [self alphaFor:@(b[i].bit).stringValue];
        UIColor *itemInk = [UIColor colorWithWhite:1 alpha:alpha];
        NSDictionary *attrs = @{NSFontAttributeName: [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold], NSForegroundColorAttributeName: itemInk};
        CGFloat size = b[i].size; CGRect box = CGRectMake(b[i].at.x - size / 2, b[i].at.y - size / 2, size, size);
        CGContextSetFillColorWithColor(c, itemInk.CGColor); CGContextSetStrokeColorWithColor(c, itemInk.CGColor);
        CGContextFillEllipseInRect(c, box); CGContextStrokeEllipseInRect(c, box);
        CGSize z = [b[i].label sizeWithAttributes:attrs]; [b[i].label drawAtPoint:CGPointMake(b[i].at.x - z.width / 2, b[i].at.y - z.height / 2) withAttributes:attrs];
    }
    // The gear is always on (small, out of the play area): it is the way into the editor. The rest is editor chrome.
    NSString *glyph = @"⚙";
    NSDictionary *small = @{NSFontAttributeName: [UIFont systemFontOfSize:editMode ? 20 : 14], NSForegroundColorAttributeName: ink};
    CGPoint gear = [self handleAt:kEditID]; CGSize gz = [glyph sizeWithAttributes:small];
    [glyph drawAtPoint:CGPointMake(gear.x - gz.width / 2, gear.y - gz.height / 2) withAttributes:small];
    if (!editMode) return;
    NSString *resetGlyph = @"↺"; CGPoint rAt = [self handleAt:kResetID];
    CGSize rz = [resetGlyph sizeWithAttributes:small]; [resetGlyph drawAtPoint:CGPointMake(rAt.x - rz.width / 2, rAt.y - rz.height / 2) withAttributes:small];
    // Ring around whatever fingers are holding, and the how-to under the hint's own alpha.
    for (NSString *item in drags.objectEnumerator) {
        CGPoint at = [self itemCenter:item];
        CGFloat rad = item.longLongValue == kStickLeftID || item.longLongValue == kStickRightID ? [self radiusFor:item] : 0;
        if (!rad) { for (int i = 0; i < kButtonCount; i++) if (b[i].bit == (uint64_t)item.longLongValue) rad = b[i].size * .75 + 8; }
        if (rad) CGContextStrokeEllipseInRect(c, CGRectMake(at.x - rad, at.y - rad, 2 * rad, 2 * rad));
    }
    // The selected item's ring, and its two sliders (size, opacity) above the hint.
    if (selected) {
        CGPoint at = [self itemCenter:selected];
        CGFloat rad = selected.longLongValue == kStickLeftID || selected.longLongValue == kStickRightID ? [self radiusFor:selected] : 0;
        if (!rad) for (int i = 0; i < kButtonCount; i++) if (b[i].bit == (uint64_t)selected.longLongValue) rad = b[i].size * .75 + 8;
        CGContextSetStrokeColorWithColor(c, [UIColor colorWithWhite:1 alpha:.9].CGColor);
        if (rad) CGContextStrokeEllipseInRect(c, CGRectMake(at.x - rad, at.y - rad, 2 * rad, 2 * rad));
    }
    // The sliders: size and opacity for the selected item (rows 0, 1), and the gyro's sensitivity (row 2) always.
    for (int row = 0; row < 3; row++) {
        if (row < 2 && !selected) continue;
        CGRect track = [self sliderTrack:row];
        CGContextSetStrokeColorWithColor(c, [UIColor colorWithWhite:1 alpha:.6].CGColor); CGContextSetLineWidth(c, 2);
        CGContextMoveToPoint(c, track.origin.x, track.origin.y); CGContextAddLineToPoint(c, track.origin.x + track.size.width, track.origin.y); CGContextStrokePath(c);
        CGFloat f = MIN(MAX([self sliderValue:row], 0), 1);
        CGPoint knob = CGPointMake(track.origin.x + f * track.size.width, track.origin.y);
        CGContextSetFillColorWithColor(c, [UIColor colorWithWhite:1 alpha:.9].CGColor);
        CGContextFillEllipseInRect(c, CGRectMake(knob.x - 11, knob.y - 11, 22, 22));
        NSDictionary *lab = @{NSFontAttributeName: [UIFont systemFontOfSize:12], NSForegroundColorAttributeName: [UIColor colorWithWhite:1 alpha:.85]};
        NSString *label;
        if (row == 2) label = [NSString stringWithFormat:@"gyro sensitivity · %.0f%%", totk_device_motion_sensitivity() * 100];
        else {
            NSString *value = row == 0 ? [NSString stringWithFormat:@"size %.0f pt", [self sizeOf:selected]]
                                       : [NSString stringWithFormat:@"opacity %.0f%%", [self alphaFor:selected] * 100];
            label = [NSString stringWithFormat:@"%@ · %@", row == 0 ? @"size" : @"opacity", value];
        }
        [label drawAtPoint:CGPointMake(track.origin.x, track.origin.y - 22) withAttributes:lab];
    }
    NSString *hint = @"drag = move · tap = select · slider = size & opacity · hold = reset";
    NSDictionary *tiny = @{NSFontAttributeName: [UIFont systemFontOfSize:13], NSForegroundColorAttributeName: [UIColor colorWithWhite:1 alpha:.85]};
    CGSize hz = [hint sizeWithAttributes:tiny]; CGRect a = self.area;
    [hint drawAtPoint:CGPointMake(a.origin.x + (a.size.width - hz.width) / 2, a.origin.y + a.size.height - hz.height - 6) withAttributes:tiny];
}
@end
