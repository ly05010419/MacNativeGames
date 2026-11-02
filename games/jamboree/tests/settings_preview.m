// Native settings UI harness; no game content, keys, Metal device or guest code.
#import "../runtime/host/front_settings.h"
#include "../runtime/host/mac_display_settings.h"
#include <stdio.h>
#include <string.h>
static DmMacSettings state = {.show_hints = 1, .output_width = 3840, .output_height = 2160};
static NSView *find(NSView *view, NSString *identifier) {
    if ([view.identifier isEqualToString:identifier]) return view;
    for (NSView *child in view.subviews) { NSView *result = find(child, identifier); if (result) return result; }
    return nil;
}
static int failures, checks;
#define CHECK(NAME, CONDITION) do { checks++; if (!(CONDITION)) { failures++; fprintf(stderr, "FAIL: %s\n", NAME); } } while (0)
static void change(DmMacSetting key, NSInteger value) {
    switch (key) {
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
    parent.title = @"Jamboree 设置预览 · 无游戏数据"; [parent center];
    parent.contentView.wantsLayer=YES;parent.contentView.layer.backgroundColor=[NSColor colorWithRed:.07 green:.12 blue:.20 alpha:1].CGColor;
    NSTextField *title=[NSTextField labelWithString:@"Jamboree\n设置面板预览"];
    title.textColor=NSColor.whiteColor;title.font=[NSFont systemFontOfSize:32 weight:NSFontWeightSemibold];title.frame=NSMakeRect(40,320,450,110);[parent.contentView addSubview:title];
    DmSettingsPanel *panel = [[DmSettingsPanel alloc] initWithParent:parent snapshot:^{return state;}
        change:^(DmMacSetting key, NSInteger value){change(key,value);}];
    DmShortcutHUD *hud=[[DmShortcutHUD alloc] initWithSnapshot:^{return state;} openSettings:^{[panel showPanel];}];
    hud.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;[parent.contentView addSubview:hud];[hud refresh];
    NSMenu *bar=[NSMenu new], *app=[NSMenu new];[[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
    [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];NSApp.mainMenu=bar;
    [parent makeKeyAndOrderFront:nil];[panel showPanel];
    if (test) {
        [panel.window.contentView layoutSubtreeIfNeeded];
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
