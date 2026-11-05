/* Pokémon Legends: Arceus launcher: a settings window with presets, then the game (forked from Bayonetta 3).
   The app is a small bundle; the game's data stay in the work folder (PLA_build) named by Info.plist's BY3WorkFolder,
   so nothing large is copied. Settings are saved at every change (org.macnativegames.by3) and shown again next time.
   What each setting does in the runtime:
   - Frame rate: 30 = the game's own pacing (30 / 60 by scene); 60 = the 60 fps patch; 120 = the 60 fps patch on a 120 Hz
     present base (experimental: correct only if the game advances by real time, not by frames).
   - Internal resolution: dynamic (the game's own, up to 1600x900) or fixed 900p (patches/res-1600x900, else the
     community "Force Scaling 900p" patch). Larger internal sizes do not start yet (the game's texture heaps).
   - Monitor (PLA_DISPLAY), window or full screen (PLA_FULLSCREEN), window size (TOTK_WINDOW_SIZE), the full-screen
     output resolution (TOTK_DRAWABLE_SCALE relative to the monitor's pixels) and the scaler (MetalFX spatial upscaling of
     the game's picture to the output, TOTK_MFX_UPSCALE / TOTK_MFX_OUTPUT, or plain linear scaling).
   - Extras: improved LOD (off in every preset: it breaks the geometry here), no film grain (patches); the RomFS
     interface mod; the FPS / resolution HUD.
   Patches come from <work>/patches (scripts/prepare_patches.py, make_res_patch.py); a missing one is left out and the
   window says so. */
#import <Cocoa/Cocoa.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static BOOL spanish(void) { return [NSLocale.preferredLanguages.firstObject hasPrefix:@"es"]; }
#define T(es, en) (spanish() ? @es : @en)

/* output: 0 = the monitor's native pixels, else the height of a 16:9 output (2160, 1440, 1080, 900). */
typedef struct { const char *name_es, *name_en; int fps, fixed900, fullscreen, metalfx, output, lod, nograin, size; } Preset;
/* PLA presets. The 30 fps track is the one with the game's own speed: the 60 fps track changes the game's frame step so
   its simulation runs at double speed (see PLA_REALTIME and key 5), so it is offered as an experiment, not as the default. */
static const Preset presets[] = {
    {"Original (30 fps, velocidad correcta)", "Original (30 fps, correct speed)", 30, 0, 0, 0, 0, 0, 0, 2},
    {"Recomendado (30 fps, 1080p nativo)", "Recommended (30 fps, native 1080p)", 30, 0, 0, 0, 3, 0, 0, 2},
    {"Calidad (30 fps, pantalla completa, MetalFX 2K)", "Quality (30 fps, full screen, MetalFX 2K)", 30, 0, 1, 1, 0, 0, 0, 2},
    {"Experimental (60 fps: el juego va al doble de velocidad)", "Experimental (60 fps: the game runs at double speed)", 60, 0, 0, 0, 0, 0, 0, 2},
};
enum { NPRESETS = sizeof presets / sizeof *presets };
static const int fps_values[] = {30, 60, 120};
static const int sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
static const int outputs[] = {0, 2160, 1440, 1080, 900};

