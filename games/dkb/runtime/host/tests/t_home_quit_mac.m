// The views of the Home button's quit prompt (home_quit_mac.h), built without a game, a window or a controller: the texts are shown, the card stays inside a small
// window and a TV-sized one, a long title wraps instead of widening the card, and a glyph the system does not have falls back to the plain letter.
// DM_HOME_QUIT_CAPTURE=<folder> also draws both views over a stand-in picture into PNG files there (the pull request's screenshots), in an offscreen window.
#import "../home_quit_mac.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void texts(NSView *v, NSMutableArray<NSString *> *out) {
    if ([v isKindOfClass:NSTextField.class]) [out addObject:((NSTextField *)v).stringValue];
    for (NSView *s in v.subviews) texts(s, out);
}
static NSView *card_of(NSView *scrim) { return scrim.subviews.firstObject; }
static NSImageView *first_image(NSView *v) {
    if ([v isKindOfClass:NSImageView.class]) return (NSImageView *)v;
    for (NSView *s in v.subviews) { NSImageView *i = first_image(s); if (i) return i; }
    return nil;
}

// A stand-in for the game's picture, so the scrim and the card are seen over something
@interface Backdrop : NSView @end
@implementation Backdrop
- (void)drawRect:(NSRect)r {
    (void)r;
    NSGradient *g = [[NSGradient alloc] initWithColors:@[hq_color(0.20, 0.42, 0.62, 1), hq_color(0.42, 0.62, 0.30, 1), hq_color(0.86, 0.74, 0.42, 1)]];
    [g drawInRect:self.bounds angle:-70];
}
@end

// The game's view behind the prompt: counts the clicks that reach it
@interface Clicks : NSView { @public int clicks; } @end
@implementation Clicks
- (void)mouseDown:(NSEvent *)e { (void)e; clicks++; }
- (void)mouseDragged:(NSEvent *)e { (void)e; clicks++; }
- (void)mouseUp:(NSEvent *)e { (void)e; clicks++; }
@end

