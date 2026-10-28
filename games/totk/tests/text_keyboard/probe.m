/* Exercise the actual AppKit keyboard and controller routing without a game, keys or save files.
   --preview opens a standalone panel with real controller input; an optional final argument writes a PNG. */
#import <Cocoa/Cocoa.h>
#include "../../runtime/gamepad.m"
#include "../../runtime/text_keyboard.h"
#include <assert.h>
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"FAIL: %s\n",s);exit(1);}
uint64_t runtime_now_ns(void){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
void runtime_feedback_attach(GCController *c){(void)c;}
void runtime_feedback_detach(GCController *c){(void)c;}
void runtime_feedback_quiet(void){}
int runtime_gyro_enabled(void){return 0;}
void runtime_motion_mouse(int on){(void)on;}
void runtime_motion_feed_mouse(const double a[3],const double g[3],uint64_t n){(void)a;(void)g;(void)n;}
uint64_t runtime_keyboard_buttons(void){return 0;}
@interface KeyboardPreviewDelegate:NSObject<NSWindowDelegate>
@end
@implementation KeyboardPreviewDelegate
-(void)windowWillClose:(NSNotification *)notification{(void)notification;[NSApp stop:nil];}
@end
static unsigned checked;
#define CHECK(x) do{checked++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static TOTKTextBar *panel(BOOL digits,BOOL secure){
 TOTKTextBar *b=[[TOTKTextBar alloc] initWithFrame:NSMakeRect(0,0,1000,640)];
 b.maxUnits=9;b.minUnits=0;b.digitsOnly=digits;b.disabled=0x12c;
 [b buildWithPrompt:@"Comment souhaitez-vous appeler votre cheval ?" initial:@"" secure:secure];return b;
}
static RuntimeGamepadState pad(uint64_t buttons,int x,int y){return (RuntimeGamepadState){.buttons=buttons,.lx=x,.ly=y,.connected=1};}
static void press(TOTKTextBar *b,uint64_t buttons){[b handlePad:pad(0,0,0) time:1 active:YES];[b handlePad:pad(buttons,0,0) time:1.1 active:YES];}
int main(int argc,char **argv){@autoreleasepool{
 [NSApplication sharedApplication];
 if(argc<2)return 2;
 NSString *controls=[@(argv[1]) stringByAppendingPathComponent:@"controls.txt"];setenv("TOTK_CONTROLS",controls.UTF8String,1);
 controller_map_default(controllerMap,controllerModes);
 const char *originalLanguage=getenv("TOTK_UI_LANG");NSString *savedLanguage=originalLanguage?@(originalLanguage):nil;
 setenv("TOTK_UI_LANG","en",1);CHECK([text_keyboard_label(@"Done",@"Valider",@"确定") isEqual:@"Done"]);
 setenv("TOTK_UI_LANG","fr",1);CHECK([text_keyboard_label(@"Done",@"Valider",@"确定") isEqual:@"Valider"]);
 setenv("TOTK_UI_LANG","zh",1);CHECK([text_keyboard_label(@"Done",@"Valider",@"确定") isEqual:@"确定"]);
 if(savedLanguage)setenv("TOTK_UI_LANG",savedLanguage.UTF8String,1);else unsetenv("TOTK_UI_LANG");
 TOTKTextBar *b=panel(NO,NO);
 b.azerty=NO;[b rebuild];CHECK([b.rows[0][0] isEqual:@"q"]);
 [b activate:@"layout"];CHECK(b.azerty&&[b.rows[0][0] isEqual:@"a"]);
 CHECK(panel(NO,NO).azerty); /* saved layout is read by the next request */
 [b activate:@"a"];[b activate:@"shift"];[b activate:@"é"];CHECK([b.field.stringValue isEqual:@"aÉ"]);
 [b activate:@"symbols"];CHECK(b.symbols);[b activate:@"space"];[b activate:@"1"];CHECK([b.field.stringValue isEqual:@"aÉ 1"]);
 b.field.stringValue=@"a@b%c\\d";[b refilter];CHECK([b.field.stringValue isEqual:@"abcd"]);
 b.field.stringValue=@"12345678🐴";[b refilter];CHECK([b.field.stringValue isEqual:@"12345678"]);
 b.field.stringValue=@"e\u0301🐴";[b activate:@"delete"];CHECK([b.field.stringValue isEqual:@"e\u0301"]);
 [b activate:@"delete"];CHECK(b.field.stringValue.length==0);
 b.field.stringValue=@"1234567890";[b refilter];CHECK(b.field.stringValue.length==9);
 b.row=0;b.column=0;[b move:1];CHECK(b.column==9);[b move:2];CHECK(b.column==0);
 [b move:3];CHECK(b.row==4);[b move:4];CHECK(b.row==0);
 b.field.stringValue=@"";b.row=0;b.column=0;b.upper=NO;b.symbols=NO;[b rebuild];
 [b handlePad:pad(1,0,0) time:0 active:YES];CHECK(b.field.stringValue.length==0); /* opener held */
 press(b,1);CHECK([b.field.stringValue isEqual:@"a"]);
 [b handlePad:pad(1,0,0) time:2 active:YES];CHECK([b.field.stringValue isEqual:@"a"]); /* held select does not repeat */
 press(b,4);CHECK(b.field.stringValue.length==0);
 press(b,16);CHECK(b.upper);press(b,32);CHECK(!b.azerty);press(b,256);CHECK(b.symbols);
 press(b,8);CHECK([b.field.stringValue isEqual:@" "]);
 [b handlePad:pad(0,0,0) time:3 active:YES];b.column=0;
 [b handlePad:pad(0,25000,0) time:3.1 active:YES];CHECK(b.column==1);
 [b handlePad:pad(0,25000,0) time:3.2 active:YES];CHECK(b.column==1);
 [b handlePad:pad(0,25000,0) time:3.5 active:YES];CHECK(b.column==2);
 b.field.stringValue=@"";[b handlePad:pad(1,0,0) time:4 active:NO];CHECK(!b.armed&&b.field.stringValue.length==0);
 [b handlePad:pad(1,0,0) time:4.1 active:YES];CHECK(b.field.stringValue.length==0);
 RuntimeGamepadState disconnected={0};[b handlePad:disconnected time:5 active:YES];CHECK(!b.armed);
 TOTKTextBar *numeric=panel(YES,NO);numeric.field.stringValue=@"a12é🐴3";[numeric refilter];CHECK([numeric.field.stringValue isEqual:@"123"]);
 CHECK(numeric.rows.count==5&&[numeric.rows[0][0] isEqual:@"1"]);
 CHECK([panel(NO,YES).field isKindOfClass:NSSecureTextField.class]);
 __block int completions=0;__block NSString *result=nil;
 b.done=^(NSString *s){completions++;result=s;};b.minUnits=3;b.field.stringValue=@"ab";[b confirm];CHECK(completions==0);
 b.field.stringValue=@"Epona";press(b,1ull<<10);CHECK(completions==1&&[result isEqual:@"Epona"]);
 [b finish:nil];CHECK(completions==1);
 TOTKTextBar *cancel=panel(NO,NO);cancel.done=^(NSString *s){result=s;completions++;};press(cancel,2);CHECK(result==nil&&completions==2);
 TOTKTextBar *empty=panel(NO,NO);empty.done=^(NSString *s){result=s;};[empty confirm];CHECK(result!=nil&&result.length==0);
 /* Test AppKit's actual field editor, selection replacement, physical typing and paste filtering. */
 NSWindow *win=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1000,640) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
 TOTKTextBar *edit=panel(NO,NO);win.contentView=edit;[win makeFirstResponder:edit.field];
 NSTextView *editor=(NSTextView *)edit.field.currentEditor;CHECK(editor!=nil);
 [editor insertText:@"Epona" replacementRange:NSMakeRange(NSNotFound,0)];[edit refilter];CHECK([edit.field.stringValue isEqual:@"Epona"]);
 [editor setSelectedRange:NSMakeRange(0,1)];[edit activate:@"a"];CHECK([edit.field.stringValue isEqual:@"apona"]);
 [editor setSelectedRange:NSMakeRange(2,0)];press(edit,64);CHECK(editor.selectedRange.location==1);
 press(edit,128);CHECK(editor.selectedRange.location==2);
 [editor setSelectedRange:NSMakeRange(1,2)];[edit activate:@"delete"];CHECK([edit.field.stringValue isEqual:@"ana"]);
 [editor insertText:@"@%" replacementRange:editor.selectedRange];[edit refilter];CHECK([edit.field.stringValue isEqual:@"ana"]);
 [editor setMarkedText:@"é" selectedRange:NSMakeRange(1,0) replacementRange:NSMakeRange(0,1)];CHECK(editor.hasMarkedText);
 NSString *composition=editor.string;[edit activate:@"z"];CHECK(editor.hasMarkedText&&[editor.string isEqual:composition]);[editor unmarkText];
 [edit setFrameSize:NSMakeSize(480,300)];[edit layout];CHECK(NSContainsRect(edit.bounds,edit.card.frame));
 [edit setFrameSize:NSMakeSize(1500,1000)];[edit layout];CHECK(NSContainsRect(edit.bounds,edit.card.frame));
 /* Labels and confirmation follow the controller family, including Switch A at east. */
 for(int family=GAMEPAD_TYPE_XBOX;family<=GAMEPAD_TYPE_SWITCH;family++){
  runtime_gamepad_type_set(family);TOTKTextBar *familyPanel=panel(NO,NO);familyPanel.azerty=NO;[familyPanel rebuild];
  const char *trigger=family==GAMEPAD_TYPE_XBOX?"LT":family==GAMEPAD_TYPE_PLAYSTATION?"L2":"ZL";
  CHECK([[familyPanel shortcutFor:@"symbols"] isEqual:@(trigger)]);
  CHECK([[familyPanel shortcutFor:@"left"] isEqual:family==GAMEPAD_TYPE_XBOX?@"LB":family==GAMEPAD_TYPE_PLAYSTATION?@"L1":@"L"]);
  press(familyPanel,family==GAMEPAD_TYPE_SWITCH?2:1);CHECK([familyPanel.field.stringValue isEqual:@"q"]);
  __block BOOL accepted=NO;familyPanel.done=^(NSString *s){accepted=[s isEqual:@"q"];};press(familyPanel,512);CHECK(accepted);
  TOTKTextBar *cancelFamily=panel(NO,NO);__block BOOL cancelled=NO;cancelFamily.done=^(NSString *s){cancelled=s==nil;};
  press(cancelFamily,family==GAMEPAD_TYPE_SWITCH?1:2);CHECK(cancelled);
 }
 runtime_gamepad_type_set(GAMEPAD_TYPE_AUTO);
 /* Real gamepad routing: guest polling must not eat the UI's press, and close must wait for release. */
 runtime_gamepad_text_capture(1);textPad=pad(1,20000,0);textEdges=1;gamepadState=pad(8,20000,0);pressedEdges=8;
 RuntimeGamepadState read;runtime_gamepad_read(&read);CHECK(!read.buttons&&!read.lx&&read.connected);
 runtime_gamepad_text_read(&read);CHECK(read.buttons==1&&read.lx==20000);CHECK(textEdges==0);
 textPad=pad(0,0,0);textEdges=4;runtime_gamepad_read(&read);runtime_gamepad_text_read(&read);CHECK(read.buttons==4);
 runtime_gamepad_text_capture(0);textPad=pad(1,0,0);gamepadState=pad(8,0,0);runtime_gamepad_read(&read);CHECK(!read.buttons&&textCapture==2);
 textPad=pad(0,0,0);gamepadState=pad(0,0,0);runtime_gamepad_read(&read);CHECK(textCapture==0&&!read.buttons);
 gamepadState=pad(8,0,0);runtime_gamepad_read(&read);CHECK(read.buttons==8);
 textPad=pad(0,25000,0);gamepadRawLeftAt=runtime_now_ns()-300000000ULL;gamepadNativeLx=gamepadNativeLy=0;
 runtime_gamepad_text_read(&read);CHECK(!read.lx);textCapture=2;runtime_gamepad_read(&read);CHECK(textCapture==0);
 gamepadRawLeftAt=0;
 /* Exercise capture through actual refresh mapping, including gameplay tap/toggle modes. */
 runtime_gamepad_clear();hidPad=(HIDPadState){.present=1,.buttons=1};controllerModes[0]=CONTROLLER_TOGGLE;
 runtime_gamepad_text_capture(1);runtime_gamepad_text_read(&read);CHECK(read.buttons==1&&!toggleOn[0]);
 runtime_gamepad_text_capture(0);runtime_gamepad_read(&read);CHECK(!read.buttons);
 hidPad.buttons=0;refresh_gamepads();runtime_gamepad_read(&read);CHECK(textCapture==0);
 hidPad.buttons=1;refresh_gamepads();runtime_gamepad_read(&read);CHECK(read.buttons==2&&toggleOn[0]);
 hidPad=(HIDPadState){0};runtime_gamepad_clear();
 printf("pass: text keyboard (%u checks)\n",checked);
 if(argc>2){
  if(strcmp(argv[2],"--preview"))runtime_gamepad_type_set(GAMEPAD_TYPE_PLAYSTATION); /* deterministic screenshot fixture */
  TOTKTextBar *preview=panel(NO,NO);preview.azerty=YES;preview.field.stringValue=@"Épona";[preview rebuild];[preview refilter];
  win.contentView=preview;[win setContentSize:NSMakeSize(1000,640)];[preview layout];
  if(strcmp(argv[2],"--preview")){
   NSBitmapImageRep *rep=[preview bitmapImageRepForCachingDisplayInRect:preview.bounds];[preview cacheDisplayInRect:preview.bounds toBitmapImageRep:rep];
   CHECK([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES]);
  }else{
   KeyboardPreviewDelegate *previewDelegate=[KeyboardPreviewDelegate new];win.delegate=previewDelegate;
   win.title=@"MacNativeGames — Controller keyboard preview (no game or save)";win.releasedWhenClosed=NO;
   preview.done=^(NSString *s){fprintf(stderr,"Preview %s\n",s?"confirmed":"cancelled");[NSApp stop:nil];};
   [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
   [win center];[win makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];[win makeFirstResponder:preview.field];
   dispatch_async(dispatch_get_main_queue(),^{runtime_gamepad_start();runtime_gamepad_text_capture(1);[preview startController];});
   [NSApp run];
  }
 }
 return 0;
}}