static NSString *work_folder(void) {
    const char *e = getenv("PLA_WORK");
    NSString *v = e && *e ? @(e) : [NSBundle.mainBundle objectForInfoDictionaryKey:@"PLAWorkFolder"];
    if (!v.length) v = [NSBundle.mainBundle objectForInfoDictionaryKey:@"BY3WorkFolder"];   /* older bundles */
    if (!v.length) return v;
    /* A relative value is inside the bundle (self-contained app: Resources/Work), so resolve it against the bundle's
       Resources directory - the bundle can then be copied anywhere and still start. Absolute paths (development runs)
       keep working unchanged. */
    if (![v isAbsolutePath]) {
        NSString *res = [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/Resources"];
        v = [res stringByAppendingPathComponent:v];
    }
    return v;
}
static BOOL have_patch(NSString *slug) {
    return [NSFileManager.defaultManager fileExistsAtPath:[work_folder() stringByAppendingFormat:@"/patches/%@.tsv", slug]];
}
static NSString *fixed900_patch(void) { return have_patch(@"res-1600x900") ? @"res-1600x900" : have_patch(@"force-scaling-900p-v3") ? @"force-scaling-900p-v3" : nil; }
/* RomFS mods: <work>/../mods/<name>/romfs with a manifest.tsv (make mods). */
static NSString *mods_folder(void) { return [[work_folder() stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"mods"]; }
static NSArray<NSString *> *romfs_mods(void) {
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *name in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:mods_folder() error:nil] sortedArrayUsingSelector:@selector(compare:)])
        if ([NSFileManager.defaultManager fileExistsAtPath:[mods_folder() stringByAppendingFormat:@"/%@/romfs/manifest.tsv", name]]) [out addObject:name];
    return out;
}
static NSSize screen_pixels(NSScreen *s) { CGFloat k = s.backingScaleFactor > 0 ? s.backingScaleFactor : 1; return NSMakeSize(round(s.frame.size.width * k), round(s.frame.size.height * k)); }
static NSString *screen_name(NSScreen *s) { return s.localizedName ?: @"?"; }

@interface Launcher : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property NSWindow *window;
@property NSPopUpButton *preset, *size, *internal, *monitor, *output, *mod;
@property NSSegmentedControl *fps, *display, *scaler;
@property NSButton *lod, *grain, *hud, *play; @property NSTextField *note;
@property NSArray<NSString *> *mods; @property BOOL applying, launch;
@end