static void capture(NSView *overlay, NSSize size, const char *dir, const char *name) {
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(-100000, -100000, size.width, size.height) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    Backdrop *b = [[Backdrop alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)]; b.wantsLayer = YES;
    w.contentView = b; overlay.frame = b.bounds; [b addSubview:overlay];
    [b layoutSubtreeIfNeeded]; [b displayIfNeeded];
    NSBitmapImageRep *rep = [b bitmapImageRepForCachingDisplayInRect:b.bounds];
    if (!rep) { fprintf(stderr, "t_home_quit_mac: no bitmap for %s\n", name); [overlay removeFromSuperview]; return; }
    [b cacheDisplayInRect:b.bounds toBitmapImageRep:rep];
    NSString *path = [NSString stringWithFormat:@"%s/%s", dir, name];
    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
    printf("t_home_quit_mac: wrote %s\n", path.UTF8String);
    [overlay removeFromSuperview];
}

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSRect small = NSMakeRect(0, 0, 1280, 720), tv = NSMakeRect(0, 0, 1920, 1080);
        NSView *p = hq_prompt_view(small, @"Quit Super Mario Odyssey?", @"Unsaved progress will be lost.", @"Quit", @"Cancel", @"xmark.circle", @"circle.circle");
        [p layoutSubtreeIfNeeded];
        NSMutableArray<NSString *> *t = [NSMutableArray new]; texts(p, t);
        CHECK([t containsObject:@"Quit Super Mario Odyssey?"] && [t containsObject:@"Unsaved progress will be lost."] && [t containsObject:@"Quit"] && [t containsObject:@"Cancel"], "the prompt shows its four texts");
        CHECK(NSContainsRect(p.bounds, card_of(p).frame), "the card is inside a 1280x720 window");
        CHECK(NSEqualPoints(NSMakePoint(NSMidX(card_of(p).frame), NSMidY(card_of(p).frame)), NSMakePoint(640, 360)), "the card is in the middle");
        CGFloat w720 = card_of(p).frame.size.width;

        NSView *ptv = hq_prompt_view(tv, @"Quit Super Mario Odyssey?", @"Unsaved progress will be lost.", @"Quit", @"Cancel", nil, nil);
        [ptv layoutSubtreeIfNeeded];
        CHECK(NSContainsRect(ptv.bounds, card_of(ptv).frame) && card_of(ptv).frame.size.width > w720 * 1.4, "on a 1080p screen the card is larger and still inside");
        CHECK(first_image(ptv).image.size.width > 0, "no symbol from the controller: the plain letter glyph");

        NSView *plong = hq_prompt_view(small, @"Quit The Legend of Zelda: Tears of the Kingdom and every other very long name?", @"n", @"Quit", @"Cancel", @"no.such.symbol", nil);
        [plong layoutSubtreeIfNeeded];
        CHECK(card_of(plong).frame.size.width == w720 && NSContainsRect(plong.bounds, card_of(plong).frame), "a long title wraps, the card keeps its width");
        CHECK(first_image(plong).image.size.width > 0, "a symbol the system does not have falls back to the letter");

        // The two actions keep one width whatever a front end passes (DM_HOME_QUIT_TEXT): short labels, Chinese ones ("quit", "cancel"), labels of about ten
        // letters, and a label far longer than any of them, which shrinks or is cut instead of widening its pill
        NSArray<NSArray<NSString *> *> *labels = @[@[@"Quit", @"Cancel"], @[@"\u9000\u51fa", @"\u53d6\u6d88"], @[@"Quit game", @"Keep playing"],
                                                  @[@"Quit this game right now please", @"Cancel and go back to the game"]];
        for (NSValue *size in @[[NSValue valueWithRect:small], [NSValue valueWithRect:tv]]) {
            NSRect r = size.rectValue; CGFloat want = HQ_ACTION_W * hq_scale(r.size);
            for (NSArray<NSString *> *l in labels) {
                NSView *pv = hq_prompt_view(r, @"Quit Hades?", @"n", l[0], l[1], nil, nil);
                [pv layoutSubtreeIfNeeded];
                NSStackView *actions = (NSStackView *)((NSStackView *)card_of(pv)).arrangedSubviews.lastObject;
                CHECK(actions.arrangedSubviews.count == 2, "two actions");
                for (NSView *a in actions.arrangedSubviews) CHECK(fabs(a.frame.size.width - want) < 0.5 && fabs(a.frame.size.height - 56 * hq_scale(r.size)) < 0.5, l[0].UTF8String);
                for (NSView *a in actions.arrangedSubviews) {
                    NSStackView *row = (NSStackView *)a.subviews.firstObject;
                    CHECK(NSContainsRect(a.bounds, row.frame), "the glyph and the label stay inside their pill");
                }
            }
        }

        // A click on the open prompt stays there: the game's view behind it (here a stand-in that counts clicks) gets none, so it makes no touch
        Clicks *game = [[Clicks alloc] initWithFrame:small]; NSView *pc = hq_prompt_view(small, @"Quit Hades?", @"n", @"Quit", @"Cancel", nil, nil);
        [game addSubview:pc]; [pc layoutSubtreeIfNeeded];
        for (int at = 0; at < 2; at++) {   // the dim edge, then the card's middle (the card, a label)
            NSView *hit = [pc hitTest:at ? NSMakePoint(640, 360) : NSMakePoint(10, 10)] ?: pc;
            NSEvent *e = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:NSMakePoint(10, 10) modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:1];
            CHECK(e != nil, "a mouse event is made"); if (!e) break;
            [hit mouseDown:e]; [hit mouseDragged:e]; [hit mouseUp:e];
        }
        CHECK(game->clicks == 0, "a click on the prompt does not reach the game's view");

        CAShapeLayer *ring = nil;
        NSView *h = hq_hold_view(small, @"Keep holding to quit", &ring);
        [h layoutSubtreeIfNeeded];
        NSMutableArray<NSString *> *ht = [NSMutableArray new]; texts(h, ht);
        CHECK([ht containsObject:@"Keep holding to quit"], "the hold indicator has its label");
        NSView *pill = h.subviews.firstObject;
        CHECK(NSMaxY(pill.frame) < 720 && NSMaxY(pill.frame) > 600 && fabs(NSMidX(pill.frame) - 640) < 1, "the indicator is at the top, in the middle");
        CHECK(ring && ring.strokeEnd == 0, "the ring starts empty");
        hq_hold_set(ring, 0.6);
        CHECK(fabs(ring.strokeEnd - 0.6) < 1e-6, "the ring follows the hold");

        const char *dir = getenv("DM_HOME_QUIT_CAPTURE");
        if (dir && *dir) {
            capture(hq_prompt_view(tv, @"Quit Super Mario Odyssey?", @"Unsaved progress will be lost.", @"Quit", @"Cancel", @"a.circle", @"b.circle"), tv.size, dir, "home-quit-prompt.png");
            CAShapeLayer *r = nil; NSView *hv = hq_hold_view(tv, @"Keep holding to quit", &r); hq_hold_set(r, 0.6);
            capture(hv, tv.size, dir, "home-quit-hold.png");
            capture(hq_prompt_view(tv, @"Quit Super Mario Odyssey?", @"Unsaved progress will be lost.", @"Quit", @"Cancel", @"xmark.circle", @"circle.circle"), tv.size, dir, "home-quit-prompt-ps.png");
        }
    }
    printf("t_home_quit_mac: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
