/* The Mac window of 明星三缺一 2013: the game draws its own picture in software (an emulated DirectDraw), so the window
   only shows the frame the game queues (hb_display.c → runtime_front_present): one Metal texture copied into the layer's
   drawable, scaled to the window with the picture's shape kept. Keys go to input.c (the console's keyboard and the
   controller layout), the mouse is the touch screen and the console's mouse. */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "runtime.h"
#include "keyboard_layout.h"
#import "mng_active.h"   /* the day's report of active players (on unless turned off in the Launcher's settings) */
#include <stdatomic.h>

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame_bgra; static unsigned frame_w, frame_h; static _Atomic int frame_new;
static NSWindow *window;

void runtime_front_present(const uint32_t *rgba, unsigned width, unsigned height) {
    pthread_mutex_lock(&frame_lock);
    if (!frame_bgra || frame_w != width || frame_h != height) { free(frame_bgra); frame_bgra = malloc((size_t)width * height * 4); frame_w = width; frame_h = height; }
    if (frame_bgra) {
        const uint8_t *s = (const uint8_t *)rgba; uint8_t *d = frame_bgra;
        for (size_t i = 0, n = (size_t)width * height; i < n; i++, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
    }
    pthread_mutex_unlock(&frame_lock);
    atomic_store(&frame_new, 1);
}
void runtime_boot_status(const char *stage, const char *detail) { fprintf(stderr, "[totk-boot] %s: %s\n", stage, detail); }
void runtime_front_request_close(void) {
    if (!NSApp) { fflush(stderr); _exit(0); }   /* TOTK_HEADLESS: no window, no run loop to end */
    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}

@interface GameView : NSView
@property(nonatomic) CAMetalLayer *metal;
@property(nonatomic) id<MTLDevice> device;
@property(nonatomic) id<MTLCommandQueue> queue;
@property(nonatomic) id<MTLTexture> texture;
@end
@implementation GameView
- (instancetype)initWithFrame:(NSRect)r {
    if ((self = [super initWithFrame:r])) {
        self.wantsLayer = YES;
        _device = MTLCreateSystemDefaultDevice(); _queue = [_device newCommandQueue];
        _metal = [CAMetalLayer layer]; _metal.device = _device; _metal.pixelFormat = MTLPixelFormatBGRA8Unorm; _metal.framebufferOnly = NO;
        _metal.contentsGravity = kCAGravityResizeAspect; _metal.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        _metal.magnificationFilter = kCAFilterLinear; _metal.drawableSize = CGSizeMake(1280, 720);
        self.layer = _metal;
    }
    return self;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (void)draw {
    if (!atomic_exchange(&frame_new, 0)) return;
    pthread_mutex_lock(&frame_lock);
    unsigned w = frame_w, h = frame_h;
    if (!_texture || _texture.width != w || _texture.height != h) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        d.storageMode = MTLStorageModeShared; _texture = [_device newTextureWithDescriptor:d];
        _metal.drawableSize = CGSizeMake(w, h);
    }
    [_texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:frame_bgra bytesPerRow:(NSUInteger)w * 4];
    pthread_mutex_unlock(&frame_lock);
    id<CAMetalDrawable> drawable = [_metal nextDrawable]; if (!drawable) return;
    id<MTLCommandBuffer> cb = [_queue commandBuffer]; id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:_texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding]; [cb presentDrawable:drawable]; [cb commit];
}
/* A window point as game pixels (1280x720), with the letterbox of the aspect fit taken out. */
- (NSPoint)gamePoint:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil]; NSSize v = self.bounds.size;
    double gw = frame_w ? frame_w : 1280, gh = frame_h ? frame_h : 720, scale = fmin(v.width / gw, v.height / gh);
    double ox = (v.width - gw * scale) / 2, oy = (v.height - gh * scale) / 2;
    return NSMakePoint((p.x - ox) / scale, gh - (p.y - oy) / scale);
}
- (void)touch:(NSEvent *)e down:(int)down {
    NSPoint g = [self gamePoint:e]; runtime_input_touch(down, (float)g.x, (float)g.y);
    runtime_input_mouse((float)g.x, (float)g.y, down ? 1 : 0);
}
- (void)mouseDown:(NSEvent *)e { [self touch:e down:1]; }
- (void)mouseDragged:(NSEvent *)e { [self touch:e down:1]; }
- (void)mouseUp:(NSEvent *)e { [self touch:e down:0]; }
- (void)mouseMoved:(NSEvent *)e { NSPoint g = [self gamePoint:e]; runtime_input_mouse((float)g.x, (float)g.y, 0); }
- (void)rightMouseDown:(NSEvent *)e { NSPoint g = [self gamePoint:e]; runtime_input_mouse((float)g.x, (float)g.y, 2); }
- (void)rightMouseUp:(NSEvent *)e { NSPoint g = [self gamePoint:e]; runtime_input_mouse((float)g.x, (float)g.y, 0); }
- (void)keyDown:(NSEvent *)e { if (!e.isARepeat) runtime_input_key(e.keyCode, 1); }
- (void)keyUp:(NSEvent *)e { runtime_input_key(e.keyCode, 0); }
- (void)flagsChanged:(NSEvent *)e {
    NSEventModifierFlags f = e.modifierFlags; unsigned k = e.keyCode;
    int down = (k == 56 || k == 60) ? !!(f & NSEventModifierFlagShift) : (k == 58 || k == 61) ? !!(f & NSEventModifierFlagOption)
             : (k == 59 || k == 62) ? !!(f & NSEventModifierFlagControl) : (k == 55 || k == 54) ? !!(f & NSEventModifierFlagCommand) : 0;
    runtime_input_key(k, down);
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic) GameView *view;
@property(nonatomic) void (*boot)(void);
@end
@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                           backing:NSBackingStoreBuffered defer:NO];
    window.title = @"明星三缺一 2013"; window.delegate = self; window.contentAspectRatio = NSMakeSize(16, 9);
    _view = [[GameView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
    window.contentView = _view; window.acceptsMouseMovedEvents = YES; [window center]; [window makeKeyAndOrderFront:nil]; [window makeFirstResponder:_view];
    [NSApp activateIgnoringOtherApps:YES];
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *t) { (void)t; [self.view draw]; }];
    mng_active_start(@"mj2013", mng_active_version());
    self.boot();
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; runtime_input_clear(); }
@end

void runtime_front_run(void (*boot)(void)) {
    @autoreleasepool {
        [NSApplication sharedApplication]; NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *d = [AppDelegate new]; d.boot = boot; NSApp.delegate = d;
        [NSApp run];
    }
}
