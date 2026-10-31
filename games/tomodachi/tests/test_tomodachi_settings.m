#import <Cocoa/Cocoa.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "runtime.h"
void runtime_title_gamepad_event(RuntimeGamepadState);
@protocol SettingsControlsTest
- (void)assignInput:(int)input action:(int)action;
- (void)assignPad:(int)physical action:(int)action;
- (void)controlsTick;
@end
void runtime_title_settings_prepare(void);
unsigned runtime_title_map_key(unsigned);
uint64_t runtime_title_map_buttons(uint64_t);
double runtime_title_drawable_scale(void);
int runtime_title_input_blocked(void);
void runtime_title_frontend_ready(void *window);
int runtime_title_settings_key(unsigned key);
void fps_overlay_set_enabled(int value) { (void)value; }
int fps_overlay_value(void) { return 60; }
void runtime_front_refresh_drawable(void) {}
void runtime_input_clear(void) {}
void runtime_feedback_quiet(void) {}
int runtime_rumble_enabled(void) { return 1; }
int runtime_hid_feedback_flag(const char *name,int value) { (void)name;(void)value;return 1; }
void runtime_title_pointer(int phase,double x,double y) { (void)phase;(void)x;(void)y; }
int main(int argc,char **argv) { @autoreleasepool {
    BOOL ui=argc>1&&!strcmp(argv[1],"--ui-smoke");
    BOOL invalid=argc>1&&!ui;
    NSString *folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]);
    NSArray *keys=invalid?@[@38,@38,@34,@32,@6,@8,@12,@14,@36,@48]:@[@40,@38,@34,@32,@6,@8,@12,@14,@36,@48];
    NSDictionary *prefs=@{@"keys":keys,@"swapButtons":@YES,@"lowResolution":@YES,@"language":invalid?@"invalid":@"ja"};
    NSData *data=[NSJSONSerialization dataWithJSONObject:prefs options:0 error:nil];
    assert([data writeToFile:[folder stringByAppendingPathComponent:@"settings.json"] atomically:YES]);
    setenv("TOTK_PROFILE_DIR",folder.fileSystemRepresentation,1);setenv("TOTK_LANG","en-US",1);
    assert(runtime_title_map_buttons(0x1234)==0x1234); // no settings prepared in a headless start
    runtime_title_settings_prepare();
    assert(runtime_title_map_key(38)==(invalid?38:40));
    assert(runtime_title_map_key(40)==(invalid?40:38));
    assert(runtime_title_map_key(13)==13); // movement unchanged
    assert(runtime_title_map_key(18)==18); // Settings shortcut unchanged
    assert(runtime_title_map_key(999)==999);
    for(uint64_t bits=0;bits<16;bits++) {
        uint64_t expected=(bits&1?2:0)|(bits&2?1:0)|(bits&4?8:0)|(bits&8?4:0);
        assert(runtime_title_map_buttons(bits|0x12340000)==(expected|0x12340000));
    }
    assert(runtime_title_drawable_scale()==.5);
    assert(!runtime_title_input_blocked());
    assert(!strcmp(getenv("TOTK_LANG"),invalid?"en-US":"ja"));
    if(ui){
        [NSApplication sharedApplication];
        NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1000,720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        runtime_title_frontend_ready((__bridge void *)window);
        assert(runtime_title_settings_key(18));assert(runtime_title_input_blocked());
        NSView *panel=nil;for(NSView *view in window.contentView.subviews)if([view isKindOfClass:NSClassFromString(@"TomodachiSettingsPanel")])panel=view;assert(panel&&!panel.hidden);
        NSSegmentedControl *segments=nil;NSTabView *tabs=nil;
        for(NSView *view in panel.subviews){if([view isKindOfClass:NSSegmentedControl.class])segments=(id)view;if([view isKindOfClass:NSTabView.class])tabs=(id)view;}
        assert(segments.segmentCount==3&&tabs.numberOfTabViewItems==3);
        segments.selectedSegment=1;[segments sendAction:segments.action to:segments.target];assert(tabs.selectedTabViewItem==tabs.tabViewItems[1]);
        NSSwitch *fps=nil;
        for(NSView *row in tabs.tabViewItems[0].view.subviews)for(NSView *control in row.subviews)if([control isKindOfClass:NSSwitch.class])fps=(id)control;
        assert(fps);fps.state=NSControlStateValueOn;[fps sendAction:fps.action to:fps.target];
        NSDictionary *persisted=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"settings.json"]] options:0 error:nil];
        assert([persisted[@"fps"] boolValue]);BOOL hudVisible=NO;for(NSView *view in window.contentView.subviews)if([view isKindOfClass:NSClassFromString(@"TomodachiPerformanceView")])hudVisible=!view.hidden;assert(hudVisible);
        runtime_title_settings_key(18);assert(!runtime_title_input_blocked());assert(panel.hidden);
        runtime_title_settings_key(18);assert(runtime_title_input_blocked());runtime_title_settings_key(18);assert(!runtime_title_input_blocked());
        id<SettingsControlsTest> controls=(id)NSApp.mainMenu.itemArray.firstObject.submenu.itemArray.firstObject.target;
        [controls assignInput:37 action:0];assert(runtime_title_map_key(37)==38);
        [controls assignPad:0 action:3];assert(runtime_title_map_buttons(1)==8);
        runtime_title_gamepad_event((RuntimeGamepadState){.connected=1,.buttons=(1u<<4)|(1u<<5)});[controls controlsTick];assert(runtime_title_input_blocked());
        runtime_title_gamepad_event((RuntimeGamepadState){.connected=1});[controls controlsTick];
        runtime_title_gamepad_event((RuntimeGamepadState){.connected=1,.buttons=1u<<7});[controls controlsTick];assert(segments.selectedSegment==2);
        runtime_title_gamepad_event((RuntimeGamepadState){.connected=1});[controls controlsTick];
        runtime_title_gamepad_event((RuntimeGamepadState){.connected=1,.buttons=2});[controls controlsTick];assert(!runtime_title_input_blocked());
        puts("PASS: upstream diagram remapping affects guest keyboard/controller bits; controller opens, changes tabs, closes settings");
    }
    assert([NSFileManager.defaultManager removeItemAtPath:folder error:nil]);
    puts(invalid?"PASS: invalid saved bindings/language safely use defaults":"PASS: saved keyboard/controller/display/language preferences load correctly");
    (void)argv;
} return 0; }
