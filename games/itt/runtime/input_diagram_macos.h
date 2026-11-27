/* Original vector diagram; no downloaded assets or emulator artwork. */
#include "keyboard_macos.h"
@interface MNGInputBackground : NSView
@end
@implementation MNGInputBackground
- (void)drawRect:(NSRect)dirty { (void)dirty;[NSColor.windowBackgroundColor setFill];NSRectFill(self.bounds); }
@end
@interface MNGInputDiagram : NSView
@property int highlighted;
@property(copy) void (^selected)(int target);
@end
@implementation MNGInputDiagram
- (void)mouseDown:(NSEvent *)event {
    NSPoint at=[self convertPoint:event.locationInWindow fromView:nil];
    const NSPoint points[]={{337,188},{310,160},{310,216},{282,188},{88,193},{253,112},{69,247},{327,247},{69,280},{327,280},{229,214},{169,214},{119,123},{143,147},{167,123},{143,99}};
    int nearest=-1;CGFloat distance=900;
    for(int i=0;i<16;i++){CGFloat dx=at.x-points[i].x,dy=at.y-points[i].y,d=dx*dx+dy*dy;if(d<distance){nearest=i;distance=d;}}
    if(nearest>=0){self.highlighted=nearest;self.needsDisplay=YES;if(self.selected)self.selected(nearest);}
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [NSColor.windowBackgroundColor setFill]; NSRectFill(self.bounds);
    NSBezierPath *body = [NSBezierPath bezierPath];
    [body moveToPoint:NSMakePoint(52,228)];
    [body curveToPoint:NSMakePoint(344,228) controlPoint1:NSMakePoint(115,265) controlPoint2:NSMakePoint(285,265)];
    [body curveToPoint:NSMakePoint(386,66) controlPoint1:NSMakePoint(376,207) controlPoint2:NSMakePoint(408,78)];
    [body curveToPoint:NSMakePoint(302,96) controlPoint1:NSMakePoint(361,38) controlPoint2:NSMakePoint(320,48)];
    [body lineToPoint:NSMakePoint(276,133)]; [body lineToPoint:NSMakePoint(120,133)];
    [body curveToPoint:NSMakePoint(30,66) controlPoint1:NSMakePoint(72,48) controlPoint2:NSMakePoint(55,37)];
    [body curveToPoint:NSMakePoint(52,228) controlPoint1:NSMakePoint(6,82) controlPoint2:NSMakePoint(27,200)]; [body closePath];
    [NSColor.controlColor setFill]; [body fill]; [NSColor.separatorColor setStroke]; body.lineWidth=2; [body stroke];
    NSArray *labels=@[@"A",@"B",@"X",@"Y",@"L3",@"R3",@"L",@"R",@"ZL",@"ZR",@"+",@"−",@"←",@"↑",@"→",@"↓"];
    const NSPoint points[]={{337,188},{310,160},{310,216},{282,188},{88,193},{253,112},{69,247},{327,247},{69,280},{327,280},{229,214},{169,214},{119,123},{143,147},{167,123},{143,99}};
    for(int i=0;i<16;i++) {
        NSPoint p=points[i]; CGFloat radius=(i==4||i==5)?28:15;
        NSBezierPath *key=[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(p.x-radius,p.y-radius,radius*2,radius*2)];
        [(self.highlighted==i?NSColor.controlAccentColor:NSColor.controlBackgroundColor) setFill]; [key fill];
        [NSColor.separatorColor setStroke];[key stroke];
        NSDictionary *attrs=@{NSFontAttributeName:[NSFont boldSystemFontOfSize:12],NSForegroundColorAttributeName:self.highlighted==i?NSColor.whiteColor:NSColor.labelColor};
        NSSize size=[labels[i] sizeWithAttributes:attrs];[labels[i] drawAtPoint:NSMakePoint(p.x-size.width/2,p.y-size.height/2) withAttributes:attrs];
    }
    [@"NS Pro Controller" drawAtPoint:NSMakePoint(126,47) withAttributes:@{NSFontAttributeName:[NSFont systemFontOfSize:16 weight:NSFontWeightMedium],NSForegroundColorAttributeName:NSColor.secondaryLabelColor}];
}
@end

/* Key capture belongs only to the settings panel's first responder. No global
   monitor, permissions, synthesized gameplay events or configuration on cancel. */
@interface MNGKeyBindingButton : NSButton
@property BOOL waiting;
@property(copy) NSString *idleTitle;
@property(copy) void (^captured)(unsigned code);
@end
@implementation MNGKeyBindingButton
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)resignFirstResponder { self.waiting=NO; self.title=self.idleTitle; return YES; }
- (void)keyDown:(NSEvent *)event {
    if (!self.waiting) { [super keyDown:event]; return; }
    if (event.isARepeat) return;
    if (event.keyCode==53) { self.waiting=NO;self.title=self.idleTitle;return; }
    if (self.captured) self.captured(event.keyCode);
}
- (void)flagsChanged:(NSEvent *)event {
    if (self.waiting && event.keyCode>=54 && event.keyCode<=62 && self.captured) self.captured(event.keyCode);
}
@end
