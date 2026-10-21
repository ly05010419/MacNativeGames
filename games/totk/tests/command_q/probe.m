/* Exercise the real AppKit menu and window-close handler without game data. */
#include "../../runtime/frontend.m"
#include <time.h>

static int mode;
static uint64_t start;
static uint64_t clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
uint64_t runtime_now_ns(void){return (clock_ns()-start)*(mode==2?100:1);}
int runtime_fs_save_busy(void){return mode==2||(mode==1&&runtime_now_ns()<200000000ull);}
_Noreturn void runtime_fail(const char *message){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
void runtime_input_key(unsigned code,int down){(void)code;(void)down;}
void runtime_input_clear(void){}
void runtime_gamepad_start(void){}
void runtime_oe_push_message(uint32_t message){(void)message;}
void hle_bind_system_event(void *event,int autoclear){(void)event;(void)autoclear;}
void hle_signal_system_event(void *event){(void)event;}
uint64_t dm_nvn_frame_count(void){return 0;}
int fps_overlay_toggle_hints(void){return 0;}
int hud_compact(void){return 0;}
int hud_snapshot(int line,char *out,size_t n,int *highlight){(void)line;(void)out;(void)n;(void)highlight;return 0;}
int fps_overlay_full_hud(void){return 1;}
int dm_gfx_fps_now(int *generated){if(generated)*generated=0;return -1;}
int mfx_enabled(void){return 0;}
int mfxup_output_enabled(void){return 0;}
uint64_t runtime_output_size_current(void){return 1920|(uint64_t)1080<<32;}
void runtime_aspect_view(double w,double h){(void)w;(void)h;}
int runtime_aspect_step(void){return -1;}
float runtime_aspect_now(void){return 0;}

static NSEvent *key(NSEventModifierFlags flags){
 return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
     windowNumber:window.windowNumber context:nil characters:@"q" charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:12];
}
int main(int argc,char **argv){
 @autoreleasepool{
  if(argc!=3)return 1;
  mode=atoi(argv[1]);
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
  window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,320,180)
      styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  window.contentView=[[TOTKInputView alloc] initWithFrame:window.contentView.bounds];
  [window makeFirstResponder:window.contentView];
  front_install_menu();
  [NSApp finishLaunching];
  if(atoi(argv[2])){
   NSTextField *field=[[NSTextField alloc] initWithFrame:NSMakeRect(10,10,200,25)];
   [window.contentView addSubview:field];[window makeFirstResponder:field];
   if(![window.firstResponder isKindOfClass:NSTextView.class])runtime_fail("text field has no editor");
  }
  if([NSApp.mainMenu performKeyEquivalent:key(0)])runtime_fail("plain Q triggered Quit");
  if([NSApp.mainMenu performKeyEquivalent:key(NSEventModifierFlagControl)])runtime_fail("Control-Q triggered Quit");
  dispatch_async(dispatch_get_main_queue(),^{
   start=clock_ns();
   if(![NSApp.mainMenu performKeyEquivalent:key(NSEventModifierFlagCommand)])runtime_fail("Command-Q was not handled");
   /* Only the save-wait path returns. The close callback must exit the process. */
  });
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{runtime_fail("Quit did not exit");});
  [NSApp run];
  runtime_fail("event loop returned");
 }
}
