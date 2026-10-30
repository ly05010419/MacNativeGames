// Native settings UI harness; no game content, keys, Metal device or guest code.
#import "../runtime/host/front_settings.h"
#include "../runtime/host/mac_display_settings.h"
#include "../runtime/host/front_shortcuts.h"
#include <stdio.h>
#include <string.h>
static DmMacSettings state = {.show_hints = 1, .dynamic_fps=1, .screen_fps=120, .output_width = 3840, .output_height = 2160};
static NSView *find(NSView *view, NSString *identifier) {
    if ([view.identifier isEqualToString:identifier]) return view;
    for (NSView *child in view.subviews) { NSView *result = find(child, identifier); if (result) return result; }
    return nil;
}
static int failures, checks;
#define CHECK(NAME, CONDITION) do { checks++; if (!(CONDITION)) { failures++; fprintf(stderr, "FAIL: %s\n", NAME); } } while (0)
static void change(DmMacSetting key, NSInteger value) {
    switch (key) {
        case DmSettingFpsTarget: state.fps_target=(int)value; break;
        case DmSettingDynamicFps: state.dynamic_fps=(int)value; break;
        case DmSettingStats: state.stats_mode=(int)value; break;
        case DmSettingFrameGraph: state.frame_graph=(int)value; break;
        case DmSettingOutput: { state.output_preset = (int)value; DmOutputSize s = dm_output_size(3840,2160,(int)value); state.output_width=s.width;state.output_height=s.height; break; }
        case DmSettingFaceLayout: state.face_layout = (int)value; break;
        case DmSettingSwapAB: state.swap_ab = (int)value; break;
        case DmSettingStartFullscreen: state.start_fullscreen = (int)value; break;
        case DmSettingHints: state.show_hints = (int)value; break;
        case DmSettingFullscreen: state.fullscreen = (int)value; break;
        case DmSettingResetController: state.face_layout=state.swap_ab=0; break;
    }
}
int main(int argc, const char **argv) { @autoreleasepool {
    BOOL test = argc > 1 && !strcmp(argv[1], "--self-test");
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow *parent = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1100,790)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    parent.title = @"Hyrule Warriors: Age of Calamity · Settings Preview · No game data"; [parent center];
    parent.contentView.wantsLayer=YES;parent.contentView.layer.backgroundColor=[NSColor colorWithRed:.07 green:.12 blue:.20 alpha:1].CGColor;
    NSTextField *title=[NSTextField labelWithString:@"Hyrule Warriors: Age of Calamity\nSettings panel preview"];
    title.textColor=NSColor.whiteColor;title.font=[NSFont systemFontOfSize:32 weight:NSFontWeightSemibold];title.frame=NSMakeRect(40,320,900,110);[parent.contentView addSubview:title];
    DmSettingsPanel *panel = [[DmSettingsPanel alloc] initWithParent:parent snapshot:^{return state;}
        change:^(DmMacSetting key, NSInteger value){change(key,value);}];
    DmShortcutHUD *hud=[[DmShortcutHUD alloc] initWithSnapshot:^{return state;} openSettings:^{[panel showPanel];}];
    hud.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;[parent.contentView addSubview:hud];[hud refresh];
    DmPerformanceHUD *performance=[[DmPerformanceHUD alloc] initWithSnapshot:^{return state;}];
    [parent.contentView addSubview:performance];[performance refresh];
    NSMenu *bar=[NSMenu new], *app=[NSMenu new];[[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
    [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];NSApp.mainMenu=bar;
    [parent makeKeyAndOrderFront:nil];[panel showPanel];
    if (test) {
        [panel.window.contentView layoutSubtreeIfNeeded];
        DmMacSettings original=state; DmMacSetting key; NSInteger value;
        CHECK("diagnostic panes start hidden",performance.hidden && state.stats_mode==0 && state.frame_graph==0);
        CHECK("F1 opens settings",dm_mac_shortcut(122,state,&key,&value)==DmShortcutSettings);
        const unsigned short gameplayKeys[]={13,0,1,2,38,40,32,34,12,14,6,8,3,5,36,48,49,123,124,125,126};
        for(unsigned i=0;i<sizeof(gameplayKeys)/sizeof(gameplayKeys[0]);i++)
            CHECK("shortcuts preserve gameplay keys",!dm_mac_is_shortcut(gameplayKeys[i]) && dm_mac_shortcut(gameplayKeys[i],state,&key,&value)==DmShortcutNone);
        for(NSNumber *target in @[@60,@0,@60,@0]) {
            CHECK("2 maps to FPS",dm_mac_shortcut(19,state,&key,&value)==DmShortcutChange && key==DmSettingFpsTarget);
            change(key,value);[hud refresh];
            CHECK("2 cycles original/60",state.fps_target==target.intValue);
            NSTextField *values=(id)find(hud,@"shortcut-values");
            CHECK("shortcut HUD shows FPS target",[values.stringValue containsString:target.intValue?target.stringValue:@"30"]);
        }
        state.fps_target=120;
        dm_mac_shortcut(19,state,&key,&value);
        CHECK("legacy 120 shortcut returns to 60",key==DmSettingFpsTarget && value==60);
        state=original;
        const unsigned short codes[]={18,20,21,23,22,26};
        const DmMacSetting settings[]={DmSettingHints,DmSettingOutput,DmSettingDynamicFps,DmSettingStats,DmSettingFaceLayout,DmSettingFrameGraph};
        const NSInteger values[]={0,1,0,1,1,1};
        for(unsigned i=0;i<sizeof(codes)/sizeof(codes[0]);i++) {
            CHECK("number key maps to correct action",dm_mac_shortcut(codes[i],state,&key,&value)==DmShortcutChange && key==settings[i] && value==values[i]);
        }
        state=original;state.stats_mode=2;state.frame_graph=1;[hud refresh];[performance refresh];
        [parent setContentSize:NSMakeSize(640,360)];[hud refresh];[performance refresh];
        CHECK("shortcut HUD fits minimum window",NSContainsRect(parent.contentView.bounds,hud.frame));
        CHECK("shortcut text fits its HUD",NSContainsRect(hud.bounds,find(hud,@"shortcut-values").frame));
        CHECK("shortcut and stats panes do not overlap",!NSIntersectsRect(hud.frame,performance.frame));
        [parent setContentSize:NSMakeSize(1100,790)];[hud refresh];[performance refresh];
        NSPopUpButton *fps=(id)find(panel.window.contentView,@"settings-fps");
        CHECK("fps control exists",fps!=nil && fps.numberOfItems==3);
        CHECK("broken 120 fps cannot be selected",[fps indexOfItemWithTag:120]==-1);
        for(NSNumber *target in @[@60,@30,@0]) {
            [fps selectItemWithTag:target.integerValue];[NSApp sendAction:fps.action to:fps.target from:fps];
            CHECK("fps target applies live",state.fps_target==target.intValue);
        }
        NSButton *dynamic=(id)find(panel.window.contentView,@"settings-dynamic-fps");
        CHECK("original mode keeps native timing",!dynamic.enabled);
        [fps selectItemWithTag:60];[NSApp sendAction:fps.action to:fps.target from:fps];
        [dynamic performClick:nil];CHECK("dynamic checkbox updates state",state.dynamic_fps==0);
        NSPopUpButton *stats=(id)find(panel.window.contentView,@"settings-stats");
        [stats selectItemWithTag:0];[NSApp sendAction:stats.action to:stats.target from:stats];[performance refresh];
        CHECK("stats can hide independently",performance.hidden && state.show_hints);
        [stats selectItemWithTag:2];[NSApp sendAction:stats.action to:stats.target from:stats];[performance refresh];
        CHECK("stats can be restored",!performance.hidden);
        NSButton *graph=(id)find(panel.window.contentView,@"settings-frame-graph");
        [graph performClick:nil];CHECK("frame graph checkbox updates state",!state.frame_graph);
        state.performance=(AocStats){.active=1,.display_active=1,.game_fps=118,.display_fps=117,.samples=3,.frame_ms=9,.p50_ms=8.5,.p95_ms=10,.p99_ms=11,.gpu_ms=5,
            .history={8,8.5,9},.shaders={.inventory=40729,.libraries=231,.render_pipelines=290,.compute_pipelines=28}};
        [panel refresh];[performance refresh];
        NSTextField *metrics=(id)find(panel.window.contentView,@"settings-performance");
        CHECK("live fps and shader counters reach panel",[metrics.stringValue containsString:@"118.0"] && [metrics.stringValue containsString:@"40729"] && [metrics.stringValue containsString:@"p99"]);
        CHECK("performance hud fits window",NSContainsRect(parent.contentView.bounds,performance.frame));
        NSPopUpButton *output=(id)find(panel.window.contentView,@"settings-output");
        CHECK("output control exists",output!=nil);
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
        NSButton *hints=(id)find(panel.window.contentView,@"settings-hints");[hints performClick:nil];[hud refresh];
        CHECK("hints hide HUD",state.show_hints==0&&hud.hidden);
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
