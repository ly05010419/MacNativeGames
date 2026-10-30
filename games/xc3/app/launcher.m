#import <Cocoa/Cocoa.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static const int sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
static const int outputs[] = {0, 2160, 1440, 1080, 900};

static NSString *game_folder(void) { return [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"game"]; }
static NSString *game_version(void) { return [NSBundle.mainBundle objectForInfoDictionaryKey:@"XC3Version"]; }
static NSString *game_binary(void) {
    NSString *version = game_version();
    return version ? [game_folder() stringByAppendingFormat:@"/build/xc3-native-%@", version] : nil;
}
static NSSize screen_pixels(NSScreen *s) { CGFloat k = s.backingScaleFactor > 0 ? s.backingScaleFactor : 1; return NSMakeSize(round(s.frame.size.width * k), round(s.frame.size.height * k)); }
static NSString *screen_name(NSScreen *s) { return s.localizedName ?: @"?"; }

@interface Launcher : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property NSWindow *window;
@property NSPopUpButton *size, *monitor, *output;
@property NSSegmentedControl *display, *scaler;
@property NSButton *hud, *asyncShaders, *play; @property NSTextField *note;
@property NSGridView *grid; @property NSInteger sizeRow, outputRow;
@property BOOL launch;
@end

static const CGFloat kWidth = 520, kLabelWidth = 120;

