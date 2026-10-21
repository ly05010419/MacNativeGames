/* Interactive-session core check of production AppKit display controls.
   No guest code, saves or Metal commands. Run on a logged-in Mac, not headless CI. */
#include "../games/totk/runtime/frontend.m"
#include <assert.h>
void runtime_input_key(unsigned c,int d){(void)c;(void)d;}
void runtime_input_key_modifiers(unsigned c,int d,unsigned m){(void)c;(void)d;(void)m;}
static RuntimeKeyboardBindings probeKeyboard;
static RuntimeControllerBindings probeController;
static int probeBindingsLoaded,probeSuspended;
static void probe_bindings_init(void){if(!probeBindingsLoaded){input_bindings_default(&probeKeyboard);controller_map_default(probeController.map,probeController.modes);probeBindingsLoaded=1;}}
void runtime_input_bindings_get(RuntimeKeyboardBindings *b){probe_bindings_init();*b=probeKeyboard;}
int runtime_input_bindings_set(const RuntimeKeyboardBindings *b){if(!input_bindings_valid(b))return 0;probeKeyboard=*b;return 1;}
void runtime_gamepad_bindings_get(RuntimeControllerBindings *b){probe_bindings_init();*b=probeController;}
int runtime_gamepad_bindings_set(const RuntimeControllerBindings *b){if(!controller_bindings_valid(b))return 0;probeController=*b;return 1;}
void runtime_input_mouse_button(unsigned b,int d){(void)b;(void)d;}
void runtime_input_mouse_motion(double x,double y){(void)x;(void)y;}
void runtime_input_suspend(int s){probeSuspended=s;}
int runtime_input_suspended(void){return probeSuspended;}
static float testAspect;
int runtime_aspect_toggle(void){testAspect=testAspect?0:21.0f/9;return 1;}
float runtime_aspect_now(void){return testAspect;}
int dm_gfx_fps_now(int *generated){*generated=0;return 60;}
int mfx_enabled(void){return 0;}
int mfxup_output_enabled(void){return 0;}
uint64_t runtime_output_size_current(void){return 1920ull|(1080ull<<32);}
static int testTAA;
int taa_enabled(void){return testTAA;}
void taa_toggle(void){testTAA=!testTAA;}
int fps_overlay_full_hud(void){return 1;}
int hud_compact(void){return 1;}
int hud_snapshot(int l,char *out,size_t n,int *highlight){(void)l;*highlight=0;snprintf(out,n,"FPS: 60");return 1;}
void runtime_input_clear(void){}
void runtime_oe_push_message(uint32_t m){(void)m;}
void runtime_gamepad_start(void){}
int runtime_fs_save_busy(void){return 0;}
uint64_t dm_nvn_frame_count(void){return 0;}
uint64_t runtime_now_ns(void){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
_Noreturn void runtime_fail(const char *s){fprintf(stderr,"FAIL: %s\n",s);exit(1);}
int fps_overlay_toggle_hints(void){return 0;}
void hle_bind_system_event(void *e,int n){(void)e;(void)n;}
void hle_signal_system_event(void *e){(void)e;}
static uint64_t appliedConfig;static int lastVsync,lastCap,lastFI;
int runtime_render_configure(unsigned w,unsigned h,int quality){uint64_t r=totk_control_render(w,h,quality);if(!r)return 0;appliedConfig=w|((uint64_t)h<<12)|((r&4095)<<24)|(((r>>32)&4095)<<36)|((uint64_t)quality<<48);return 1;}
uint64_t runtime_render_applied(void){return appliedConfig;}
uint64_t nvn_control_render_observed(void){return appliedConfig;}
void nvn_control_pacing(int sync,int cap){lastVsync=sync;lastCap=cap;}
int nvn_control_vsync(void){return lastVsync;}
void mfx_set(int m){lastFI=m;}
uint64_t mfxup_completed(int ctx){(void)ctx;return 123;}
static uint32_t displayID;static CGDisplayModeRef originalMode;
static unsigned stage;
static void capture_panel(void){
 const char *path=getenv("TOTK_DISPLAY_PROBE_IMAGE");if(!path)return;
 NSView *view=displaySettings.window.contentView;
 NSBitmapImageRep *rep=[view bitmapImageRepForCachingDisplayInRect:view.bounds];[view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
 assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(path) atomically:YES]);
}
static void check_fixed(void){
 assert(CGSizeEqualToSize(metalLayer.drawableSize,CGSizeMake(1920,1080)));
 assert(fabs(metalLayer.frame.size.width/metalLayer.frame.size.height-16.0/9)<0.001);
 CGDisplayModeRef mode=CGDisplayCopyDisplayMode(displayID);
 assert(CGDisplayModeGetPixelWidth(mode)==CGDisplayModeGetPixelWidth(originalMode));
 assert(CGDisplayModeGetPixelHeight(mode)==CGDisplayModeGetPixelHeight(originalMode));
 CGDisplayModeRelease(mode);
}
static void tick(void){
 if(displayTransition){dispatch_after(dispatch_time(DISPATCH_TIME_NOW,200*NSEC_PER_MSEC),dispatch_get_main_queue(),^{tick();});return;}
 check_fixed();
 switch(stage++){
 case 0:
  assert(displayMode==TOTK_WINDOWED);
  [window setContentSize:NSMakeSize(800,700)];front_sync_drawable();check_fixed();
  [displaySettings showSettings:nil];assert(displaySettings.window.sheetParent==window);capture_panel();
  assert(displaySettings.hotkeyButtons.count==5);
  assert(shortcutHelp&&!shortcutHelp.hidden); // retain the upstream compact FPS/shader/load panel
  testAspect=21.0f/9;front_sync_drawable();assert(fabs(metalLayer.frame.size.width/metalLayer.frame.size.height-21.0/9)<0.001);
  testAspect=0;front_sync_drawable();check_fixed();
  // Cancel must leave the actual output unchanged.
  [displaySettings.resolution selectItemAtIndex:1];[displaySettings cancelSettings:nil];check_fixed();
  [displaySettings showSettings:nil];[displaySettings.mode selectItemAtIndex:TOTK_BORDERLESS];[displaySettings applySettings:nil];break;
 case 1:
  assert(displayBorderless);assert(NSEqualRects(window.frame,window.screen.frame));assert(window.canBecomeKeyWindow);
  [displaySettings showSettings:nil];[displaySettings.mode selectItemAtIndex:TOTK_WINDOWED];[displaySettings applySettings:nil];break;
 case 2:
  assert(!displayBorderless);assert((window.styleMask&NSWindowStyleMaskTitled)!=0);
  assert(fabs(window.contentView.bounds.size.width-800)<1);
  [displaySettings showSettings:nil];[displaySettings.mode selectItemAtIndex:TOTK_FULLSCREEN];[displaySettings applySettings:nil];break;
 case 3:
  assert((window.styleMask&NSWindowStyleMaskFullScreen)!=0);
  [displaySettings showSettings:nil];[displaySettings.mode selectItemAtIndex:TOTK_BORDERLESS];[displaySettings applySettings:nil];break;
 case 4:
  assert(displayBorderless);assert(!(window.styleMask&NSWindowStyleMaskFullScreen));
  displayMode=TOTK_WINDOWED;display_apply_mode();
  [displaySettings showSettings:nil];
  [displaySettings.quality selectItemAtIndex:3];
  displaySettings.cap.stringValue=@"oops";[displaySettings applySettings:nil];assert(displaySettings.window.sheetParent==window);
  displaySettings.cap.stringValue=@"40";displaySettings.vsync.state=NSControlStateValueOff;displaySettings.generation.state=NSControlStateValueOn;
  [displaySettings.resolution selectItemAtIndex:0];
  [displaySettings.mode selectItemAtIndex:TOTK_WINDOWED];[displaySettings applySettings:nil];
  assert(CGSizeEqualToSize(metalLayer.drawableSize,CGSizeMake(1280,720)));
  assert(lastVsync==0&&lastCap==40&&lastFI==3);assert(((appliedConfig>>24)&4095)==640&&((appliedConfig>>36)&4095)==360);
  assert(runtime_controls_handle_key(19,1)==0); // modifiers must match
  displayResolution=3;display_load();assert(displayResolution==0); // reload persisted settings
  displayResolution=-1;front_sync_drawable();
  assert(CGSizeEqualToSize(metalLayer.drawableSize,CGSizeMake(round(window.contentView.bounds.size.width*window.backingScaleFactor),round(window.contentView.bounds.size.height*window.backingScaleFactor))));
  [displayDefaults removePersistentDomainForName:@"org.macnativegames.totk.display.tests"];
  CGDisplayModeRelease(originalMode);puts("PASS: fixed output, aspect ratio, resize, three modes, cancel, preferences, unchanged display mode");fflush(stdout);exit(0);
 }
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,800*NSEC_PER_MSEC),dispatch_get_main_queue(),^{tick();});
}
static void start(void){dispatch_async(dispatch_get_main_queue(),^{
 runtime_controls_prepare();displayResolution=2;displayMode=TOTK_WINDOWED;display_apply_mode();front_sync_drawable();
 if(getenv("TOTK_DISPLAY_PROBE_PREVIEW")){[displaySettings showSettings:nil];return;}
 displayID=[window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];originalMode=CGDisplayCopyDisplayMode(displayID);tick();
});}
int main(int argc,char **argv){if(argc>1&&!strcmp(argv[1],"--preview"))setenv("TOTK_DISPLAY_PROBE_PREVIEW","1",1);setenv("TOTK_SLOT_OWNER","codex-display-probe",1);setenv("TOTK_ASSUME_FOCUS","1",1);
 @autoreleasepool {NSUserDefaults *d=[[NSUserDefaults alloc] initWithSuiteName:@"org.macnativegames.totk.display.tests"];[d removePersistentDomainForName:@"org.macnativegames.totk.display.tests"];}
 if(!getenv("TOTK_DISPLAY_PROBE_PREVIEW"))dispatch_after(dispatch_time(DISPATCH_TIME_NOW,45*NSEC_PER_SEC),dispatch_get_main_queue(),^{runtime_fail("display transition timeout");});
 runtime_front_run(start);return 0;
}
