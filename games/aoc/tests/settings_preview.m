// Native settings UI harness; no game content, keys, Metal device or guest code.
#import "../runtime/host/front_settings.h"
#include "../runtime/host/mac_display_settings.h"
#include "../runtime/host/front_shortcuts.h"
#include "../runtime/host/front_keys.h"
#include "../runtime/host/front_prefs.h"
#include "../runtime/host/front_settings_file.h"
#include "../runtime/host/shader_activity.h"
#include "../runtime/host/front_graph.h"
#include <stdio.h>
#include <string.h>
static DmMacSettings state = {.dynamic_fps=1, .screen_fps=120, .output_width = 3840, .output_height = 2160, .aspect_available = 1};
static NSView *find(NSView *view, NSString *identifier) {
    if ([view.identifier isEqualToString:identifier]) return view;
    for (NSView *child in view.subviews) { NSView *result = find(child, identifier); if (result) return result; }
    return nil;
}
static NSTextField *find_text(NSView *view, NSString *text) {
    if ([view isKindOfClass:NSTextField.class] && [((NSTextField *)view).stringValue containsString:text]) return (NSTextField *)view;
    for (NSView *child in view.subviews) { NSTextField *result = find_text(child, text); if (result) return result; }
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
        case DmSettingAspect: state.aspect=(int)value; break;
        case DmSettingOutput: { state.output_preset = (int)value; DmOutputSize s = dm_output_size(3840,2160,(int)value); state.output_width=s.width;state.output_height=s.height; break; }
        case DmSettingFaceLayout: state.face_layout = (int)value; break;
        case DmSettingSwapAB: state.swap_ab = (int)value; break;
        case DmSettingStartFullscreen: state.start_fullscreen = (int)value; break;
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
    DmPerformanceHUD *performance=[[DmPerformanceHUD alloc] initWithSnapshot:^{return state;}];
    [parent.contentView addSubview:performance];[performance refresh];
    NSMenu *bar=[NSMenu new], *app=[NSMenu new];[[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
    [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];NSApp.mainMenu=bar;
    [parent makeKeyAndOrderFront:nil];[panel showPanel];
    if (test) {
        [panel.window.contentView layoutSubtreeIfNeeded];
        DmMacSettings original=state;
        CHECK("diagnostic panes start hidden",performance.hidden && state.stats_mode==0 && state.frame_graph==0);
        // Key 1 (and F1) opens and closes the panel, as in Tears of the Kingdom; 2 to 7 are no longer shortcuts
        CHECK("F1 opens settings",dm_mac_shortcut(DM_KEY_F1)==DmShortcutSettings);
        CHECK("1 opens settings",dm_mac_shortcut(DM_KEY_1)==DmShortcutSettings);
        const unsigned short gameplayKeys[]={13,0,1,2,38,40,32,34,12,14,6,8,3,5,36,48,49,123,124,125,126};
        for(unsigned i=0;i<sizeof(gameplayKeys)/sizeof(gameplayKeys[0]);i++)
            CHECK("shortcuts preserve gameplay keys",!dm_mac_is_shortcut(gameplayKeys[i]) && dm_mac_shortcut(gameplayKeys[i])==DmShortcutNone &&
                  dm_mac_game_key(gameplayKeys[i],0,YES,NO)==DmKeyToGame);
        const unsigned short retired[]={19,20,21,23,22,26};   // 2 3 4 5 6 7
        for(unsigned i=0;i<sizeof(retired)/sizeof(retired[0]);i++)
            CHECK("2 to 7 are neither shortcuts nor game buttons",!dm_mac_is_shortcut(retired[i]) && dm_key_bit(retired[i])<0);
        CHECK("1 pressed toggles the panel",dm_mac_game_key(DM_KEY_1,0,YES,NO)==DmKeyTogglePanel);
        CHECK("1 held: its repeats do nothing",dm_mac_game_key(DM_KEY_1,0,YES,YES)==DmKeySwallow);
        CHECK("1 released does not reach the game",dm_mac_game_key(DM_KEY_1,0,NO,NO)==DmKeySwallow);
        CHECK("F1 pressed toggles the panel",dm_mac_game_key(DM_KEY_F1,0,YES,NO)==DmKeyTogglePanel);
        CHECK("Command-1 goes to the menu",dm_mac_game_key(DM_KEY_1,NSEventModifierFlagCommand,YES,NO)==DmKeyToSystem);
        CHECK("a release with Command held still reaches the game",dm_mac_game_key(13,NSEventModifierFlagCommand,NO,NO)==DmKeyToGame);
        CHECK("a gameplay key's repeats are swallowed",dm_mac_game_key(13,0,YES,YES)==DmKeySwallow);
        CHECK("on the panel 1 closes",dm_mac_panel_key(DM_KEY_1,0,NO)==DmPanelKeyClose && dm_mac_panel_key(DM_KEY_F1,0,NO)==DmPanelKeyClose);
        CHECK("on the panel a held 1 does not close and reopen",dm_mac_panel_key(DM_KEY_1,0,YES)==DmPanelKeySwallow);
        CHECK("on the panel other keys reach its controls",dm_mac_panel_key(48,0,NO)==DmPanelKeyPass && dm_mac_panel_key(DM_KEY_ESC,0,NO)==DmPanelKeyPass &&
              dm_mac_panel_key(DM_KEY_1,NSEventModifierFlagCommand,NO)==DmPanelKeyPass);
        // No key stays held: W and J down, 1 opens the panel (the game window lets go of the keyboard), W and J are released on
        // the panel, 1 closes it and its release is swallowed by the game window
        DmKeyboard kb={0};dm_keyboard_key(&kb,13,1);dm_keyboard_key(&kb,38,1);
        CHECK("held W and J hold input",kb.w && kb.buttons==1);
        dm_keyboard_key(&kb,DM_KEY_1,1);CHECK("1 is not a game button",kb.w && kb.buttons==1);
        if(dm_mac_game_key(DM_KEY_1,0,YES,NO)==DmKeyTogglePanel) dm_keyboard_release(&kb);
        CHECK("opening the panel releases every key",dm_keyboard_idle(&kb));
        if(dm_mac_game_key(DM_KEY_1,0,NO,NO)==DmKeyToGame) dm_keyboard_key(&kb,DM_KEY_1,0);
        CHECK("closing the panel leaves nothing held",dm_keyboard_idle(&kb));
        [panel.window performClose:nil];
        CHECK("toggle opens the panel",([panel togglePanel],panel.window.visible));
        CHECK("toggle closes the panel",([panel togglePanel],!panel.window.visible));
        [panel togglePanel];
        NSEvent *(^key)(unsigned short,NSString *,BOOL)=^(unsigned short code,NSString *chars,BOOL repeat){
            return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:panel.window.windowNumber
                context:nil characters:chars charactersIgnoringModifiers:chars isARepeat:repeat keyCode:code];};
        [panel.window keyDown:key(DM_KEY_1,@"1",YES)];CHECK("a repeated 1 leaves the panel open",panel.window.visible);
        [panel.window keyDown:key(DM_KEY_1,@"1",NO)];CHECK("1 on the panel closes it",!panel.window.visible);
        [panel showPanel];[panel.window keyDown:key(DM_KEY_F1,@"\uF704",NO)];CHECK("F1 on the panel closes it",!panel.window.visible);
        [panel showPanel];
        // Precedence (front_prefs.h): the variable decides the run and locks the row; the saved choice is kept and written back
        int rej;DmPref p=dm_pref_resolve(2,NULL,2,0,&rej);
        CHECK("no variable: the saved stats choice",p.value==2&&!p.locked&&!rej);
        p=dm_pref_resolve(2,"1",2,0,&rej);
        CHECK("DM_AOC_STATS=1 decides the run",p.value==1&&p.locked&&p.saved==2&&!rej);
        CHECK("a locked row refuses a change",!dm_pref_set(&p,0)&&p.saved==2&&p.value==1);
        p=dm_pref_resolve(p.saved,NULL,2,0,&rej);CHECK("next run without the variable: the saved choice again",p.value==2&&!p.locked);
        for(NSString *bad in @[@"3",@"x",@"10",@"",@"-1"]){p=dm_pref_resolve(2,bad.UTF8String,2,0,&rej);
            CHECK("an invalid DM_AOC_STATS is ignored",p.value==2&&!p.locked&&rej);}
        p=dm_pref_resolve(-1,NULL,2,0,&rej);CHECK("missing from the file: default Off",p.value==0&&p.saved==0);
        p=dm_pref_resolve(7,NULL,2,0,&rej);CHECK("an out-of-range saved value: default Off",p.value==0&&p.saved==0);
        CHECK("an unlocked change is saved",dm_pref_set(&p,1)&&p.saved==1&&p.value==1);
        p=dm_pref_resolve(0,"1",1,0,&rej);CHECK("DM_AOC_FRAME_GRAPH=1 decides the graph",p.value==1&&p.locked&&p.saved==0);
        p=dm_pref_resolve(1,"2",1,0,&rej);CHECK("DM_AOC_FRAME_GRAPH=2 is ignored",p.value==1&&!p.locked&&rej);
        p=dm_pref_resolve(1,"0",1,0,&rej);CHECK("DM_AOC_FRAME_GRAPH=0 turns it off for the run",p.value==0&&p.locked&&p.saved==1);
        state.stats_locked=state.graph_locked=1;[panel refresh];
        NSPopUpButton *lockedStats=(id)find(panel.window.contentView,@"settings-stats");NSButton *lockedGraph=(id)find(panel.window.contentView,@"settings-frame-graph");
        CHECK("rows the environment decides are greyed and say why",!lockedStats.enabled&&!lockedGraph.enabled&&[lockedStats.toolTip containsString:@"DM_AOC_STATS"]&&
              [lockedGraph.toolTip containsString:@"DM_AOC_FRAME_GRAPH"]);
        state.stats_locked=state.graph_locked=0;[panel refresh];
        CHECK("rows come back without the variables",lockedStats.enabled&&lockedGraph.enabled&&!lockedStats.toolTip);
        // controller-layout.txt through the real file (front_settings_file.h, as front_mac.m loads and saves it): a player's file
        // with stats Full and the graph off, a run where the environment decides both, another setting changed, then a run
        // without the variables
        NSString *dir=[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"aoc-settings-%d",getpid()]];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *file=[dir stringByAppendingPathComponent:@"controller-layout.txt"];
        [@"layout=label\nswap_ab=0\nfullscreen=0\noutput=2\nhints=1\nfps_target=60\ndynamic_fps=1\nstats=2\nframe_graph=0\n" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
        DmSettingsLoaded run=dm_settings_load(file,"1","1");
        CHECK("file: the environment decides this run",run.stats.value==1&&run.stats.locked&&run.graph.value==1&&run.graph.locked);
        CHECK("file: the saved choices are read",run.stats.saved==2&&run.graph.saved==0&&run.file.layout==2&&run.file.output==2&&run.file.fps_target==60);
        DmSettingsFile now=run.file;now.swap_ab=1;   // another setting changed in the panel
        CHECK("file: a locked stats row refuses the panel's change",!dm_pref_set(&run.stats,0)&&run.stats.saved==2);
        CHECK("file: saving after another change succeeds",dm_settings_save(file,now,run.stats,run.graph,NULL));
        NSString *written=[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];
        CHECK("file: keeps the saved stats and graph, not the run's",[written containsString:@"stats=2\n"]&&[written containsString:@"frame_graph=0\n"]);
        CHECK("file: the other change and settings are written",[written containsString:@"swap_ab=1\n"]&&[written containsString:@"layout=label\n"]&&
              [written containsString:@"output=2\n"]&&[written containsString:@"fps_target=60\n"]);
        CHECK("file: the retired hints key is dropped",![written containsString:@"hints"]);
        CHECK("file: an old file without aspect reads as not set and saves 16:9",run.file.aspect==-1&&[written containsString:@"aspect=0\n"]);
        { DmSettingsFile wide=run.file; wide.aspect=1;
          CHECK("file: 16:10 is saved and read back",dm_settings_save(file,wide,run.stats,run.graph,NULL)&&dm_settings_load(file,NULL,NULL).file.aspect==1);
          CHECK("file: the other settings survive the aspect change",dm_settings_load(file,NULL,NULL).file.output==2);
          dm_settings_save(file,now,run.stats,run.graph,NULL); }
        DmSettingsLoaded next=dm_settings_load(file,NULL,NULL);
        CHECK("file: next run without the variables gets the saved choices back",next.stats.value==2&&!next.stats.locked&&next.graph.value==0&&!next.graph.locked&&next.file.swap_ab==1);
        CHECK("file: an unlocked change is saved and read back",dm_pref_set(&next.stats,1)&&dm_settings_save(file,next.file,next.stats,next.graph,NULL)&&
              dm_settings_load(file,NULL,NULL).stats.value==1);
        DmSettingsLoaded bad=dm_settings_load(file,"9","x");
        CHECK("file: invalid variables are ignored",bad.stats.value==1&&!bad.stats.locked&&bad.stats_rejected&&bad.graph.value==0&&bad.graph_rejected);
        [NSFileManager.defaultManager removeItemAtPath:file error:nil];
        DmSettingsLoaded none=dm_settings_load(file,NULL,NULL);
        CHECK("file: a missing file gives the defaults",none.stats.value==0&&none.graph.value==0&&none.file.layout==-1&&none.file.swap_ab==-1);
        [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
        // The hints box and its switch are gone (Tears of the Kingdom's panel replaced its reminder box)
        CHECK("no hints row in the panel",find(panel.window.contentView,@"settings-hints")==nil);
        { NSPopUpButton *aspect=(id)find(panel.window.contentView,@"settings-aspect");
          CHECK("aspect ratio row offers 16:9 and 16:10",aspect&&aspect.numberOfItems==2&&[aspect itemAtIndex:0].tag==0&&[aspect itemAtIndex:1].tag==1);
          [aspect selectItemWithTag:1]; [aspect sendAction:aspect.action to:aspect.target];
          CHECK("choosing 16:10 reaches the host",state.aspect==1);
          [aspect selectItemWithTag:0]; [aspect sendAction:aspect.action to:aspect.target];
          CHECK("choosing 16:9 reaches the host",state.aspect==0); }
        CHECK("16:10 is offered on 1.4.1's main",dm_aspect_supported("e5f275b5d0889899e55de1a164eef060"));
        CHECK("16:10 is not offered on 1.3.0's main or an unknown one",!dm_aspect_supported("295d41bd00000000000000000000000")&&!dm_aspect_supported("")&&!dm_aspect_supported(NULL));
        { DmMacSettings narrow=state; narrow.aspect_available=0;
          DmSettingsPanel *other=[[DmSettingsPanel alloc] initWithParent:parent snapshot:^{return narrow;} change:^(DmMacSetting key, NSInteger value){(void)key;(void)value;}];
          CHECK("without the camera stubs the panel has no aspect ratio row",!find(other.window.contentView,@"settings-aspect")&&!find_text(other.window.contentView,@"Aspect ratio"));
          CHECK("without the camera stubs the other display rows stay",find(other.window.contentView,@"settings-output")&&find(other.window.contentView,@"settings-fullscreen"));
          [other close]; }
        CHECK("no hints box over the game",find(parent.contentView,@"settings-shortcuts")==nil&&find(parent.contentView,@"shortcut-values")==nil);
        state=original;state.stats_mode=2;state.frame_graph=1;[performance refresh];
        [parent setContentSize:NSMakeSize(640,360)];[performance refresh];
        CHECK("stats pane fits the minimum window",NSContainsRect(parent.contentView.bounds,performance.frame));
        [parent setContentSize:NSMakeSize(1100,790)];[performance refresh];
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
        CHECK("stats can hide",performance.hidden);
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
        // The graph shows frames per second from 0 to 60 (Tears of the Kingdom's FPS graph)
        CHECK("graph: a 16.7 ms frame is 60 FPS",fabs(dm_graph_fps(1000.0/60,DM_GRAPH_TOP_FPS)-60)<1e-9);
        CHECK("graph: a 33.3 ms frame is 30 FPS",fabs(dm_graph_fps(1000.0/30,DM_GRAPH_TOP_FPS)-30)<1e-9);
        CHECK("graph: 100 ms is 10 FPS",fabs(dm_graph_fps(100,DM_GRAPH_TOP_FPS)-10)<1e-9);
        CHECK("graph: a frame faster than 60 FPS is drawn at the top",dm_graph_fps(1000.0/120,DM_GRAPH_TOP_FPS)==60);
        CHECK("graph: no time is 0 FPS",dm_graph_fps(0,DM_GRAPH_TOP_FPS)==0&&dm_graph_fps(-1,DM_GRAPH_TOP_FPS)==0);
        // The resolutions in force: the game's picture (scene target), its output (swap chain) and the window
        NSTextField *res=(id)find(panel.window.contentView,@"settings-resolution");
        state.scene_width=state.scene_height=state.game_width=state.game_height=0;state.scene_age_s=state.game_age_s=-1;[panel refresh];
        CHECK("resolution: never observed shows --",[res.stringValue isEqualToString:@"Game picture -- · game output -- · window 3840 × 2160"]);
        state.scene_width=1440;state.scene_height=810;state.game_width=1920;state.game_height=1080;state.scene_age_s=0.2;state.game_age_s=0.02;[panel refresh];[performance refresh];
        CHECK("resolution: fresh sizes are shown as current",[res.stringValue isEqualToString:@"Game picture 1440 × 810 · game output 1920 × 1080 · window 3840 × 2160"]);
        NSTextField *ptext=(id)find(performance,@"performance-text");
        CHECK("resolution: the stats pane shows them too",[ptext.stringValue containsString:@"Picture 1440 × 810 → output 1920 × 1080 → window 3840 × 2160"]);
        state.scene_width=1920;state.scene_height=1080;state.game_width=2560;state.game_height=1440;state.scene_age_s=0.5;[panel refresh];
        CHECK("resolution: a new observed size replaces the old one",[res.stringValue isEqualToString:@"Game picture 1920 × 1080 · game output 2560 × 1440 · window 3840 × 2160"]);
        state.scene_age_s=12.4;[panel refresh];[performance refresh];
        CHECK("resolution: a stale picture says when it was last seen",[res.stringValue isEqualToString:@"Game picture 1920 × 1080 (last seen 12 s ago) · game output 2560 × 1440 · window 3840 × 2160"]);
        CHECK("resolution: the stats pane says it too",[ptext.stringValue containsString:@"Picture 1920 × 1080 (last seen 12 s ago) → output 2560 × 1440"]);
        state.scene_age_s=0.1;state.game_age_s=3.9;[panel refresh];
        CHECK("resolution: observed again it is current; a stale output says so",[res.stringValue isEqualToString:@"Game picture 1920 × 1080 · game output 2560 × 1440 (last seen 3 s ago) · window 3840 × 2160"]);
        state.scene_age_s=1.0;state.game_age_s=1.01;[panel refresh];
        CHECK("resolution: 1 s is still current, just over is last seen",[res.stringValue isEqualToString:@"Game picture 1920 × 1080 · game output 2560 × 1440 (last seen 1 s ago) · window 3840 × 2160"]);
        CHECK("the size cap row says what it is",find_text(panel.window.contentView,@"Window size cap")!=nil&&find_text(panel.window.contentView,@"Output cap")==nil);
        CHECK("the size cap says it does not change the game's resolution",find_text(panel.window.contentView,@"does not change the game's own resolution")!=nil);
        // On-demand activity (Tears of the Kingdom's shader line, Link's Awakening's copy): the colour rule with injected seconds
        DmShaderActivity a={0};
        CHECK("first sample only sets the start",dm_activity_step(&a,500,900,0,10)==DM_ACTIVITY_GREEN);
        CHECK("control: no rise stays green",dm_activity_step(&a,500,900,0,11)==DM_ACTIVITY_GREEN);
        CHECK("a pipeline build attempt lights yellow",dm_activity_step(&a,500,901,0,12)==DM_ACTIVITY_YELLOW);
        CHECK("still yellow at 2.9 s",dm_activity_step(&a,500,901,0,14.9)==DM_ACTIVITY_YELLOW);
        CHECK("green again at 3 s",dm_activity_step(&a,500,901,0,15)==DM_ACTIVITY_GREEN);
        CHECK("a library load lights yellow",dm_activity_step(&a,501,901,0,20)==DM_ACTIVITY_YELLOW);
        CHECK("errors stay orange over activity",dm_activity_step(&a,502,901,1,20.5)==DM_ACTIVITY_ORANGE);
        CHECK("a counter going down is a new start",dm_activity_step(&a,10,10,0,21)==DM_ACTIVITY_GREEN);
        dm_activity_reset(&a);CHECK("after a reset, grown counters are a new start",dm_activity_step(&a,900,2000,0,21.5)==DM_ACTIVITY_GREEN);
        AocStats kept=state.performance;
        state.performance.shaders=(AocShaderStats){.libraries=231,.render_pipelines=290,.compute_pipelines=28,.library_loads=300,.pipeline_builds=700,.library_ms=4.5,.pipeline_ms=2.5};
        int keptMode=state.stats_mode;state.stats_mode=1;
        DmPerformanceHUD *fresh=[[DmPerformanceHUD alloc] initWithSnapshot:^{return state;}];
        NSTextField *ftext=(id)find(fresh,@"performance-text");
        CHECK("a new pane with counted work starts green",[ftext.textColor isEqual:NSColor.systemGreenColor]);
        CHECK("FPS only shows on-demand loads and builds against ready",[ftext.stringValue containsString:@"On-demand library loads 300 · Ready 231"] &&
              [ftext.stringValue containsString:@"On-demand pipeline builds 700 · Ready 318"] && ![ftext.stringValue containsString:@" ms"]);
        state.performance.shaders.pipeline_builds=701;[fresh refresh];
        CHECK("the pane turns yellow on a new build attempt",[ftext.textColor isEqual:NSColor.systemYellowColor]);
        state.performance.shaders.pipeline_errors=1;[fresh refresh];
        CHECK("the pane keeps orange for errors during activity",[ftext.textColor isEqual:NSColor.systemOrangeColor]);
        state.performance.shaders.pipeline_errors=0;state.stats_mode=0;[fresh refresh];
        CHECK("Off hides the pane",fresh.hidden);
        state.performance.shaders.pipeline_builds=800;state.stats_mode=1;[fresh refresh];
        CHECK("shown again with grown counters: green",!fresh.hidden && [ftext.textColor isEqual:NSColor.systemGreenColor]);
        state.stats_mode=2;[fresh refresh];
        CHECK("Full shows them with their time, without the old lines",[ftext.stringValue containsString:@"On-demand library loads 300 / 4.5 ms · Ready 231"] &&
              [ftext.stringValue containsString:@"On-demand pipeline builds 800 / 2.5 ms · Ready 318"] && ![ftext.stringValue containsString:@"On-demand PSOs"]);
        state.performance.shaders.library_loads=301;[fresh refresh];
        CHECK("Full turns yellow on a new library load",[ftext.textColor isEqual:NSColor.systemYellowColor]);
        state.performance=kept;state.stats_mode=keptMode;
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
