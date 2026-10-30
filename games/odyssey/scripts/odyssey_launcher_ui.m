/* Odyssey.app's settings window, shown before the game starts (user, 26 October: "a game launcher with the game's logo
   that lets the player configure the play settings before the launch"). scripts/odyssey_app_launcher.c runs its own
   executable again with --settings and waits: this process shows the window and exits 0 for Play, 1 for Quit, so the
   game itself starts from a process that never opened a window (one Dock icon, the game's).
   Everything is kept in "Odyssey Data", in the files the launcher and the game already read:
     graphics.txt         aa, cas, upscaler, textures, resolution (third_party/d2r_gfx/gfx/nvn/odyssey_gfx.h; keys 3-7 are TOTK's),
                          frame_generation (60+60 at start), launcher (show / skip this window)
     windowed             present: start in a window
     session-minutes.txt  the session limit (0 = none)
     face-buttons.txt     position / label (runtime/gamepad_layout.h)
   The window shows the app's own icon (made from the game's icon at packaging). */
#import <Cocoa/Cocoa.h>
#include <sys/stat.h>

static NSString *g_data;
static NSMutableDictionary<NSString *, NSString *> *g_gfx;
static NSMutableArray<NSString *> *g_gfx_order;

static NSString *data_file(NSString *name) { return [g_data stringByAppendingPathComponent:name]; }
static void load_graphics(void) {
    g_gfx = [NSMutableDictionary new]; g_gfx_order = [NSMutableArray new];
    NSString *text = [NSString stringWithContentsOfFile:data_file(@"graphics.txt") encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSRange eq = [line rangeOfString:@"="]; if (eq.location == NSNotFound) continue;
        NSString *k = [line substringToIndex:eq.location], *v = [[line substringFromIndex:eq.location + 1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!g_gfx[k]) [g_gfx_order addObject:k];
        g_gfx[k] = v;
    }
}
static void save_graphics(void) {
    NSMutableString *out = [NSMutableString new];
    for (NSString *k in g_gfx_order) [out appendFormat:@"%@=%@\n", k, g_gfx[k]];
    [out writeToFile:data_file(@"graphics.txt") atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
static void set_graphics(NSString *k, NSString *v) { if (!g_gfx[k]) [g_gfx_order addObject:k]; g_gfx[k] = v; }

@interface OdysseySettings : NSObject <NSWindowDelegate>
@property NSWindow *window;
@property NSMutableDictionary<NSString *, NSPopUpButton *> *popups;
@property NSButton *cas, *showAtLaunch;
@property int result;
@end

@implementation OdysseySettings
// One labelled pop-up: titles shown, tokens written to the file.
- (NSPopUpButton *)row:(NSString *)label key:(NSString *)key titles:(NSArray<NSString *> *)titles tokens:(NSArray<NSString *> *)tokens
               current:(NSString *)current in:(NSView *)view y:(CGFloat)y {
    NSTextField *l = [NSTextField labelWithString:label]; l.frame = NSMakeRect(40, y + 3, 190, 20); l.alignment = NSTextAlignmentRight;
    l.textColor = NSColor.secondaryLabelColor; [view addSubview:l];
    NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(240, y, 260, 26) pullsDown:NO];
    [p addItemsWithTitles:titles];
    for (NSUInteger i = 0; i < tokens.count; i++) [p itemAtIndex:(NSInteger)i].representedObject = tokens[i];
    NSUInteger at = [tokens indexOfObject:current ?: @""]; [p selectItemAtIndex:at == NSNotFound ? 0 : (NSInteger)at];
    [view addSubview:p]; self.popups[key] = p;
    return p;
}
- (NSTextField *)header:(NSString *)text in:(NSView *)view y:(CGFloat)y {
    NSTextField *h = [NSTextField labelWithString:text]; h.font = [NSFont boldSystemFontOfSize:13]; h.frame = NSMakeRect(40, y, 460, 18);
    [view addSubview:h]; return h;
}
- (void)build {
    self.popups = [NSMutableDictionary new];
    NSRect frame = NSMakeRect(0, 0, 560, 690);
    self.window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Super Mario Odyssey"; self.window.delegate = self; self.window.releasedWhenClosed = NO;
    NSView *v = self.window.contentView;

    NSImageView *logo = [NSImageView imageViewWithImage:NSApp.applicationIconImage ?: [NSImage imageNamed:NSImageNameApplicationIcon]];
    logo.frame = NSMakeRect(40, 540, 128, 128); logo.imageScaling = NSImageScaleProportionallyUpOrDown; [v addSubview:logo];
    NSTextField *title = [NSTextField labelWithString:@"Super Mario Odyssey"]; title.font = [NSFont systemFontOfSize:26 weight:NSFontWeightBold];
    title.frame = NSMakeRect(184, 618, 340, 34); [v addSubview:title];
    /* The game's version from its update's Control data (scripts/extract_app_icon.py writes Resources/game-version.txt):
       1.3.0 and 1.4.1 share their executables, so only that file tells them apart. */
    NSString *ver = [[NSString stringWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"game-version.txt"]
                                               encoding:NSUTF8StringEncoding error:nil]
                     stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSString *head = ver.length ? [NSString stringWithFormat:@"Version %@, native on Apple silicon.", ver] : @"Native on Apple silicon.";
    NSTextField *sub = [NSTextField wrappingLabelWithString:[head stringByAppendingString:@"\nBuilt from your own game files: do not share this app."]];
    sub.textColor = NSColor.secondaryLabelColor; sub.frame = NSMakeRect(184, 560, 340, 50); [v addSubview:sub];

    CGFloat y = 510;
    [self header:@"Graphics" in:v y:y]; y -= 34;
    [self row:@"Resolution (render size)" key:@"resolution" titles:@[@"1600 × 900 (the game's own)", @"1920 × 1080", @"2240 × 1260", @"2560 × 1440"]
       tokens:@[@"1600x900", @"1920x1080", @"2240x1260", @"2560x1440"] current:g_gfx[@"resolution"] in:v y:y]; y -= 32;
    [self row:@"Anti-aliasing" key:@"aa" titles:@[@"Off", @"FXAA", @"SMAA"] tokens:@[@"off", @"fxaa", @"smaa"] current:g_gfx[@"aa"] in:v y:y]; y -= 32;
    [self row:@"Upscaler" key:@"upscaler" titles:@[@"MetalFX", @"FSR 1", @"Lanczos", @"Bilinear"] tokens:@[@"metalfx", @"fsr1", @"lanczos", @"bilinear"]
       current:g_gfx[@"upscaler"] in:v y:y]; y -= 32;
    [self row:@"Texture filtering" key:@"textures" titles:@[@"Game default", @"4× anisotropic", @"8× anisotropic", @"16× anisotropic"]
       tokens:@[@"game", @"4x", @"8x", @"16x"] current:g_gfx[@"textures"] in:v y:y]; y -= 30;
    self.cas = [NSButton checkboxWithTitle:@"Sharpen the picture (CAS)" target:nil action:nil]; self.cas.frame = NSMakeRect(240, y, 260, 20);
    self.cas.state = [g_gfx[@"cas"] isEqualToString:@"on"] ? NSControlStateValueOn : NSControlStateValueOff; [v addSubview:self.cas]; y -= 44;

    [self header:@"Play" in:v y:y]; y -= 34;
    [self row:@"Frame rate" key:@"frame_generation" titles:@[@"60 fps", @"60+60 frame generation (120 Hz screens)"] tokens:@[@"off", @"on"]
       current:g_gfx[@"frame_generation"] in:v y:y]; y -= 32;
    struct stat st; BOOL windowed = !stat(data_file(@"windowed").fileSystemRepresentation, &st);
    [self row:@"Display" key:@"display" titles:@[@"Full screen", @"Window"] tokens:@[@"full", @"window"] current:windowed ? @"window" : @"full" in:v y:y]; y -= 32;
    NSString *minutes = [[NSString stringWithContentsOfFile:data_file(@"session-minutes.txt") encoding:NSUTF8StringEncoding error:nil]
                         stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"20";
    [self row:@"Session limit" key:@"minutes" titles:@[@"20 minutes", @"60 minutes", @"90 minutes", @"120 minutes", @"No limit"]
       tokens:@[@"20", @"60", @"90", @"120", @"0"] current:minutes in:v y:y]; y -= 32;
    NSString *face = [[NSString stringWithContentsOfFile:data_file(@"face-buttons.txt") encoding:NSUTF8StringEncoding error:nil]
                      stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"label";
    [self row:@"Controller A / B / X / Y" key:@"face" titles:@[@"By label (as printed)", @"By position (Xbox layout)"] tokens:@[@"label", @"position"]
       current:face in:v y:y]; y -= 40;

    NSTextField *note = [NSTextField wrappingLabelWithString:@"In game: 1 shows or hides the overlay, 2 turns 60+60 on or off, 3 steps the picture size, 4 the aspect ratio, "
                         "5 edge smoothing, 6 the controller type, 7 mouse mode. Anti-aliasing, sharpening and the upscaler apply while 60+60 is on."];
    note.font = [NSFont systemFontOfSize:11]; note.textColor = NSColor.secondaryLabelColor; note.frame = NSMakeRect(40, y - 24, 480, 48); [v addSubview:note];

    self.showAtLaunch = [NSButton checkboxWithTitle:@"Show this window at every launch (otherwise hold ⌥ Option)" target:nil action:nil];
    self.showAtLaunch.frame = NSMakeRect(40, 62, 480, 20);
    self.showAtLaunch.state = [g_gfx[@"launcher"] isEqualToString:@"skip"] ? NSControlStateValueOff : NSControlStateValueOn; [v addSubview:self.showAtLaunch];
    NSButton *quit = [NSButton buttonWithTitle:@"Quit" target:self action:@selector(quit:)]; quit.frame = NSMakeRect(320, 18, 90, 32);
    quit.keyEquivalent = @"\e"; [v addSubview:quit];
    NSButton *play = [NSButton buttonWithTitle:@"Play" target:self action:@selector(play:)]; play.frame = NSMakeRect(420, 18, 100, 32);
    play.keyEquivalent = @"\r"; [v addSubview:play];

    [self.window center]; [self.window makeKeyAndOrderFront:nil];
}
- (NSString *)token:(NSString *)key { return self.popups[key].selectedItem.representedObject; }
- (void)play:(id)sender {
    (void)sender;
    for (NSString *k in @[@"aa", @"upscaler", @"textures", @"resolution", @"frame_generation"]) set_graphics(k, [self token:k]);
    set_graphics(@"cas", self.cas.state == NSControlStateValueOn ? @"on" : @"off");
    set_graphics(@"launcher", self.showAtLaunch.state == NSControlStateValueOn ? @"show" : @"skip");
    save_graphics();
    NSString *w = data_file(@"windowed");
    if ([[self token:@"display"] isEqualToString:@"window"]) [NSData.data writeToFile:w atomically:YES];
    else [NSFileManager.defaultManager removeItemAtPath:w error:nil];
    [[[self token:@"minutes"] stringByAppendingString:@"\n"] writeToFile:data_file(@"session-minutes.txt") atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [[[self token:@"face"] stringByAppendingString:@"\n"] writeToFile:data_file(@"face-buttons.txt") atomically:YES encoding:NSUTF8StringEncoding error:nil];
    self.result = 0; [NSApp stop:nil];
}
- (void)quit:(id)sender { (void)sender; self.result = 1; [NSApp stop:nil]; }
- (void)windowWillClose:(NSNotification *)n { (void)n; if (self.result < 0) { self.result = 1; [NSApp stop:nil]; } }
@end

int odyssey_settings_window(const char *data_dir) {
    @autoreleasepool {
        g_data = @(data_dir); load_graphics();
        [NSApplication sharedApplication];
        NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        OdysseySettings *s = [OdysseySettings new]; s.result = -1;
        [s build];
        [NSApp activate];
        [NSApp run];
        int r = s.result == 0 ? 0 : 1;
        [s.window orderOut:nil];
        return r;
    }
}
