#import <Cocoa/Cocoa.h>
#include <assert.h>
static NSWindow *window;
static int rate, pad, mouse;
void tmnt_frame_rate_set(int n){rate=n;}
int tmnt_frame_rate_get(void){return rate;}
int runtime_gamepad_type(int *p){*p=0;return pad;}
int runtime_gamepad_type_cycle(void){return pad=(pad+1)%4;}
int runtime_mouse_enabled(void){return mouse;}
int runtime_mouse_toggle(void){return mouse=!mouse;}
void runtime_input_clear(void){}
#include "../runtime/tmnt_settings.h"
int main(void) {
 @autoreleasepool {
  [NSApplication sharedApplication];
  window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1280,720) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
  tmnt_settings_load();
  assert(!tmntPreferences[@"syncFolder"] && !tmntPreferences[@"frameRate"]);
  tmnt_settings_toggle();
  dispatch_async(dispatch_get_main_queue(), ^{
   assert(tmntSettingsPanel.visible);
   NSStackView *stack=tmntSettingsPanel.contentView.subviews[0];
   NSPopUpButton *fps=(NSPopUpButton *)((NSStackView *)stack.arrangedSubviews[0]).arrangedSubviews[1];
   [fps selectItemWithTag:60];[tmntSettingsController change:fps];assert(rate==60);
   NSPopUpButton *controller=(NSPopUpButton *)((NSStackView *)stack.arrangedSubviews[1]).arrangedSubviews[1];
   [controller selectItemWithTag:3];[tmntSettingsController change:controller];assert(pad==3);
   tmntPreferences[@"syncFolder"]=[@(getenv("TOTK_PROFILE_DIR")) stringByAppendingPathComponent:@"cloud"];
   tmnt_preferences_save();tmnt_settings_load();assert(rate==60);
   assert(getenv("TOTK_CLOUD_SAVE_FOLDER"));
   [tmntSettingsController stopSync:nil];
   assert(!tmntPreferences[@"syncFolder"]);
   assert(getenv("TOTK_CLOUD_SAVE_FOLDER")); // live session keeps its selected save root until restart
   setenv("TOTK_UI_LANG","zh-Hant",1);assert([TMNT_TEXT("简","繁","English") isEqualToString:@"繁"]);
   setenv("TOTK_UI_LANG","en",1);assert([TMNT_TEXT("简","繁","English") isEqualToString:@"English"]);
   tmnt_settings_toggle();
   dispatch_async(dispatch_get_main_queue(), ^{assert(!tmntSettingsPanel.visible);exit(0);});
  });
  [NSApp run];
 }
}
