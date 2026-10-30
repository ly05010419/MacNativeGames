/* Exercise frontend.m's guest-thread handoff and cleanup, not just the keyboard view.
   Reuse the existing no-game frontend fixture (renaming its entry point). */
#define main command_q_main
#include "../command_q/probe.m"
#undef main
#include <assert.h>
static _Atomic int completed[2];
static int returned[2];
static char answers[2][128];
static void begin(int i){
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  RuntimeTextRequest req={"Name your horse","Epona",9,1,0,0,0x12c};
  returned[i]=runtime_front_text_input(&req,answers[i],sizeof answers[i]);atomic_store(&completed[i],1);
 });
}
static void tick(void){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];}
static void wait_open(void){for(int i=0;i<100&&!openBar;i++)tick();assert(openBar&&textEntryOpen&&textCaptureFixture);}
static void wait_done(int i){for(int n=0;n<100&&!atomic_load(&completed[i]);n++)tick();assert(atomic_load(&completed[i]));}
int main(void){@autoreleasepool{
 setenv("TOTK_ASSUME_FOCUS","1",1);
 [NSApplication sharedApplication];
 window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,900,600) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
 window.contentView=[[TOTKInputView alloc] initWithFrame:window.contentView.bounds];
 begin(0);wait_open();TOTKTextBar *first=openBar;
 assert([first.field.stringValue isEqual:@"Epona"]);
 begin(1);wait_done(1);assert(!returned[1]&&openBar==first); /* overlapping request cancels, leaves first intact */
 [first handlePad:(RuntimeGamepadState){.connected=1} time:0 active:YES];
 [first handlePad:(RuntimeGamepadState){.connected=1,.buttons=1024} time:0.1 active:YES];wait_done(0);
 assert(returned[0]&&!strcmp(answers[0],"Epona"));assert(!textCaptureFixture&&!textEntryOpen&&!openBar);
 assert(window.contentView.subviews.count==0&&window.firstResponder==window.contentView);
 for(int i=0;i<5;i++){
  atomic_store(&completed[0],0);begin(0);wait_open();
  [openBar control:openBar.field textView:(NSTextView *)openBar.field.currentEditor doCommandBySelector:@selector(cancelOperation:)];
  wait_done(0);assert(!returned[0]&&!openBar&&!textEntryOpen&&!textCaptureFixture);
  assert(window.contentView.subviews.count==0);
 }
 puts("pass: text keyboard lifecycle (confirm, cancel, concurrent requests, repeated open/close, capture cleanup)");
 return 0;
}}
