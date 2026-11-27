/* zeldaVR, Ocarina of Time: the wrist panel's dungeon row (vr.md row 83): the small keys with their count, the boss key, the
   map and the compass, each only where the dungeon has one, dimmed until found (HookshotVR's idea, camera-ui.md 6.1-6.2; the
   pictures are ours, drawn with CoreGraphics). The values: zeldaVR/runtime/oot_vr_dungeon.h, read by the game's side
   (games/oot/runtime/oot_vr.h runtime_oot_vr_dungeon). Included by oot_vr_wrist_draw.h. HookshotVR's full mode (a minimap from the
   game's map pictures) is not done: it needs pictures from the game's files. */
#include "../runtime/oot_vr_dungeon.h"
extern int runtime_oot_vr_dungeon(oot_dungeon_row *out) __attribute__((weak_import));
enum { OOT_WRIST_DUNGEON_H = 60 };
static void wrist_text(CGContextRef c, NSString *text, CGFloat size, CGFloat x, CGFloat y);   /* oot_vr_wrist_draw.h */
static oot_dungeon_row g_wrist_dungeon = { -1, 0, 0, 0 }, g_wrist_dungeon_drawn = { -1, 0, 0, 0 };

static void wrist_key_path(CGContextRef c, CGFloat x, CGFloat y, CGFloat s) {   /* a key lying along x, its bow at the left; s: its height */
    CGContextBeginPath(c);
    CGContextAddEllipseInRect(c, CGRectMake(x, y, s, s));
    CGContextAddRect(c, CGRectMake(x + s * 0.9, y + s * 0.38, s * 1.3, s * 0.24));
    CGContextAddRect(c, CGRectMake(x + s * 1.75, y + s * 0.1, s * 0.18, s * 0.3));
    CGContextAddRect(c, CGRectMake(x + s * 2.0, y + s * 0.1, s * 0.18, s * 0.3));
}
/* The row with its lower left at (left, y), in the panel's own colours. */
static void wrist_dungeon_draw(CGContextRef c, const oot_dungeon_row *d, CGFloat left, CGFloat y) {
    if (d->dungeon < 0) return;
    CGFloat x = left + 4;
    if (d->has & OOT_DG_KEYS) {
        wrist_key_path(c, x, y + 6, 28); CGContextSetRGBFillColor(c, 0.80, 0.78, 0.72, d->keys ? 1 : 0.30); CGContextFillPath(c);
        CGContextSetRGBFillColor(c, 0.96, 0.92, 0.82, 1);
        wrist_text(c, [NSString stringWithFormat:@"×%d", d->keys], 34, x + 70, y + 8);
        x += 150;
    }
    if (d->has & OOT_DG_BOSS) {
        const int got = (d->owned & OOT_DG_BOSS) != 0;
        wrist_key_path(c, x, y + 2, 36); CGContextSetRGBFillColor(c, 0.90, 0.30, 0.22, got ? 1 : 0.25); CGContextFillPath(c);
        x += 106;
    }
    if (d->has & OOT_DG_MAP) {
        const int got = (d->owned & OOT_DG_MAP) != 0; const CGFloat a = got ? 1 : 0.25;
        CGContextSetRGBFillColor(c, 0.88, 0.80, 0.58, a); CGContextFillRect(c, CGRectMake(x, y + 4, 48, 36));
        CGContextSetRGBStrokeColor(c, 0.45, 0.32, 0.18, a); CGContextSetLineWidth(c, 3);
        CGContextBeginPath(c); CGContextMoveToPoint(c, x + 16, y + 4); CGContextAddLineToPoint(c, x + 16, y + 40); CGContextMoveToPoint(c, x + 32, y + 4); CGContextAddLineToPoint(c, x + 32, y + 40); CGContextStrokePath(c);
        x += 70;
    }
    if (d->has & OOT_DG_COMPASS) {
        const int got = (d->owned & OOT_DG_COMPASS) != 0; const CGFloat a = got ? 1 : 0.25;
        CGContextSetRGBFillColor(c, 0.80, 0.78, 0.72, a); CGContextFillEllipseInRect(c, CGRectMake(x, y + 2, 40, 40));
        CGContextSetRGBFillColor(c, 0.86, 0.22, 0.20, a);
        CGContextBeginPath(c); CGContextMoveToPoint(c, x + 20, y + 38); CGContextAddLineToPoint(c, x + 26, y + 22); CGContextAddLineToPoint(c, x + 14, y + 22); CGContextClosePath(c); CGContextFillPath(c);
        CGContextSetRGBFillColor(c, 0.20, 0.20, 0.24, a);
        CGContextBeginPath(c); CGContextMoveToPoint(c, x + 20, y + 6); CGContextAddLineToPoint(c, x + 26, y + 22); CGContextAddLineToPoint(c, x + 14, y + 22); CGContextClosePath(c); CGContextFillPath(c);
    }
}
