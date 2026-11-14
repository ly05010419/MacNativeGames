// Native settings UI harness; no game content, keys, Metal device or guest code.
#import "../runtime/host/front_settings.h"
#import "../runtime/host/front_hud.h"
#include "../runtime/host/mac_display_settings.h"
#include "../runtime/host/front_shortcuts.h"
#include "../runtime/host/shader_activity.h"
#include "../runtime/host/hud_settings.h"
#include "../runtime/host/front_graph.h"
#import "../runtime/host/pso_export.h"
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
static DmMacSettings state = {.screen_fps=120, .output_width = 3840, .output_height = 2160};
static DmHud saved; static DmHudImpose imposed; static int collecting;   // as front_mac.m keeps them
static NSView *find(NSView *view, NSString *identifier) {
    if ([view.identifier isEqualToString:identifier]) return view;
    for (NSView *child in view.subviews) { NSView *result = find(child, identifier); if (result) return result; }
    return nil;
}
static int failures, checks;
#define CHECK(NAME, ...) do { checks++; if (!(__VA_ARGS__)) { failures++; fprintf(stderr, "FAIL: %s\n", NAME); } } while (0)
static void apply(void) { DmHud e = dm_hud_effective(saved, imposed); state.hud_on = e.on; state.hud_lines = e.lines; collecting = dm_hud_needs_stats(e); }
// front_mac.m settings_changed, the corner box's part: the same hud_settings.h calls, then statistics collection follows
static void change(DmMacSetting key, NSInteger value) {
    switch (key) {
        case DmSettingHudOn: dm_hud_set_on(&saved, &imposed, value != 0); break;
        case DmSettingHudKey: dm_hud_key(&saved, &imposed, (int)value & HUD_LINES_ALL); break;
        case DmSettingHudLine: dm_hud_set_line(&saved, &imposed, (int)(value >> 1) & HUD_LINES_ALL, (int)(value & 1)); break;
        case DmSettingOutput: { state.output_preset = (int)value; DmOutputSize s = dm_output_size(3840,2160,(int)value); state.output_width=s.width;state.output_height=s.height; break; }
        case DmSettingFaceLayout: state.face_layout = (int)value; break;
        case DmSettingSwapAB: state.swap_ab = (int)value; break;
        case DmSettingStartFullscreen: state.start_fullscreen = (int)value; break;
        case DmSettingFullscreen: state.fullscreen = (int)value; break;
        case DmSettingResetController: state.face_layout=state.swap_ab=0; break;
        case DmSettingFpsTarget: case DmSettingDynamicFps: break;
    }
    apply();
}
static int key(unsigned short code, int repeat) {   // a key press as the game window takes it; 1 when it changed something
    DmMacSetting k; NSInteger v; DmShortcutAction a = dm_mac_shortcut(code, repeat, state, &k, &v);
    if (a == DmShortcutChange) change(k, v);
    return a == DmShortcutChange;
}
static int saved_as(const char *want) { char out[128]; dm_hud_format(saved, out, sizeof out); return !strcmp(out, want); }
static int shown(void) { return dm_hud_visible((DmHud){state.hud_on, state.hud_lines}); }
// Settings files written by earlier builds, as found in this Mac's disposable profiles (runs/s1, validation/ind-2)
static const char *OLD_S1 = "layout=auto\nswap_ab=0\nfullscreen=0\noutput=0\nhints=1\nfps_target=60\ndynamic_fps=0\nstats=1\nframe_graph=1\n";
static const char *OLD_IND2 = "layout=auto\nswap_ab=0\nfullscreen=0\noutput=2\nhints=1\nstats=1\nframe_graph=0\nfps_target=30\ndynamic_fps=1\n";
static void hud_rules(void) {
    DmHud h = dm_hud_parse(NULL); CHECK("no settings file: the box starts hidden", !h.on && h.lines == HUD_DEFAULT_LINES);
    h = dm_hud_parse(OLD_S1); CHECK("old file s1: shortcut list, FPS, shaders and graph", h.on && h.lines == (HUD_KEYS | HUD_FPS | HUD_SHADERS | HUD_GRAPH));
    h = dm_hud_parse(OLD_IND2); CHECK("old file ind-2: shortcut list, FPS and shaders", h.on && h.lines == (HUD_KEYS | HUD_FPS | HUD_SHADERS));
    h = dm_hud_parse("hints=0\nstats=2\nframe_graph=0\n"); CHECK("old stats=2 adds timing", h.on && h.lines == (HUD_FPS | HUD_SHADERS | HUD_TIMING));
    h = dm_hud_parse("hints=0\nstats=0\nframe_graph=0\n"); CHECK("old file that showed nothing: hidden", !h.on && h.lines == HUD_DEFAULT_LINES);
    h = dm_hud_parse("layout=auto\n"); CHECK("old file without hints= showed the shortcuts", h.on && h.lines == HUD_KEYS);
    h = dm_hud_parse("hud=1\nhud_lines=fps,graph\nhints=1\nstats=2\n"); CHECK("new keys win over old ones", h.on && h.lines == (HUD_FPS | HUD_GRAPH));
    h = dm_hud_parse("hud=1\nhud_lines=\n"); CHECK("an empty line list stays empty", h.on && !h.lines && !dm_hud_visible(h));
    h = dm_hud_parse(" hud = 1 \r\nhud_lines=keys,unknown,timing\r\n"); CHECK("spaces, CRLF and unknown names", h.on && h.lines == (HUD_KEYS | HUD_TIMING));
    saved = dm_hud_parse(OLD_S1); CHECK("an old file is saved with the new keys only", saved_as("hud=1\nhud_lines=fps,shaders,graph,keys\n"));
    DmHudImpose m = dm_hud_impose("0", NULL); CHECK("DM_AOC_STATS=0 hides FPS, shaders, timing", m.mask == (HUD_FPS | HUD_SHADERS | HUD_TIMING) && !m.value && !m.on && !m.invalid);
    m = dm_hud_impose("1", NULL); CHECK("DM_AOC_STATS=1 shows FPS and shaders", m.value == (HUD_FPS | HUD_SHADERS) && m.on);
    m = dm_hud_impose("2", "1"); CHECK("DM_AOC_STATS=2 and DM_AOC_FRAME_GRAPH=1", m.value == (HUD_FPS | HUD_SHADERS | HUD_TIMING | HUD_GRAPH) && m.on);
    m = dm_hud_impose("x", "0"); CHECK("an invalid DM_AOC_STATS counts as 0; DM_AOC_FRAME_GRAPH=0 hides the graph",
                                       m.invalid && !m.value && m.mask == HUD_LINES_ALL - HUD_KEYS && !m.on);
    m = dm_hud_impose(NULL, NULL); CHECK("control: no environment imposes nothing", !m.mask && !m.on);
    saved = (DmHud){0, HUD_KEYS}; imposed = dm_hud_impose("2", NULL); apply();
    CHECK("an imposition shows the box this run", state.hud_on && state.hud_lines == (HUD_KEYS | HUD_FPS | HUD_SHADERS | HUD_TIMING) && collecting);
    change(DmSettingFaceLayout, 0); CHECK("saving another setting keeps the player's own box", saved_as("hud=0\nhud_lines=keys\n"));
    key(19, 0); CHECK("a key on an imposed line ends its imposition", !(state.hud_lines & HUD_FPS) && !(imposed.mask & HUD_FPS) && (imposed.mask & HUD_SHADERS));
    CHECK("untouched imposed lines are saved with the player's value", saved_as("hud=0\nhud_lines=keys\n"));
    imposed = (DmHudImpose){0}; saved = (DmHud){1, HUD_FPS | HUD_GRAPH}; apply();
    CHECK("key 1 hides the box", key(18, 0) && !shown() && !collecting);
    CHECK("hidden, the lines are kept", state.hud_lines == (HUD_FPS | HUD_GRAPH));
    CHECK("key 1 shows the same lines again", key(18, 0) && state.hud_on && state.hud_lines == (HUD_FPS | HUD_GRAPH) && collecting);
    key(18, 0); key(20, 0); CHECK("a line key with the box hidden shows the box with that line", state.hud_on && state.hud_lines == (HUD_FPS | HUD_GRAPH | HUD_SHADERS));
    key(19, 0); key(20, 0); key(23, 0); CHECK("removing the last line hides the box", !shown() && !collecting);
    key(18, 0); CHECK("key 1 on an empty box brings the default lines", state.hud_on && state.hud_lines == HUD_DEFAULT_LINES);
    saved = (DmHud){1, HUD_KEYS}; apply(); CHECK("the shortcut list alone collects nothing", !collecting);
    key(23, 0); CHECK("the graph collects statistics", collecting);
    DmHud before = saved; CHECK("a held key's repeats change nothing", !key(23, 1) && !key(18, 1) && !memcmp(&before, &saved, sizeof saved));
    DmMacSetting k; NSInteger v;
    CHECK("F1 opens settings", dm_mac_shortcut(122, 0, state, &k, &v) == DmShortcutSettings);
    const unsigned short codes[] = {18, 19, 20, 21, 23, 22}; const int bits[] = {0, HUD_FPS, HUD_SHADERS, HUD_TIMING, HUD_GRAPH, HUD_KEYS};
    for (unsigned i = 0; i < 6; i++)
        CHECK("keys 1-6 are the box and its lines", dm_mac_is_shortcut(codes[i]) && dm_mac_shortcut(codes[i], 0, state, &k, &v) == DmShortcutChange && k == DmSettingHudKey && v == bits[i]);
    CHECK("7 is no shortcut any more", !dm_mac_is_shortcut(26) && dm_mac_shortcut(26, 0, state, &k, &v) == DmShortcutNone);
    const unsigned short gameplayKeys[] = {13,0,1,2,38,40,32,34,12,14,6,8,3,5,36,48,49,123,124,125,126};
    for (unsigned i = 0; i < sizeof gameplayKeys / sizeof gameplayKeys[0]; i++)
        CHECK("shortcuts preserve gameplay keys", !dm_mac_is_shortcut(gameplayKeys[i]) && dm_mac_shortcut(gameplayKeys[i], 0, state, &k, &v) == DmShortcutNone);
    CHECK("graph: a 16.7 ms frame is 60 FPS", fabs(dm_graph_fps(1000.0/60, DM_GRAPH_TOP_FPS) - 60) < 1e-9);   // the box's graph, 0 to 60 FPS
    CHECK("graph: a 33.3 ms frame is 30 FPS", fabs(dm_graph_fps(1000.0/30, DM_GRAPH_TOP_FPS) - 30) < 1e-9);
    CHECK("graph: 100 ms is 10 FPS", fabs(dm_graph_fps(100, DM_GRAPH_TOP_FPS) - 10) < 1e-9);
    CHECK("graph: a frame faster than 60 FPS is drawn at the top", dm_graph_fps(1000.0/120, DM_GRAPH_TOP_FPS) == 60);
    CHECK("graph: no time is 0 FPS", dm_graph_fps(0, DM_GRAPH_TOP_FPS) == 0 && dm_graph_fps(-1, DM_GRAPH_TOP_FPS) == 0);
    DmShaderActivity a = {0};   // the colour rule with injected monotonic seconds
    CHECK("first sample only sets the start", dm_activity_step(&a,500,900,0,10)==DM_ACTIVITY_GREEN);
    CHECK("control: no rise stays green", dm_activity_step(&a,500,900,0,11)==DM_ACTIVITY_GREEN);
    CHECK("a pipeline build attempt lights yellow", dm_activity_step(&a,500,901,0,12)==DM_ACTIVITY_YELLOW);
    CHECK("still yellow at 2.9 s", dm_activity_step(&a,500,901,0,14.9)==DM_ACTIVITY_YELLOW);
    CHECK("green again at 3 s", dm_activity_step(&a,500,901,0,15)==DM_ACTIVITY_GREEN);
    CHECK("errors stay orange over activity", dm_activity_step(&a,502,901,1,20.5)==DM_ACTIVITY_ORANGE);
    CHECK("a counter going down is a new start", dm_activity_step(&a,10,10,0,21)==DM_ACTIVITY_GREEN);
    dm_activity_reset(&a); CHECK("after a reset, grown counters are a new start", dm_activity_step(&a,900,2000,0,21.5)==DM_ACTIVITY_GREEN);
}
static NSString *r_row(const char *fs) {   // a well-formed render pipeline row (pso_export.h layout)
    uint8_t b[DM_PSO_SKEY_SIZE] = {0}; memcpy(b + DM_PSO_VS, "00000000000000aa_b", 18); if (fs) memcpy(b + DM_PSO_FS, fs, strlen(fs)); b[DM_PSO_FS_STAGE] = 1;
    NSMutableString *s = [NSMutableString stringWithString:@"R\t"]; for (int i = 0; i < DM_PSO_SKEY_SIZE; i++) [s appendFormat:@"%02x", b[i]];
    return s;
}
static void export_rules(DmSettingsPanel *panel) {   // the settings window's Export shader list, on files of its own
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID UUID].UUIDString], *ro = [dir stringByAppendingPathComponent:@"read-only"];
    [NSFileManager.defaultManager createDirectoryAtPath:ro withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *user = [dir stringByAppendingPathComponent:@"pso_user.tsv"], *seed = [dir stringByAppendingPathComponent:@"pso_manifest.tsv"];
    NSString *A = @"S\t00000000000000a1_b\t0", *B = @"C\t00000000000000b1_b", *C = r_row("00000000000000bb_b"), *D = r_row(NULL);
    [[NSString stringWithFormat:@"# seed\n%@\n%@\n", A, B] writeToFile:seed atomically:YES encoding:NSUTF8StringEncoding error:nil];
    void (^put)(NSString *) = ^(NSString *text) { [text writeToFile:user atomically:YES encoding:NSUTF8StringEncoding error:nil]; };
    DmPsoExport *e = [DmPsoExport fromUserPath:user seedPath:seed]; CHECK("no user list yet: nothing to export", !e.error && !e.rows);
    put([NSString stringWithFormat:@"%@\n%@\n%@\n%@\n", A, C, C, D]);
    e = [DmPsoExport fromUserPath:user seedPath:seed]; CHECK("valid, unique rows the seed lacks", !e.error && e.rows == 2 && [e.body isEqualToString:[NSString stringWithFormat:@"%@\n%@\n", C, D]]);
    put([NSString stringWithFormat:@"%@\n%@\nS\t00000000000000e1", C, D]);
    e = [DmPsoExport fromUserPath:user seedPath:seed]; CHECK("a row still being written is left out and named", !e.error && e.rows == 2 && e.partialTail);
    put([NSString stringWithFormat:@"%@\nS\t00000000000000e1_b\t9\n%@\n", C, D]);
    e = [DmPsoExport fromUserPath:user seedPath:seed]; NSString *bad = [dir stringByAppendingPathComponent:@"bad.tsv"];
    CHECK("an invalid row stops the export with its line", [e.error isEqualToString:@"line 2: stage not 0-5"] && [e writeTo:[NSURL fileURLWithPath:bad] date:NSDate.date] &&
          ![NSFileManager.defaultManager fileExistsAtPath:bad]);
    put([NSString stringWithFormat:@"%@\n%@\n", C, r_row("00000000000000BB_b")]);
    CHECK("an uppercase key in a pipeline is refused", [[DmPsoExport fromUserPath:user seedPath:seed].error hasPrefix:@"line 2:"]);
    put([NSString stringWithFormat:@"%@\n", C]); panel.shaderLists = @[user, seed]; [panel refresh];
    NSButton *button = (id)find(panel.window.contentView, @"settings-export-shaders");
    CHECK("the window counts the new rows and offers the export", button.enabled);
    put([NSString stringWithFormat:@"%@\n%@\n", C, D]);   // the game wrote another row after the window counted
    NSURL *out = [NSURL fileURLWithPath:[dir stringByAppendingPathComponent:@"ss-shaders.tsv"]];
    CHECK("cancelling writes nothing", ![panel finishExport:NSModalResponseCancel url:out] && ![NSFileManager.defaultManager fileExistsAtPath:out.path]);
    NSString *said = [panel finishExport:NSModalResponseOK url:out];
    NSString *file = [NSString stringWithContentsOfFile:out.path encoding:NSUTF8StringEncoding error:nil];
    CHECK("the export counts again and says nothing was sent", [said hasPrefix:@"Saved 2 rows to ss-shaders.tsv"] && [said containsString:@"Nothing was sent"]);
    CHECK("the file holds a header and the rows", [file hasPrefix:@"# The Legend of Zelda: Skyward Sword HD 1.0.1, shader list contribution exported "] &&
          [file containsString:@": 2 rows the app's list lacks.\n"] && [file hasSuffix:[NSString stringWithFormat:@"\n%@\n%@\n", C, D]]);
    CHECK("exporting leaves the player's list as it was", [[NSString stringWithContentsOfFile:user encoding:NSUTF8StringEncoding error:nil] isEqualToString:[NSString stringWithFormat:@"%@\n%@\n", C, D]]);
    chmod(ro.fileSystemRepresentation, 0555);
    NSURL *denied = [NSURL fileURLWithPath:[ro stringByAppendingPathComponent:@"x.tsv"]]; said = [panel finishExport:NSModalResponseOK url:denied];
    chmod(ro.fileSystemRepresentation, 0755);   // restored before any check
    CHECK("a write error is shown, not a success", [said hasPrefix:@"Not exported: could not write x.tsv"] && ![NSFileManager.defaultManager fileExistsAtPath:denied.path]);
    put(@"X\tbroken\n"); [panel refresh]; CHECK("an unreadable list offers no export", !button.enabled);
    panel.shaderLists = nil; [panel refresh]; CHECK("no shader map named: no export", !button.enabled);
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
}
int main(int argc, const char **argv) { @autoreleasepool {
    BOOL test = argc > 1 && !strcmp(argv[1], "--self-test");
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow *parent = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1100,790)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    parent.title = @"The Legend of Zelda: Skyward Sword HD · Settings Preview · No game data"; [parent center];
    parent.contentView.wantsLayer=YES;parent.contentView.layer.backgroundColor=[NSColor colorWithRed:.07 green:.12 blue:.20 alpha:1].CGColor;
    saved = dm_hud_parse(OLD_S1); apply();
    DmSettingsPanel *panel = [[DmSettingsPanel alloc] initWithParent:parent snapshot:^{return state;}
        change:^(DmMacSetting k, NSInteger value){change(k,value);}];
    DmCornerHUD *hud = [[DmCornerHUD alloc] initWithSnapshot:^{return state;}];
    [parent.contentView addSubview:hud]; [hud refresh];
    NSMenu *bar=[NSMenu new], *app=[NSMenu new];[[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
    [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];NSApp.mainMenu=bar;
    [parent makeKeyAndOrderFront:nil];[panel showPanel];
    if (test) {
        hud_rules(); export_rules(panel);
        [panel.window.contentView layoutSubtreeIfNeeded];
        state.performance=(AocStats){.active=1,.display_active=1,.game_fps=118,.display_fps=117,.samples=3,.frame_ms=9,.p50_ms=8.5,.p95_ms=10,.p99_ms=11,.gpu_ms=5,
            .history={8,8.5,9},.shaders={.inventory=40729,.libraries=231,.render_pipelines=290,.compute_pipelines=28,.library_loads=300,.pipeline_builds=700,.library_ms=4.5,.pipeline_ms=2.5}};
        saved = (DmHud){1, HUD_LINES_ALL}; imposed = (DmHudImpose){0}; apply();
        [parent setContentSize:NSMakeSize(640,360)]; [hud refresh];
        CHECK("the whole box fits the minimum window", !hud.hidden && NSContainsRect(parent.contentView.bounds, hud.frame));
        CHECK("the box sits in the top-left corner", NSMinX(hud.frame) <= 12.5 && NSMaxY(parent.contentView.bounds) - NSMaxY(hud.frame) <= 12.5);
        [parent setContentSize:NSMakeSize(1100,790)];
        saved = (DmHud){1, HUD_FPS | HUD_SHADERS}; apply();
        DmCornerHUD *fresh = [[DmCornerHUD alloc] initWithSnapshot:^{return state;}]; [parent.contentView addSubview:fresh]; [fresh refresh];
        NSTextField *stats = (id)find(fresh, @"hud-stats"), *keys = (id)find(fresh, @"hud-keys");
        CHECK("a new box with counted work starts green", [stats.textColor isEqual:NSColor.systemGreenColor]);
        CHECK("FPS and shader lines, no timing", [stats.stringValue containsString:@"118.0"] && [stats.stringValue containsString:@"On-demand library loads 300 · Ready 231"] &&
              [stats.stringValue containsString:@"On-demand pipeline builds 700 · Ready 318"] && ![stats.stringValue containsString:@"p99"] && ![stats.stringValue containsString:@" ms"]);
        CHECK("only the chosen parts show", !stats.hidden && keys.hidden);
        state.performance.shaders.pipeline_builds = 701; [fresh refresh];
        CHECK("the box turns yellow on a new build attempt", [stats.textColor isEqual:NSColor.systemYellowColor]);
        state.performance.shaders.pipeline_errors = 1; [fresh refresh];
        CHECK("errors keep it orange during activity", [stats.textColor isEqual:NSColor.systemOrangeColor]);
        state.performance.shaders.pipeline_errors = 0; key(18, 0); [fresh refresh];
        CHECK("key 1 hides the box view", fresh.hidden);
        state.performance.shaders.pipeline_builds = 800; key(18, 0); [fresh refresh];
        CHECK("shown again with grown counters: green", !fresh.hidden && [stats.textColor isEqual:NSColor.systemGreenColor]);
        key(21, 0); key(22, 0); [fresh refresh];
        CHECK("timing adds the times; the shortcut list shows", [stats.stringValue containsString:@"p99"] && [stats.stringValue containsString:@"800 / 2.5 ms"] && !keys.hidden);
        key(20, 0); state.performance.shaders.pipeline_builds = 900; [fresh refresh]; key(20, 0); [fresh refresh];
        CHECK("shaders line back after counting meanwhile: green", [stats.textColor isEqual:NSColor.systemGreenColor]);
        [panel refresh];
        NSButton *box = (id)find(panel.window.contentView, @"settings-hud"), *graphLine = (id)find(panel.window.contentView, @"settings-hud-graph");
        CHECK("settings show the box and its lines", box && box.state == NSControlStateValueOn && graphLine && graphLine.state == NSControlStateValueOff);
        CHECK("the graph's row says it shows FPS", [graphLine.title isEqualToString:@"FPS graph (5)"]);
        [graphLine performClick:nil]; CHECK("a line checkbox adds its line", (state.hud_lines & HUD_GRAPH) && collecting);
        [box performClick:nil]; CHECK("the box checkbox hides it, lines kept", !shown() && (state.hud_lines & HUD_GRAPH));
        [box performClick:nil];
        CHECK("no stats pane, FPS target, step or hints controls", !find(panel.window.contentView,@"settings-stats") && !find(panel.window.contentView,@"settings-fps") &&
              !find(panel.window.contentView,@"settings-dynamic-fps") && !find(panel.window.contentView,@"settings-hints"));
        NSTextField *metrics=(id)find(panel.window.contentView,@"settings-performance");
        CHECK("every counter reaches the settings window", [metrics.stringValue containsString:@"118.0"] && [metrics.stringValue containsString:@"40729"] && [metrics.stringValue containsString:@"p99"] &&
              [metrics.stringValue containsString:@"Prewarm imported"] && ![metrics.stringValue containsString:@"Target"]);
        NSPopUpButton *output=(id)find(panel.window.contentView,@"settings-output");
        CHECK("settings remain inside the display work area", NSContainsRect(NSInsetRect((parent.screen ?: NSScreen.mainScreen).visibleFrame,-1,-1),panel.window.frame));
        [output selectItemWithTag:2];[NSApp sendAction:output.action to:output.target from:output];
        CHECK("1080p control applies output cap",state.output_preset==2&&state.output_width==1920&&state.output_height==1080);
        NSPopUpButton *face=(id)find(panel.window.contentView,@"settings-controller");
        [face selectItemWithTag:2];[NSApp sendAction:face.action to:face.target from:face];
        CHECK("Nintendo labels map to label mode",state.face_layout==2);
        NSButton *swap=(id)find(panel.window.contentView,@"settings-swap-ab");[swap performClick:nil];
        CHECK("A/B checkbox updates state",state.swap_ab==1);
        NSButton *startup=(id)find(panel.window.contentView,@"settings-start-fullscreen");[startup performClick:nil];
        CHECK("start-fullscreen checkbox updates state",state.start_fullscreen==1);
        [panel.window performClose:nil];[panel showPanel];
        CHECK("reopening preserves selected layout",face.selectedItem.tag==2&&swap.state==NSControlStateValueOn);
        NSButton *taa=(id)find(panel.window.contentView,@"settings-metalfx-unavailable");
        CHECK("unsupported MetalFX cannot be enabled",taa&&!taa.enabled&&taa.state==NSControlStateValueOff);
        DmOutputSize s=dm_output_size(3840,2160,0);CHECK("automatic keeps native pixels",s.width==3840&&s.height==2160);
        s=dm_output_size(1280,720,4);CHECK("cap never upscales small windows",s.width==1280&&s.height==720);
        s=dm_output_size(2560,1504,2);CHECK("non-16:9 window preserves aspect",s.height==1080&&fabs((double)s.width/s.height-2560.0/1504)<.002);
        s=dm_output_size(3840,2160,99);CHECK("invalid preset falls back to automatic",s.width==3840&&s.height==2160);
        s=dm_output_size(0,0,2);CHECK("zero-sized view cannot create zero texture",s.width==1&&s.height==1);
        printf("settings UI: %d checks, %d failed\n",checks,failures);return failures?1:0;
    }
    [NSApp activateIgnoringOtherApps:YES];[NSApp run];return 0;
}}