@implementation Launcher
- (NSTextField *)label:(NSString *)s { NSTextField *l = [NSTextField labelWithString:s]; l.alignment = NSTextAlignmentRight; return l; }
- (NSPopUpButton *)popup:(NSArray<NSString *> *)items { NSPopUpButton *p = [NSPopUpButton new]; [p addItemsWithTitles:items]; p.target = self; p.action = @selector(changed:); return p; }
- (NSSegmentedControl *)segments:(NSArray<NSString *> *)labels {
    return [NSSegmentedControl segmentedControlWithLabels:labels trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(changed:)];
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    self.preset = [NSPopUpButton new];
    for (int i = 0; i < NPRESETS; i++) [self.preset addItemWithTitle:spanish() ? @(presets[i].name_es) : @(presets[i].name_en)];
    [self.preset addItemWithTitle:T("Personalizado", "Custom")];
    self.preset.target = self; self.preset.action = @selector(presetChanged:);
    self.fps = [self segments:@[@"30", @"60", T("120 (experimental)", "120 (experimental)")]];
    self.internal = [self popup:@[T("Dinámica (la del juego, hasta 900p)", "Dynamic (the game's own, up to 900p)"), T("900p fija", "Fixed 900p")]];
    self.monitor = [NSPopUpButton new]; self.monitor.target = self; self.monitor.action = @selector(monitorChanged:);
    for (NSScreen *s in NSScreen.screens) {
        NSSize px = screen_pixels(s);
        [self.monitor addItemWithTitle:[NSString stringWithFormat:@"%@ — %.0f × %.0f, %ld Hz", screen_name(s), px.width, px.height, (long)s.maximumFramesPerSecond]];
    }
    self.display = [self segments:@[T("Ventana", "Window"), T("Pantalla completa", "Full screen")]];
    NSMutableArray *sizeTitles = [NSMutableArray array];
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) [sizeTitles addObject:[NSString stringWithFormat:@"%d × %d", sizes[i][0], sizes[i][1]]];
    self.size = [self popup:sizeTitles];
    self.output = [NSPopUpButton new]; self.output.target = self; self.output.action = @selector(changed:);
    self.scaler = [self segments:@[T("MetalFX (más nítido)", "MetalFX (sharper)"), T("Normal", "Plain")]];
    self.lod = [NSButton checkboxWithTitle:T("LOD mejorado (más detalle a distancia)", "Improved LOD (more distant detail)") target:self action:@selector(changed:)];
    self.grain = [NSButton checkboxWithTitle:T("Quitar el grano de película", "Remove film grain") target:self action:@selector(changed:)];
    self.mod = [self popup:@[T("Ninguno", "None")]]; self.mods = romfs_mods(); [self.mod addItemsWithTitles:self.mods];
    self.hud = [NSButton checkboxWithTitle:T("Mostrar FPS y resolución (tecla 1 lo oculta)", "Show FPS and resolution (key 1 hides it)") target:self action:@selector(changed:)];
    self.note = [NSTextField wrappingLabelWithString:@""]; self.note.textColor = NSColor.secondaryLabelColor; self.note.font = [NSFont systemFontOfSize:11];
    self.play = [NSButton buttonWithTitle:T("Jugar", "Play") target:self action:@selector(start:)]; self.play.keyEquivalent = @"\r";
    NSButton *quit = [NSButton buttonWithTitle:T("Salir", "Quit") target:NSApp action:@selector(terminate:)]; quit.keyEquivalent = @"\033";

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[[self label:T("Preset", "Preset")], self.preset],
        @[[self label:T("Fotogramas por segundo", "Frame rate")], self.fps],
        @[[self label:T("Resolución interna", "Internal resolution")], self.internal],
        @[[self label:T("Monitor", "Monitor")], self.monitor],
        @[[self label:T("Pantalla", "Display")], self.display],
        @[[self label:T("Tamaño de la ventana", "Window size")], self.size],
        @[[self label:T("Resolución de salida", "Output resolution")], self.output],
        @[[self label:T("Escalado", "Scaling")], self.scaler],
        @[[self label:T("Mejoras", "Extras")], self.lod],
        @[NSGridCell.emptyContentView, self.grain],
        @[[self label:T("Mod de interfaz", "Interface mod")], self.mod],
        @[[self label:T("Información", "Info")], self.hud],
    ]];
    grid.rowSpacing = 10; grid.columnSpacing = 12; [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    NSStackView *buttons = [NSStackView stackViewWithViews:@[quit, self.play]];
    NSStackView *stack = [NSStackView stackViewWithViews:@[grid, self.note, buttons]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical; stack.alignment = NSLayoutAttributeTrailing; stack.spacing = 16;
    stack.edgeInsets = NSEdgeInsetsMake(20, 20, 20, 20);
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 680, 480) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"Pokémon Legends: Arceus"; self.window.contentView = stack; self.window.delegate = self;
    [self.note.widthAnchor constraintEqualToConstant:600].active = YES;

    [self loadCommon:d];
    NSInteger p = [d objectForKey:@"preset"] ? [d integerForKey:@"preset"] : 1;
    if (p >= 0 && p < NPRESETS) [self applyPreset:(int)p]; else { [self.preset selectItemAtIndex:NPRESETS]; [self load:d]; }
    [self refresh];
    [self.window center]; [self.window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
}
- (NSScreen *)chosenScreen { NSInteger i = self.monitor.indexOfSelectedItem; NSArray *all = NSScreen.screens; return i >= 0 && i < (NSInteger)all.count ? all[i] : NSScreen.mainScreen; }
/* The output resolutions this monitor can show: native first, then the 16:9 ones that fit. */
- (void)fillOutputs:(int)want {
    [self.output removeAllItems];
    NSSize px = screen_pixels(self.chosenScreen); int pick = 0, k = 0;
    for (unsigned i = 0; i < sizeof outputs / sizeof *outputs; i++) {
        int h = outputs[i];
        if (h && h > px.height) continue;
        [self.output addItemWithTitle:h ? [NSString stringWithFormat:@"%d × %d", h * 16 / 9, h] : [NSString stringWithFormat:T("Nativa del monitor (%.0f × %.0f)", "The monitor's native (%.0f × %.0f)"), px.width, px.height]];
        self.output.lastItem.tag = h; if (h == want) pick = k; k++;
    }
    [self.output selectItemAtIndex:pick];
}
- (void)loadCommon:(NSUserDefaults *)d {   // not part of presets
    NSString *m = [d stringForKey:@"monitor"]; NSArray *all = NSScreen.screens; NSUInteger mi = [all indexOfObject:NSScreen.mainScreen];
    if (mi == NSNotFound) mi = 0;
    for (NSUInteger i = 0; i < all.count; i++) if (m && [screen_name(all[i]) isEqualToString:m]) mi = i;
    [self.monitor selectItemAtIndex:(NSInteger)mi];
    self.hud.state = [d objectForKey:@"hud"] ? [d boolForKey:@"hud"] : YES;
    NSString *mod = [d stringForKey:@"mod"]; NSUInteger i = mod ? [self.mods indexOfObject:mod] : NSNotFound;
    [self.mod selectItemAtIndex:i == NSNotFound ? 0 : (NSInteger)i + 1];
}
- (void)load:(NSUserDefaults *)d {
    int fps = (int)[d integerForKey:@"fps"]; self.fps.selectedSegment = fps == 120 ? 2 : fps == 30 ? 0 : 1;
    [self.internal selectItemAtIndex:[d boolForKey:@"fixed900"]]; self.display.selectedSegment = [d boolForKey:@"fullscreen"];
    NSInteger s = [d objectForKey:@"size"] ? [d integerForKey:@"size"] : 2; [self.size selectItemAtIndex:s >= 0 && s < 4 ? s : 2];
    self.scaler.selectedSegment = [d objectForKey:@"metalfx"] && ![d boolForKey:@"metalfx"] ? 1 : 0;
    [self fillOutputs:(int)[d integerForKey:@"output"]];
    self.lod.state = [d boolForKey:@"lod"]; self.grain.state = [d boolForKey:@"nograin"];
}
- (void)applyPreset:(int)i {
    const Preset *p = &presets[i]; self.applying = YES;
    [self.preset selectItemAtIndex:i]; self.fps.selectedSegment = p->fps == 30 ? 0 : p->fps == 60 ? 1 : 2;
    [self.internal selectItemAtIndex:p->fixed900]; self.display.selectedSegment = p->fullscreen; [self.size selectItemAtIndex:p->size];
    self.scaler.selectedSegment = p->metalfx ? 0 : 1; [self fillOutputs:p->output];
    self.lod.state = p->lod; self.grain.state = p->nograin; self.applying = NO;
}
/* Every change is saved at once (and written to disk), so the next start shows the same choices. */
- (void)presetChanged:(id)sender { (void)sender; NSInteger i = self.preset.indexOfSelectedItem; if (i < NPRESETS) [self applyPreset:(int)i]; [self refresh]; [self save]; }
- (void)monitorChanged:(id)sender { (void)sender; [self fillOutputs:(int)self.output.selectedItem.tag]; [self refresh]; [self save]; }
- (void)changed:(id)sender {
    if (!self.applying && sender != self.hud && sender != self.mod) [self.preset selectItemAtIndex:NPRESETS];
    [self refresh]; [self save];
}
- (void)refresh {
    BOOL full = self.display.selectedSegment == 1;
    self.size.enabled = !full; self.output.enabled = full;
    NSMutableArray *missing = [NSMutableArray array];
    // The 60 fps track is the port's own now (real-time frame step + one present per display tick), so a 60fps game
    // patch from mods/ is neither needed nor wanted: two patches at the same site would fight each other.
    if (self.internal.indexOfSelectedItem == 1 && !fixed900_patch()) [missing addObject:@"res-1600x900 / Force Scaling 900p"];
    if (self.lod.state && !have_patch(@"improved-lod-v2")) [missing addObject:@"Improved LOD v2"];
    if (self.grain.state && !have_patch(@"remove-film-grain")) [missing addObject:@"Remove Film Grain"];
    NSMutableString *s = [NSMutableString string];
    if (self.fps.selectedSegment == 2) {
        [s appendString:T("120 fps es experimental (los fotogramas los limita el monitor). ", "120 fps is experimental (frames are capped by the display). ")];
        if (self.chosenScreen.maximumFramesPerSecond < 120) [s appendString:T("Este monitor no llega a 120 Hz. ", "This monitor does not reach 120 Hz. ")];
    }
    if (self.lod.state)   // tried 2026-10-19: with the patch the scenery breaks into blocks and smears, characters turn ghostly
        [s appendString:T("Mejor LOD rompe la geometría en este port (bloques, personajes fantasma): mejor apagado. ", "Improved LOD breaks the geometry in this port (blocks, ghostly characters): best left off. ")];
    if (missing.count)
        [s appendFormat:T("Faltan estos parches y se omitirán: %@ (make patches). ", "These patches are missing and will be left out: %@ (make patches). "),
                          [missing componentsJoinedByString:@", "]];
    self.play.enabled = YES;
    if (!work_folder() || ![NSFileManager.defaultManager isExecutableFileAtPath:[work_folder() stringByAppendingPathComponent:@"build/pla-native"]]) {
        [s appendFormat:T("No encuentro el juego en %@.", "The game is not in %@."), work_folder() ?: @"?"]; self.play.enabled = NO;
    }
    self.note.stringValue = s; self.note.hidden = !s.length;
}
- (void)save {
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    [d setInteger:self.preset.indexOfSelectedItem forKey:@"preset"];
    [d setInteger:fps_values[self.fps.selectedSegment] forKey:@"fps"]; [d setBool:self.internal.indexOfSelectedItem == 1 forKey:@"fixed900"];
    [d setBool:self.display.selectedSegment == 1 forKey:@"fullscreen"]; [d setInteger:self.size.indexOfSelectedItem forKey:@"size"];
    [d setInteger:self.output.selectedItem.tag forKey:@"output"]; [d setBool:self.scaler.selectedSegment == 0 forKey:@"metalfx"];
    [d setObject:screen_name(self.chosenScreen) forKey:@"monitor"];
    [d setBool:self.lod.state forKey:@"lod"]; [d setBool:self.grain.state forKey:@"nograin"]; [d setBool:self.hud.state forKey:@"hud"];
    if (self.mod.indexOfSelectedItem > 0) [d setObject:self.mods[self.mod.indexOfSelectedItem - 1] forKey:@"mod"]; else [d removeObjectForKey:@"mod"];
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);   // the launcher then replaces itself with the game (exec)
}
- (void)start:(id)sender { (void)sender; [self save]; self.launch = YES; [NSApp stop:nil]; [self.window orderOut:nil];
    NSEvent *e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:e atStart:YES]; }
