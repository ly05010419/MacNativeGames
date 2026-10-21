/* Core AppKit binding editor check; no guest, saves, controller or GPU work. */
#define main display_probe_original_main
#include "frontend_display_probe.m"
#undef main
static NSEvent *key_event(NSWindow *w,unsigned code,NSEventModifierFlags mods,BOOL repeat){return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:mods timestamp:0 windowNumber:w.windowNumber context:nil characters:@"b" charactersIgnoringModifiers:@"b" isARepeat:repeat keyCode:code];}
static void capture_input_tab(NSInteger index,NSString *path){
 [displaySettings.tabs selectTabViewItemAtIndex:index];[displaySettings.window.contentView layoutSubtreeIfNeeded];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
 NSView *v=displaySettings.window.contentView;NSBitmapImageRep *rep=[v bitmapImageRepForCachingDisplayInRect:v.bounds];[v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES]);
}
int main(void){@autoreleasepool{
 setenv("TOTK_SLOT_OWNER","binding-probe",1);setenv("TOTK_ASSUME_FOCUS","1",1);[NSApplication sharedApplication];NSApp.appearance=[NSAppearance appearanceNamed:NSAppearanceNameAqua];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
 display_load();NSString *suite=@"org.macnativegames.totk.display.tests";[displayDefaults removePersistentDomainForName:suite];display_load();
 window=[[TOTKGameWindow alloc] initWithContentRect:NSMakeRect(100,100,900,600) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
 [window makeKeyAndOrderFront:nil];displaySettings=[TOTKDisplaySettings new];[displaySettings showSettings:nil];
 assert(front_modifier_down(56,NX_DEVICELSHIFTKEYMASK)&&!front_modifier_down(60,NX_DEVICELSHIFTKEYMASK));
 assert(front_modifier_down(60,NX_DEVICERSHIFTKEYMASK)&&!front_modifier_down(56,NX_DEVICERSHIFTKEYMASK));
 TOTKWindowDelegate *delegate=[TOTKWindowDelegate new];[delegate windowDidResignKey:[NSNotification notificationWithName:NSWindowDidResignKeyNotification object:window]];assert(probeSuspended);
 assert(displaySettings.tabs.numberOfTabViewItems==5&&probeSuspended);TOTKInputSettings *s=displaySettings.inputSettings;assert(s.keyButtons.count==48&&s.targets.count==17);[displaySettings.tabs selectTabViewItemAtIndex:3];
 RuntimeKeyboardBindings original=probeKeyboard;
 [s record:s.keyButtons[0]];assert(s.monitor);NSEvent *outside=key_event(window,11,0,NO);assert([s captureEvent:outside]==outside&&s.recording==0);
 [s captureEvent:key_event(s.hostWindow,11,0,YES)];assert(s.recording==0);
 [s captureEvent:key_event(s.hostWindow,11,NSEventModifierFlagCommand,NO)];assert(s.recording==0&&s.error.stringValue.length);
 [s captureEvent:key_event(s.hostWindow,53,0,NO)];assert(!s.monitor&&s.keyboard.source[0][0]==original.source[0][0]);
 s.mouseLook.state=NSControlStateValueOn;s.sensitivity.integerValue=175;[s keyboardOptionsChanged:nil];
 [s record:s.keyButtons[0]];[s captureEvent:key_event(s.hostWindow,11,0,NO)];assert(s.keyboard.source[0][0]==12&&!s.monitor&&s.keyboard.mouse_look&&s.keyboard.mouse_sensitivity==175);
 [s record:s.keyButtons[2]];NSPoint bindingPoint=[s.keyButtons[2] convertPoint:NSMakePoint(10,10) toView:nil];NSEvent *mouse=[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:bindingPoint modifierFlags:0 timestamp:0 windowNumber:s.hostWindow.windowNumber context:nil eventNumber:1 clickCount:1 pressure:1];[s captureEvent:mouse];assert(s.keyboard.source[1][0]==129);
 [s record:s.keyButtons[0]];NSEvent *outsidePage=[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:s.hostWindow.windowNumber context:nil eventNumber:2 clickCount:1 pressure:1];assert([s captureEvent:outsidePage]==outsidePage&&!s.monitor&&s.keyboard.source[0][0]==12);
 [s record:s.keyButtons[0]];[displaySettings.tabs selectTabViewItemAtIndex:4];assert(!s.monitor);
 [displaySettings cancelSettings:nil];assert(!probeSuspended&&!memcmp(&original,&probeKeyboard,sizeof original));[delegate windowDidResignKey:[NSNotification notificationWithName:NSWindowDidResignKeyNotification object:window]];assert(!probeSuspended);
 [displaySettings showSettings:nil];RuntimeKeyboardBindings k=s.keyboard;k.source[0][0]=12;s.keyboard=k;[s refreshKeyboard];
 assert([s validate:displaySettings.draftKeys]);k.source[1][0]=12;s.keyboard=k;assert(![s validate:displaySettings.draftKeys]);
 k.source[1][0]=129;k.source[0][0]=24;s.keyboard=k;assert(![s validate:displaySettings.draftKeys]);k.source[0][0]=19;s.keyboard=k;assert(![s validate:displaySettings.draftKeys]);
 k.source[0][0]=12;k.mouse_look=1;k.mouse_sensitivity=175;s.keyboard=k;[s refreshKeyboard];
 [s.targets[0] selectItemAtIndex:2];[s controllerChanged:s.targets[0]];s.swapSticks.state=NSControlStateValueOn;s.padInvert.state=NSControlStateValueOn;[s controllerOptionsChanged:nil];
 [s.modes[0] selectItemAtIndex:CONTROLLER_AIM];[s.fires[0] selectItemAtIndex:0];[s controllerChanged:s.modes[0]];assert(![s validate:displaySettings.draftKeys]);
 [s.fires[0] selectItemAtIndex:9];[s controllerChanged:s.fires[0]];assert([s validate:displaySettings.draftKeys]);
 const char *out=getenv("TOTK_INPUT_PROBE_IMAGES");if(out){capture_input_tab(3,[@(out) stringByAppendingPathComponent:@"keyboard.png"]);capture_input_tab(4,[@(out) stringByAppendingPathComponent:@"controller.png"]);}
 [displaySettings applySettings:nil];assert(!probeSuspended&&probeKeyboard.source[0][0]==12&&probeKeyboard.source[1][0]==129&&probeKeyboard.mouse_sensitivity==175&&probeController.map[0]==2&&probeController.swap_sticks&&probeController.invert_look_y);
 input_bindings_default(&probeKeyboard);controller_map_default(probeController.map,probeController.modes);probeController.swap_sticks=probeController.invert_look_y=0;display_input_load();assert(probeKeyboard.source[0][0]==12&&probeKeyboard.mouse_look&&probeController.swap_sticks);
 [displaySettings showSettings:nil];[s resetKeyboard:nil];[s resetController:nil];assert(s.keyboard.source[0][0]==39&&!s.controller.swap_sticks);[displaySettings cancelSettings:nil];assert(probeKeyboard.source[0][0]==12&&probeController.swap_sticks);
 RuntimeKeyboardBindings decoded;NSMutableArray *malformed=[input_keyboard_values(&probeKeyboard) mutableCopy];malformed[0]=@1.5;assert(!input_keyboard_decode(malformed,&decoded));malformed[0]=@"12";assert(!input_keyboard_decode(malformed,&decoded));assert(!input_keyboard_decode(@[],&decoded));
 RuntimeControllerBindings pad;assert(!input_controller_decode(@[],&pad));NSMutableArray *bad=[input_controller_values(&probeController) mutableCopy];bad[0]=@3;assert(!input_controller_decode(bad,&pad));
 [displayDefaults setObject:@[] forKey:@"keyboardMouseBindingsV1"];display_input_load();assert(probeKeyboard.source[0][0]==12);
 [displayDefaults removePersistentDomainForName:suite];[window orderOut:nil];puts("PASS: capture scope, keyboard/mouse/controller drafts, validation, apply/cancel/reset and preference reload");
}}
