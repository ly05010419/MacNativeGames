// The views of the Home button's quit prompt (home_quit.h), drawn over the game's view by front_mac.m: AppKit views, so they draw while the game runs, and built here
// alone so tests/t_home_quit_mac.m can build and draw them without a window or a controller. The look follows NX2Apple's console mode: a dark surface and a cream
// accent, the primary action as a cream pill with the controller's own button glyph. Sizes follow the view's height, so the prompt reads from a couch on a TV and
// stays in proportion in a small window.
#pragma once
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

static NSColor *hq_color(double r, double g, double b, double a) { return [NSColor colorWithSRGBRed:r green:g blue:b alpha:a]; }
#define HQ_BACKGROUND hq_color(0.055, 0.055, 0.06, 1)
#define HQ_SURFACE hq_color(0.10, 0.10, 0.11, 1)
#define HQ_CREAM hq_color(1, 0.94, 0.84, 1)

// Points per point of a 720-high view, between 1 and 2
static CGFloat hq_scale(NSSize s) { CGFloat k = s.height / 720; return k < 1 ? 1 : k > 2 ? 2 : k; }

static NSTextField *hq_label(NSString *text, CGFloat size, NSFontWeight w, NSColor *c, int wrap) {
    NSTextField *t = wrap ? [NSTextField wrappingLabelWithString:text] : [NSTextField labelWithString:text];
    t.font = [NSFont systemFontOfSize:size weight:w]; t.textColor = c; t.alignment = NSTextAlignmentCenter;
    t.translatesAutoresizingMaskIntoConstraints = NO;
    return t;
}
// A controller button's glyph: the symbol the controller reports for it, else a plain letter in a circle
static NSImageView *hq_glyph(NSString *symbol, NSString *fallback, CGFloat size, NSColor *c) {
    NSImage *img = symbol ? [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil] : nil;
    if (!img) img = [NSImage imageWithSystemSymbolName:fallback accessibilityDescription:nil];
    img = [img imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPointSize:size weight:NSFontWeightMedium]];
    NSImageView *v = [NSImageView imageViewWithImage:img ?: [NSImage new]];
    v.contentTintColor = c; v.translatesAutoresizingMaskIntoConstraints = NO;
    return v;
}
// Every action is this wide (points at a 720-high view), whatever its label and language: a label of about ten letters ("Abbrechen", "Annuler") fits at
// full size, and a longer one gets a smaller font, then is cut, so the pills never change width
#define HQ_ACTION_W 220
// The font size that fits text into room on one line: size, or smaller down to 70 % of it
static CGFloat hq_fit(NSString *text, CGFloat size, CGFloat room) {
    CGFloat w = [text sizeWithAttributes:@{NSFontAttributeName: [NSFont systemFontOfSize:size weight:NSFontWeightSemibold]}].width;
    if (w <= room) return size;
    CGFloat s = floor(size * room / w * 10) / 10;
    return s < size * 0.7 ? size * 0.7 : s;
}
// One action of the prompt: glyph and label centred in a pill of a fixed size, the primary one cream
static NSView *hq_action(NSString *symbol, NSString *fallback, NSString *label, int primary, CGFloat k) {
    NSColor *fg = primary ? HQ_BACKGROUND : [NSColor colorWithWhite:1 alpha:0.8];
    CGFloat w = HQ_ACTION_W * k, glyph = 24 * k, gap = 10 * k, inset = 20 * k, room = w - 2 * inset - glyph - gap;
    NSTextField *t = hq_label(label, hq_fit(label, 22 * k, room), NSFontWeightSemibold, fg, 0);
    t.lineBreakMode = NSLineBreakByTruncatingTail;
    [t setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *row = [NSStackView stackViewWithViews:@[hq_glyph(symbol, fallback, glyph, fg), t]];
    row.spacing = gap; row.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *pill = [NSView new]; pill.translatesAutoresizingMaskIntoConstraints = NO;
    pill.wantsLayer = YES; pill.layer.cornerRadius = 28 * k;
    if (primary) pill.layer.backgroundColor = HQ_CREAM.CGColor;
    [pill addSubview:row];
    [NSLayoutConstraint activateConstraints:@[[pill.widthAnchor constraintEqualToConstant:w], [pill.heightAnchor constraintEqualToConstant:56 * k],
        [row.centerXAnchor constraintEqualToAnchor:pill.centerXAnchor], [row.centerYAnchor constraintEqualToAnchor:pill.centerYAnchor],
        [row.widthAnchor constraintLessThanOrEqualToConstant:w - 2 * inset]]];
    pill.accessibilityElement = YES; pill.accessibilityRole = NSAccessibilityButtonRole; pill.accessibilityLabel = label;
    return pill;
}

// The prompt's scrim keeps the mouse: a view passes the clicks it does not take on to its superview, the game's view, which would make them touches while the
// prompt has the game out of focus
@interface HqScrim : NSView @end
@implementation HqScrim
- (void)mouseDown:(NSEvent *)e { (void)e; }
- (void)mouseDragged:(NSEvent *)e { (void)e; }
- (void)mouseUp:(NSEvent *)e { (void)e; }
- (void)rightMouseDown:(NSEvent *)e { (void)e; }
- (void)otherMouseDown:(NSEvent *)e { (void)e; }
@end

// The prompt over the whole view: a dim scrim, and in its middle the card with the title, the note and the two actions (A quits, B cancels).
// a_symbol, b_symbol: the controller's own glyphs (GCControllerButtonInput.sfSymbolsName), nil for the plain ones
static NSView *hq_prompt_view(NSRect bounds, NSString *title, NSString *note, NSString *quit, NSString *cancel, NSString *a_symbol, NSString *b_symbol) {
    CGFloat k = hq_scale(bounds.size);
    NSView *scrim = [[HqScrim alloc] initWithFrame:bounds]; scrim.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scrim.wantsLayer = YES; scrim.layer.backgroundColor = [NSColor colorWithWhite:0 alpha:0.6].CGColor;
    NSTextField *t = hq_label(title, 34 * k, NSFontWeightBold, NSColor.whiteColor, 1), *n = hq_label(note, 20 * k, NSFontWeightRegular, [NSColor colorWithWhite:1 alpha:0.6], 1);
    NSStackView *actions = [NSStackView stackViewWithViews:@[hq_action(a_symbol, @"a.circle", quit, 1, k), hq_action(b_symbol, @"b.circle", cancel, 0, k)]];
    actions.spacing = 16 * k; actions.translatesAutoresizingMaskIntoConstraints = NO;
    NSStackView *card = [NSStackView stackViewWithViews:@[t, n, actions]];
    card.orientation = NSUserInterfaceLayoutOrientationVertical; card.alignment = NSLayoutAttributeCenterX;
    card.spacing = 14 * k; [card setCustomSpacing:32 * k afterView:n];
    card.edgeInsets = NSEdgeInsetsMake(40 * k, 48 * k, 36 * k, 48 * k); card.translatesAutoresizingMaskIntoConstraints = NO;
    card.wantsLayer = YES; card.layer.backgroundColor = HQ_SURFACE.CGColor; card.layer.cornerRadius = 28 * k;
    card.layer.borderWidth = 1; card.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.10].CGColor;
    [scrim addSubview:card];
    CGFloat w = 600 * k;
    [NSLayoutConstraint activateConstraints:@[[card.centerXAnchor constraintEqualToAnchor:scrim.centerXAnchor], [card.centerYAnchor constraintEqualToAnchor:scrim.centerYAnchor],
        [card.widthAnchor constraintEqualToConstant:w], [t.widthAnchor constraintLessThanOrEqualToConstant:w - 96 * k], [n.widthAnchor constraintLessThanOrEqualToConstant:w - 96 * k]]];
    scrim.accessibilityElement = YES; scrim.accessibilityRole = NSAccessibilityGroupRole; scrim.accessibilityLabel = title;
    return scrim;
}

