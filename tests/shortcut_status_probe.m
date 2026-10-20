/* Production AppKit panel, keyboard dispatch, mode state and output helpers.
   No guest, save files, game window or GPU commands are used. */
#include "../games/totk/runtime/frontend.m"
#include "../games/totk/runtime/input.c"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/fps_overlay.h"
#include <assert.h>

void mfx_set(int mode);
int mfx_latch(void);
void mfxup_set(int on);
void mfxup_output_set(int on);
void shortcut_test_output_init(void);
void shortcut_test_output_size(unsigned w, unsigned h);
int shortcut_test_scene_init(void);
void nvn_frame_mode_cycle(void);

void dm_gfx_set_flag(const char *name, int value) {
    if (!strcmp(name, "mfx_interp")) mfx_set(value);
    else if (!strcmp(name, "frame_mode") && value == -1) nvn_frame_mode_cycle();
    else if (!strcmp(name, "mfx_output")) mfxup_output_set(value);
    else if (!strcmp(name, "mfx_upscale")) mfxup_set(value);
    else abort();
}
static void press(unsigned key) {
    runtime_input_key_synthetic(key, 1);
    runtime_input_key_synthetic(key, 0);
}
static void tick(void) {
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    [((TOTKShortcutHelp *)shortcutHelp).timer fire];
}
static void expect(TOTKShortcutHelp *panel, NSString *text) {
    if (![panel.label.stringValue containsString:text]) {
        fprintf(stderr, "expected %s in:\n%s\n", text.UTF8String, panel.label.stringValue.UTF8String);
        abort();
    }
}

int main(int argc, const char **argv) { @autoreleasepool {
    [NSApplication sharedApplication];
    shortcut_test_output_init();
    int initial = mfx_enabled();
    assert(initial == ((!getenv("TOTK_MFX") || !strcmp(getenv("TOTK_MFX"), "0")) ? MFX_OFF : MFX_HALF));
    NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 900, 540)];
    TOTKShortcutHelp *panel = [[TOTKShortcutHelp alloc] initWithFrame:NSZeroRect];
    [host addSubview:panel]; shortcutHelp = panel;
    assert(panel.timer && panel.timer.valid);
    assert(panel.timer.timeInterval == 0.5);
    [panel refresh];
    expect(panel, initial ? @"half refresh rate" : @"Frame rate: auto 72 fps");
    expect(panel, mfxup_output_enabled() ? @"MetalFX 2K" : @"Picture: off");

    /* Key 2: auto, the even caps, uncapped, then interpolation; a request is not the current frame's mode until latched. */
    mfx_set(MFX_OFF); mfx_latch();
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.6]];
    expect(panel, @"Frame rate: auto 72 fps");
    press(19); tick(); expect(panel, @"Frame rate: 72 fps");
    runtime_input_key_synthetic(19, 1); runtime_input_key_synthetic(19, 1);
    runtime_input_key_synthetic(19, 0); tick(); expect(panel, @"Frame rate: 48 fps");
    press(19); tick(); expect(panel, @"Frame rate: 36 fps");
    press(19); tick(); expect(panel, @"Frame rate: uncapped");
    press(19); tick(); expect(panel, @"Frame rate: uncapped");
    mfx_latch(); tick(); expect(panel, @"Frame rate: interpolated 30+30");
    press(19); mfx_latch(); tick(); expect(panel, @"Frame rate: interpolated, half refresh rate");
    press(19); mfx_latch(); tick(); expect(panel, @"Frame rate: interpolated, unlimited");
    press(19); mfx_latch(); tick(); expect(panel, @"Frame rate: auto 72 fps");

    mfxup_output_set(0); tick(); expect(panel, @"Picture: off");
    press(20); tick(); expect(panel, @"Picture: MetalFX 2K");
    press(20); tick(); expect(panel, @"Picture: native 2K");
    press(20); tick(); expect(panel, @"Picture: off");
    /* Startup/diagnostic paths can set the canvas without a key press. */
    shortcut_test_output_size(3840, 2160); tick(); expect(panel, @"Picture: native 3840×2160");
    shortcut_test_output_size(2560, 1440);
    if (shortcut_test_scene_init()) {   /* 1.4.3: key 3 cycles the render resolution, MetalFX scaling it to the 1440p canvas */
        unsetenv("TOTK_MFX_UPSCALE");   /* the harness turned the scene stage off for the checks above; key 3 keeps the default */
        mfxup_set(1); tick(); expect(panel, @"Picture: 1080p MetalFX");
        press(20); tick(); expect(panel, @"Picture: native 1440p");
        press(20); tick(); expect(panel, @"Picture: 720p MetalFX");
        press(20); tick(); expect(panel, @"Picture: 900p MetalFX");
        press(20); tick(); expect(panel, @"Picture: 1080p MetalFX");
        assert(mfxup_output_enabled() && mfxup_enabled());
        press(20); tick();
    }
    mfx_set(MFX_HALF); mfx_latch();
    setenv("TOTK_UI_LANG", "zh", 1); tick();
    expect(panel, @"帧率：当前 补帧 刷新率一半"); expect(panel, @"画质：当前 原生");

    if (hud_compact()) {
        assert(panel.frame.size.width < host.bounds.size.width - 20);
        assert(panel.frame.size.height < host.bounds.size.height - 20);
        assert(panel.frame.origin.x >= 0 && panel.frame.origin.y >= 0);
        assert(NSWidth(panel.label.frame) >= panel.textSize.width);
    } else {
        NSSize fit = [panel.label sizeThatFits:panel.label.bounds.size];
        assert(fit.height <= panel.label.bounds.size.height);
    }
    press(18); tick(); assert(panel.hidden && !fps_overlay_hints_enabled());
    mfx_set(MFX_MAX); mfx_latch(); tick();
    press(18); tick(); assert(!panel.hidden && fps_overlay_hints_enabled());
    expect(panel, @"帧率：当前 补帧 不限帧");
    hud_publish(0, "60 FPS (30 + 30)", 0); tick();
    if (hud_compact()) expect(panel, @"60 FPS (30 + 30)");

    if (argc == 2) {
        NSBitmapImageRep *rep = [panel bitmapImageRepForCachingDisplayInRect:panel.bounds];
        [panel cacheDisplayInRect:panel.bounds toBitmapImageRep:rep];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
                writeToFile:@(argv[1]) atomically:YES]);
    }
    [panel.timer invalidate];
    puts("pass: current modes, keyboard cycling, frame latch, localization, both layouts and hide/restore");
    return 0;
}}