@implementation Launcher
- (NSTextField *)label:(NSString *)s {
    NSTextField *l = [NSTextField labelWithString:s]; l.alignment = NSTextAlignmentRight; l.textColor = NSColor.secondaryLabelColor;
    [l.widthAnchor constraintEqualToConstant:kLabelWidth].active = YES; return l;
}
- (NSTextField *)hint:(NSString *)s {
    NSTextField *t = [NSTextField wrappingLabelWithString:s]; t.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize]; t.textColor = NSColor.secondaryLabelColor;
    t.preferredMaxLayoutWidth = kWidth - kLabelWidth - 40; [t setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationVertical]; return t;
}
- (NSPopUpButton *)popup:(NSArray<NSString *> *)items { NSPopUpButton *p = [NSPopUpButton new]; [p addItemsWithTitles:items]; p.target = self; p.action = @selector(changed:); return p; }
- (NSSegmentedControl *)segments:(NSArray<NSString *> *)labels {
    return [NSSegmentedControl segmentedControlWithLabels:labels trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(changed:)];
}
- (NSStackView *)checkbox:(NSButton *)box hint:(NSString *)hint {
    NSTextField *h = [self hint:hint];
    NSStackView *v = [NSStackView stackViewWithViews:@[box, h]];
    v.orientation = NSUserInterfaceLayoutOrientationVertical; v.alignment = NSLayoutAttributeLeading; v.spacing = 2;
    [h.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:20].active = YES;
    return v;
}
- (NSBox *)separator { NSBox *b = [NSBox new]; b.boxType = NSBoxSeparator; return b; }
- (NSView *)header {
    NSImageView *icon = [NSImageView imageViewWithImage:NSApp.applicationIconImage];
    [icon.widthAnchor constraintEqualToConstant:56].active = YES; [icon.heightAnchor constraintEqualToConstant:56].active = YES;
    NSTextField *title = [NSTextField labelWithString:@"Xenoblade Chronicles 3"]; title.font = [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold];
    NSString *version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"";
    NSTextField *sub = [NSTextField labelWithString:[NSString stringWithFormat:@"Version %@", version]]; sub.textColor = NSColor.secondaryLabelColor;
    NSStackView *text = [NSStackView stackViewWithViews:@[title, sub]];
    text.orientation = NSUserInterfaceLayoutOrientationVertical; text.alignment = NSLayoutAttributeLeading; text.spacing = 2;
    NSStackView *h = [NSStackView stackViewWithViews:@[icon, text]]; h.spacing = 14; h.alignment = NSLayoutAttributeCenterY;
    return h;
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    self.monitor = [NSPopUpButton new]; self.monitor.target = self; self.monitor.action = @selector(monitorChanged:);
    for (NSScreen *s in NSScreen.screens) {
        NSSize px = screen_pixels(s);
        [self.monitor addItemWithTitle:[NSString stringWithFormat:@"%@ (%.0f × %.0f, %ld Hz)", screen_name(s), px.width, px.height, (long)s.maximumFramesPerSecond]];
    }
    self.display = [self segments:@[@"Window", @"Full screen"]];
    NSMutableArray *sizeTitles = [NSMutableArray array];
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) [sizeTitles addObject:[NSString stringWithFormat:@"%d × %d", sizes[i][0], sizes[i][1]]];
    self.size = [self popup:sizeTitles];
    self.output = [NSPopUpButton new]; self.output.target = self; self.output.action = @selector(changed:);
    self.scaler = [self segments:@[@"MetalFX", @"Plain"]];
    self.hud = [NSButton checkboxWithTitle:@"Show frame rate and resolution" target:self action:@selector(changed:)];
    self.asyncShaders = [NSButton checkboxWithTitle:@"Build new shaders in the background" target:self action:@selector(changed:)];
    self.note = [NSTextField wrappingLabelWithString:@""]; self.note.textColor = NSColor.systemOrangeColor; self.note.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    self.note.preferredMaxLayoutWidth = kWidth;
    self.play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(start:)]; self.play.keyEquivalent = @"\r";
    NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:NSApp action:@selector(terminate:)]; quit.keyEquivalent = @"\033";
    [self.play.widthAnchor constraintGreaterThanOrEqualToConstant:80].active = YES; [quit.widthAnchor constraintGreaterThanOrEqualToConstant:80].active = YES;

    NSMutableArray *rows = [NSMutableArray arrayWithArray:@[
        @[[self label:@"Monitor"], self.monitor],
        @[[self label:@"Display"], self.display],
        @[[self label:@"Window size"], self.size],
        @[[self label:@"Resolution"], self.output],
        @[[self label:@"Upscaling"], self.scaler],
    ]];
    self.sizeRow = 2; self.outputRow = 3;
    NSTextField *overlayLabel = [self label:@"Overlay"], *shaderLabel = [self label:@"Shaders"];
    [rows addObject:@[overlayLabel, [self checkbox:self.hud hint:@"Press 1 in the game to hide or show it."]]];
    [rows addObject:@[shaderLabel, [self checkbox:self.asyncShaders hint:@"Avoids pauses when the game uses a shader for the first time. A new effect can appear a moment late."]]];
    self.grid = [NSGridView gridViewWithViews:rows];
    self.grid.rowSpacing = 12; self.grid.columnSpacing = 14; self.grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [self.grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    for (NSInteger r = (NSInteger)rows.count - 2; r < (NSInteger)rows.count; r++) { NSGridRow *row = [self.grid rowAtIndex:r]; row.rowAlignment = NSGridRowAlignmentNone; row.yPlacement = NSGridCellPlacementTop; }
    [overlayLabel.firstBaselineAnchor constraintEqualToAnchor:self.hud.firstBaselineAnchor].active = YES;
    [shaderLabel.firstBaselineAnchor constraintEqualToAnchor:self.asyncShaders.firstBaselineAnchor].active = YES;

    NSView *spacer = [NSView new]; [spacer setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView *buttons = [NSStackView stackViewWithViews:@[spacer, quit, self.play]]; buttons.spacing = 12;
    NSBox *top = [self separator], *bottom = [self separator];
    NSStackView *stack = [NSStackView stackViewWithViews:@[[self header], top, self.grid, self.note, bottom, buttons]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeLeading; stack.spacing = 18;
    stack.edgeInsets = NSEdgeInsetsMake(24, 28, 22, 28);
    for (NSView *v in @[top, bottom, buttons]) [v.widthAnchor constraintEqualToConstant:kWidth].active = YES;
    [self.note.widthAnchor constraintEqualToConstant:kWidth].active = YES;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kWidth + 56, 500) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Xenoblade Chronicles 3"; self.window.contentView = stack; self.window.delegate = self;
    self.window.titlebarAppearsTransparent = YES; self.window.titleVisibility = NSWindowTitleHidden;
    self.window.initialFirstResponder = self.play;

    [self load:d];
    [self refresh];
    [self fitWindow];
    [self.window center]; [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
- (NSScreen *)chosenScreen { NSInteger i = self.monitor.indexOfSelectedItem; NSArray *all = NSScreen.screens; return i >= 0 && i < (NSInteger)all.count ? all[i] : NSScreen.mainScreen; }
- (void)fillOutputs:(int)want {
    [self.output removeAllItems];
    NSSize px = screen_pixels(self.chosenScreen); int pick = 0, k = 0;
    for (unsigned i = 0; i < sizeof outputs / sizeof *outputs; i++) {
        int h = outputs[i];
        if (h && h > px.height) continue;
        [self.output addItemWithTitle:h ? [NSString stringWithFormat:@"%d × %d", h * 16 / 9, h] : [NSString stringWithFormat:@"The monitor's native (%.0f × %.0f)", px.width, px.height]];
        self.output.lastItem.tag = h; if (h == want) pick = k; k++;
    }
    [self.output selectItemAtIndex:pick];
}
- (void)load:(NSUserDefaults *)d {
    NSString *m = [d stringForKey:@"monitor"]; NSArray *all = NSScreen.screens; NSUInteger mi = [all indexOfObject:NSScreen.mainScreen];
    if (mi == NSNotFound) mi = 0;
    for (NSUInteger i = 0; i < all.count; i++) if (m && [screen_name(all[i]) isEqualToString:m]) mi = i;
    [self.monitor selectItemAtIndex:(NSInteger)mi];
    self.display.selectedSegment = [d boolForKey:@"fullscreen"];
    NSInteger s = [d objectForKey:@"size"] ? [d integerForKey:@"size"] : 2; [self.size selectItemAtIndex:s >= 0 && s < 4 ? s : 2];
    self.scaler.selectedSegment = [d objectForKey:@"metalfx"] && ![d boolForKey:@"metalfx"] ? 1 : 0;
    [self fillOutputs:(int)[d integerForKey:@"output"]];
    self.hud.state = [d objectForKey:@"hud"] ? [d boolForKey:@"hud"] : YES;
    self.asyncShaders.state = [d objectForKey:@"asyncShaders"] ? [d boolForKey:@"asyncShaders"] : YES;
}
- (void)monitorChanged:(id)sender { (void)sender; [self fillOutputs:(int)self.output.selectedItem.tag]; [self refresh]; [self save]; }
- (void)fitWindow {
    NSView *content = self.window.contentView; if (!content) return;
    [content layoutSubtreeIfNeeded];
    NSRect f = self.window.frame; NSRect r = [self.window frameRectForContentRect:(NSRect){NSZeroPoint, content.fittingSize}];
    r.origin = NSMakePoint(f.origin.x, NSMaxY(f) - r.size.height); [self.window setFrame:r display:YES];
}
- (void)changed:(id)sender { (void)sender; [self refresh]; [self save]; }
- (void)refresh {
    BOOL full = self.display.selectedSegment == 1;
    [self.grid rowAtIndex:self.sizeRow].hidden = full; [self.grid rowAtIndex:self.outputRow].hidden = !full;
    [self fitWindow];
    NSMutableString *s = [NSMutableString string];
    self.play.enabled = YES;
    if (!game_binary() || ![NSFileManager.defaultManager isExecutableFileAtPath:game_binary()]) {
        [s appendString:@"The game is missing from this app. Build it again with make app."]; self.play.enabled = NO;
    }
    self.note.stringValue = s; self.note.hidden = !s.length;
}
- (void)save {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setBool:self.display.selectedSegment == 1 forKey:@"fullscreen"]; [d setInteger:self.size.indexOfSelectedItem forKey:@"size"];
    [d setInteger:self.output.selectedItem.tag forKey:@"output"]; [d setBool:self.scaler.selectedSegment == 0 forKey:@"metalfx"];
    [d setObject:screen_name(self.chosenScreen) forKey:@"monitor"]; [d setBool:self.hud.state forKey:@"hud"]; [d setBool:self.asyncShaders.state forKey:@"asyncShaders"];
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
}
- (void)start:(id)sender { (void)sender; [self save]; self.launch = YES; [NSApp stop:nil]; [self.window orderOut:nil];
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:e atStart:YES]; }
- (void)windowWillClose:(NSNotification *)n { (void)n; if (!self.launch) { [self save]; [NSApp terminate:nil]; } }
@end

static void fail(NSString *s) {
    NSAlert *a = [NSAlert new]; a.messageText = @"Xenoblade Chronicles 3 could not start"; a.informativeText = s; [a runModal];
}
static BOOL make_dir(NSString *p) { return [NSFileManager.defaultManager createDirectoryAtPath:p withIntermediateDirectories:YES attributes:nil error:nil]; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication]; NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        Launcher *l = [Launcher new]; NSApp.delegate = l;
        [NSApp run];
        if (!l.launch) return 0;
        NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
        NSString *game = game_folder(), *home = NSHomeDirectory();
        NSString *data = [home stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/XC3"];
        NSString *logs = [home stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
        NSString *profile = [data stringByAppendingPathComponent:@"profile"], *slot = [data stringByAppendingPathComponent:@"game-slot"];
        if (!make_dir(profile) || !make_dir(slot) || !make_dir(logs)) { fail([NSString stringWithFormat:@"%@", data]); return 1; }
        if (chdir(game.fileSystemRepresentation)) { fail(game); return 1; }

        /* No frame-rate choice: the game holds itself to 30 fps and has no unlock patch (yet), so frames follow the game's own
           present interval on every display, as `make boot` does. */
        setenv("TOTK_UNCAPPED", "0", 1);

        NSArray<NSScreen *> *all = NSScreen.screens; NSUInteger mi = [all indexOfObject:NSScreen.mainScreen];
        if (mi == NSNotFound) mi = 0;
        for (NSUInteger i = 0; i < all.count; i++) if ([screen_name(all[i]) isEqualToString:[d stringForKey:@"monitor"]]) mi = i;
        setenv("XC3_DISPLAY", [NSString stringWithFormat:@"%lu", (unsigned long)mi].UTF8String, 1);
        BOOL full = [d boolForKey:@"fullscreen"];
        if (full) {
            setenv("XC3_FULLSCREEN", "1", 1);
            int h = (int)[d integerForKey:@"output"]; NSSize px = screen_pixels(all.count ? all[mi] : NSScreen.mainScreen);
            if (h > 0 && h < px.height) setenv("TOTK_DRAWABLE_SCALE", [NSString stringWithFormat:@"%.4f", h / px.height].UTF8String, 1);
        }
        NSInteger s = [d integerForKey:@"size"]; if (s < 0 || s > 3) s = 2;
        setenv("TOTK_WINDOW_SIZE", [NSString stringWithFormat:@"%dx%d", sizes[s][0], sizes[s][1]].UTF8String, 1);
        if (![d objectForKey:@"metalfx"] || [d boolForKey:@"metalfx"]) { setenv("TOTK_MFX_UPSCALE", "1", 1); setenv("TOTK_MFX_OUTPUT", "1", 1); }
        if ([d objectForKey:@"hud"] && ![d boolForKey:@"hud"]) setenv("TOTK_FPS_OVERLAY", "0", 1);
        if ([d objectForKey:@"asyncShaders"] && ![d boolForKey:@"asyncShaders"]) setenv("TOTK_ASYNC_SHADERS", "0", 1);
        setenv("TOTK_ROMFS_MODS", [data stringByAppendingFormat:@"/mods/%@", game_version()].fileSystemRepresentation, 0);
        setenv("TOTK_SHADER_CACHE", [data stringByAppendingPathComponent:@"shader-cache"].fileSystemRepresentation, 0);

        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);
        NSString *preferred = NSLocale.preferredLanguages.firstObject; if (preferred) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1);
        setenv("DM_PRESENT_LINK_FALLBACK", "1", 1);
        setenv("TOTK_SLOT_OWNER", "user", 1); setenv("TOTK_SLOT_PURPOSE", "player", 1);
        unsetenv("TOTK_HEADLESS");
        { NSString *cmds = [data stringByAppendingPathComponent:@"input.txt"]; FILE *cf = fopen(cmds.fileSystemRepresentation, "w"); if (cf) fclose(cf);
          setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1); }

        NSString *log = [logs stringByAppendingPathComponent:@"xc3.log"], *prev = [logs stringByAppendingPathComponent:@"xc3.previous.log"];
        [NSFileManager.defaultManager removeItemAtPath:prev error:nil]; [NSFileManager.defaultManager moveItemAtPath:log toPath:prev error:nil];
        FILE *lf = fopen(log.fileSystemRepresentation, "w"); if (lf) { dup2(fileno(lf), STDOUT_FILENO); dup2(fileno(lf), STDERR_FILENO); fclose(lf); }
        fprintf(stderr, "[launcher] game %s, version %s, display %s, fullscreen %s, window %s, drawable scale %s, MetalFX %s, mod %s\n",
                game.UTF8String, game_version().UTF8String, getenv("XC3_DISPLAY"), full ? "yes" : "no", getenv("TOTK_WINDOW_SIZE"),
                getenv("TOTK_DRAWABLE_SCALE") ?: "1", getenv("TOTK_MFX_OUTPUT") ? "on" : "off", getenv("TOTK_ROMFS_MODS"));
        NSString *rewritten = [@"rewritten" stringByAppendingPathComponent:game_version()];
        char *const args[] = {(char *)game_binary().fileSystemRepresentation, (char *)rewritten.fileSystemRepresentation, "boot", NULL};
        execv(args[0], args);
        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(errno));
        fail([NSString stringWithFormat:@"Starting the game failed. See %@.", log]);
        return 1;
    }
}