// The hold indicator at the top of the view: a pill with a ring that fills while Home is held, and its label. hq_hold_set moves the ring
static NSView *hq_hold_view(NSRect bounds, NSString *label, CAShapeLayer **ring_out) {
    CGFloat k = hq_scale(bounds.size), d = 26 * k, line = 3.5 * k;
    NSView *ringView = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, d, d)]; ringView.wantsLayer = YES; ringView.translatesAutoresizingMaskIntoConstraints = NO;
    CGPathRef circle = CGPathCreateWithEllipseInRect(CGRectInset(CGRectMake(0, 0, d, d), line / 2, line / 2), NULL);
    CAShapeLayer *track = [CAShapeLayer layer], *ring = [CAShapeLayer layer];
    for (CAShapeLayer *l in @[track, ring]) { l.frame = CGRectMake(0, 0, d, d); l.path = circle; l.fillColor = NSColor.clearColor.CGColor; l.lineWidth = line; [ringView.layer addSublayer:l]; }
    CGPathRelease(circle);
    track.strokeColor = [NSColor colorWithWhite:1 alpha:0.18].CGColor;
    ring.strokeColor = HQ_CREAM.CGColor; ring.lineCap = kCALineCapRound; ring.strokeEnd = 0;
    ring.transform = CATransform3DConcat(CATransform3DMakeRotation(M_PI_2, 0, 0, 1), CATransform3DMakeScale(-1, 1, 1));   // from the top, clockwise
    [NSLayoutConstraint activateConstraints:@[[ringView.widthAnchor constraintEqualToConstant:d], [ringView.heightAnchor constraintEqualToConstant:d]]];
    NSStackView *pill = [NSStackView stackViewWithViews:@[ringView, hq_label(label, 20 * k, NSFontWeightSemibold, [NSColor colorWithWhite:1 alpha:0.9], 0)]];
    pill.spacing = 12 * k; pill.edgeInsets = NSEdgeInsetsMake(0, 18 * k, 0, 24 * k); pill.translatesAutoresizingMaskIntoConstraints = NO;
    pill.wantsLayer = YES; pill.layer.backgroundColor = [HQ_BACKGROUND colorWithAlphaComponent:0.92].CGColor; pill.layer.cornerRadius = 28 * k;
    pill.layer.borderWidth = 1; pill.layer.borderColor = [NSColor colorWithWhite:1 alpha:0.12].CGColor;
    NSView *host = [[NSView alloc] initWithFrame:bounds]; host.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [host addSubview:pill];
    [NSLayoutConstraint activateConstraints:@[[pill.centerXAnchor constraintEqualToAnchor:host.centerXAnchor], [pill.topAnchor constraintEqualToAnchor:host.topAnchor constant:40 * k],
        [pill.heightAnchor constraintEqualToConstant:56 * k]]];
    *ring_out = ring;
    return host;
}
static void hq_hold_set(CAShapeLayer *ring, double fill) {
    [CATransaction begin]; [CATransaction setDisableActions:YES]; ring.strokeEnd = fill; [CATransaction commit];
}