- (void)windowWillClose:(NSNotification *)n { (void)n; if (!self.launch) { [self save]; [NSApp terminate:nil]; } }
@end

static void fail(NSString *s) {
    NSAlert *a = [NSAlert new]; a.messageText = T("Bayonetta 3 no pudo arrancar", "Bayonetta 3 could not start"); a.informativeText = s; [a runModal];
}
static void pla_trace(const char *fmt, ...) {
    if (!getenv("PLA_TRACE")) return;   /* PLA_TRACE=1 writes the launcher's start-up steps to /tmp/pla_launcher_trace.log */
    FILE *f = fopen("/tmp/pla_launcher_trace.log", "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
static BOOL make_dir(NSString *p) { return [NSFileManager.defaultManager createDirectoryAtPath:p withIntermediateDirectories:YES attributes:nil error:nil]; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication]; NSApp.activationPolicy = NSApplicationActivationPolicyRegular;
        pla_trace("entry");
        Launcher *l = [Launcher new]; NSApp.delegate = l;
        /* PLA_AUTOSTART=1 (tests, packaging check): skip the window and go straight to the game, the way a player's
           Start click would. */
        if (getenv("PLA_AUTOSTART")) { l.launch = YES; NSApp.delegate = l; } else [NSApp run];
        if (!l.launch) return 0;
        NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
        pla_trace("launch=1");
        NSString *work = work_folder(), *home = NSHomeDirectory();
        pla_trace("work=%s", work.UTF8String);
        NSString *data = [home stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/PLA"];
        NSString *logs = [home stringByAppendingPathComponent:@"Library/Logs/MacNativeGames"];
        NSString *profile = [data stringByAppendingPathComponent:@"profile"], *slot = [data stringByAppendingPathComponent:@"game-slot"];
        pla_trace("data=%s", data.UTF8String);
        if (!make_dir(profile) || !make_dir(slot) || !make_dir(logs)) { pla_trace("make_dir failed %s", data.UTF8String); fail([NSString stringWithFormat:@"%@", data]); return 1; }
        if (chdir(work.fileSystemRepresentation)) { fail(work); return 1; }   // the runtime reads extracted/ and rewritten/ relative to it

        int fps = (int)[d integerForKey:@"fps"]; NSMutableArray *patches = [NSMutableArray array];
        // Native 60 fps: the runtime patches the game's frame step and presents every display tick (PLA_REALTIME=1).
        // It is opt-in since 2026-11-04: with the 60 fps step PLA advances twice as fast as real time (part of the
        // simulation steps once per update instead of by dt), so "Original 30" is the default and key 2 gives the
        // 60 Hz picture through 30+30 interpolation.
        if (fps >= 60) setenv("PLA_REALTIME", "1", 1); else setenv("PLA_REALTIME", "0", 1);
        if ([d boolForKey:@"fixed900"] && fixed900_patch()) [patches addObject:fixed900_patch()];
        if ([d boolForKey:@"lod"] && have_patch(@"improved-lod-v2")) [patches addObject:@"improved-lod-v2"];
        if ([d boolForKey:@"nograin"] && have_patch(@"remove-film-grain")) [patches addObject:@"remove-film-grain"];
        setenv("PLA_PATCHES", [patches componentsJoinedByString:@","].UTF8String, 1);
        setenv("DM_PRESENT_HZ", fps == 120 ? "120" : "60", 1);

        NSArray<NSScreen *> *all = NSScreen.screens; NSUInteger mi = [all indexOfObject:NSScreen.mainScreen];
        if (mi == NSNotFound) mi = 0;
        for (NSUInteger i = 0; i < all.count; i++) if ([screen_name(all[i]) isEqualToString:[d stringForKey:@"monitor"]]) mi = i;
        setenv("PLA_DISPLAY", [NSString stringWithFormat:@"%lu", (unsigned long)mi].UTF8String, 1);
        BOOL full = [d boolForKey:@"fullscreen"];
        if (full) {
            setenv("PLA_FULLSCREEN", "1", 1);
            int h = (int)[d integerForKey:@"output"]; NSSize px = screen_pixels(all.count ? all[mi] : NSScreen.mainScreen);
            if (h > 0 && h < px.height) setenv("TOTK_DRAWABLE_SCALE", [NSString stringWithFormat:@"%.4f", h / px.height].UTF8String, 1);
        }
        NSInteger s = [d integerForKey:@"size"]; if (s < 0 || s > 3) s = 2;
        setenv("TOTK_WINDOW_SIZE", [NSString stringWithFormat:@"%dx%d", sizes[s][0], sizes[s][1]].UTF8String, 1);
        if (![d objectForKey:@"metalfx"] || [d boolForKey:@"metalfx"]) { setenv("TOTK_MFX_UPSCALE", "1", 1); setenv("TOTK_MFX_OUTPUT", "1", 1); }
        if ([d objectForKey:@"hud"] && ![d boolForKey:@"hud"]) setenv("TOTK_FPS_OVERLAY", "0", 1); else setenv("PLA_HUD_RENDER", "1", 1);
        NSString *mod = [d stringForKey:@"mod"];
        if (mod && [romfs_mods() containsObject:mod]) setenv("TOTK_ROMFS_MODS", [mods_folder() stringByAppendingFormat:@"/%@/romfs", mod].fileSystemRepresentation, 1);
        else setenv("TOTK_ROMFS_MODS", "0", 1);

        setenv("TOTK_PROFILE_DIR", profile.fileSystemRepresentation, 1);
        setenv("TOTK_GAME_SLOT_DIR", slot.fileSystemRepresentation, 1);
        setenv("TOTK_ROMFS_PROOF_DIR", data.fileSystemRepresentation, 1);
        NSString *preferred = NSLocale.preferredLanguages.firstObject; if (preferred) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
        setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1); setenv("TOTK_GPU_WIDEN", "1", 1); setenv("TOTK_GPU_BLOCKRT", "1", 1);
        setenv("TOTK_GPU_MIDCLEAR", "1", 1); setenv("TOTK_GPU_BUDGET_MB", "5000", 1);
        setenv("DM_PRESENT_LINK_FALLBACK", "1", 1);   // keep presenting when macOS pauses the display link for a window it thinks covered
        setenv("TOTK_SLOT_OWNER", "user", 1); setenv("TOTK_SLOT_PURPOSE", "player", 1);
        unsetenv("TOTK_HEADLESS");
        { NSString *cmds = [data stringByAppendingPathComponent:@"input.txt"]; FILE *cf = fopen(cmds.fileSystemRepresentation, "w"); if (cf) fclose(cf);
          setenv("TOTK_INPUT_FILE", cmds.fileSystemRepresentation, 1); }   // echo "shot ~/Desktop/shot.png" >> it

        NSString *log = [logs stringByAppendingPathComponent:@"pla.log"], *prev = [logs stringByAppendingPathComponent:@"pla.previous.log"];
        [NSFileManager.defaultManager removeItemAtPath:prev error:nil]; [NSFileManager.defaultManager moveItemAtPath:log toPath:prev error:nil];
        FILE *lf = fopen(log.fileSystemRepresentation, "w"); if (lf) { dup2(fileno(lf), STDOUT_FILENO); dup2(fileno(lf), STDERR_FILENO); fclose(lf); }
        fprintf(stderr, "[launcher] work %s, patches %s, present %s Hz, display %s, fullscreen %s, window %s, drawable scale %s, MetalFX %s, mod %s\n",
                work.UTF8String, getenv("PLA_PATCHES"), getenv("DM_PRESENT_HZ"), getenv("PLA_DISPLAY"), full ? "yes" : "no", getenv("TOTK_WINDOW_SIZE"),
                getenv("TOTK_DRAWABLE_SCALE") ?: "1", getenv("TOTK_MFX_OUTPUT") ? "on" : "off", getenv("TOTK_ROMFS_MODS"));
        NSString *binary = [work stringByAppendingPathComponent:@"build/pla-native"];
        char *const args[] = {(char *)binary.fileSystemRepresentation, "rewritten/v1101", "boot", NULL};
        pla_trace("exec %s %s", args[0], args[1]);
        execv(args[0], args);
        fprintf(stderr, "[launcher] execv failed: %s\n", strerror(errno));
        fail([NSString stringWithFormat:T("No se pudo iniciar el juego. Mira %@.", "Starting the game failed. See %@."), log]);
        return 1;
    }
}
