/* The picture settings (target resolution, MetalFX upscaling preset, window mode, aspect ratio, vsync, frame limit, frame
   generation): main-thread state, saved in the org.macnativegames.totk.display defaults, applied by controls_apply; the
   rendering requests latch on the guest present boundary. The settings panel (key 1, settings_game.h) edits them; the old
   "Game Controls" window and its F1-F4 shortcuts are gone (the panel replaced them). */
#pragma once
#include "control_policy.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_present_policy.h"
extern void mfx_set(int mode);
extern uint64_t mfxup_completed(int context);
extern uint64_t nvn_control_render_observed(void);

enum { TOTK_WINDOWED, TOTK_FULLSCREEN, TOTK_BORDERLESS };
static const unsigned displaySizes[][2]={{1280,720},{1600,900},{1920,1080},{2560,1440}};
static int displayResolution=2,displayMode=TOTK_WINDOWED,controlQuality=1,controlVsync=1,controlCap,controlFI,displayAspect;
static const float displayAspects[]={0,16.0f/10.0f,21.0f/9.0f};   // 16:9 (the game's own), 16:10, 21:9 (aspect_patch.h)
static BOOL displayTransition,displayBorderless,controlsLoaded;
static NSRect displayWindowFrame;
static NSUserDefaults *displayDefaults;
static void display_apply_mode(void);
static void controls_apply(void);
static void display_save(void){
 [displayDefaults setInteger:displayResolution forKey:@"resolution"];[displayDefaults setInteger:displayMode forKey:@"mode"];
 [displayDefaults setInteger:controlQuality forKey:@"upscalePreset"];[displayDefaults setBool:controlVsync forKey:@"vsync"];
 [displayDefaults setInteger:displayAspect forKey:@"aspect"];[displayDefaults setInteger:controlCap forKey:@"nativeFrameCap"];[displayDefaults setBool:controlFI forKey:@"frameGeneration"];
}
static void display_load(void){
 const char *owner=getenv("TOTK_SLOT_OWNER");NSString *suite=owner&&*owner&&strcmp(owner,"user")?@"org.macnativegames.totk.display.tests":@"org.macnativegames.totk.display";
 displayDefaults=[[NSUserDefaults alloc] initWithSuiteName:suite];
 NSNumber *r=[displayDefaults objectForKey:@"resolution"],*m=[displayDefaults objectForKey:@"mode"],*q=[displayDefaults objectForKey:@"upscalePreset"];
 displayResolution=r&&r.integerValue>=0&&r.integerValue<4?r.intValue:2;displayMode=m&&m.integerValue>=0&&m.integerValue<=2?m.intValue:0;
 controlQuality=q&&q.integerValue>=0&&q.integerValue<=3?q.intValue:1;
 controlVsync=[displayDefaults objectForKey:@"vsync"]?[displayDefaults boolForKey:@"vsync"]:1;
 controlCap=(int)[displayDefaults integerForKey:@"nativeFrameCap"];if(controlCap!=0&&(controlCap<15||controlCap>240))controlCap=0;
 controlFI=[displayDefaults boolForKey:@"frameGeneration"];
 displayAspect=(int)[displayDefaults integerForKey:@"aspect"];if(displayAspect<0||displayAspect>2)displayAspect=0;
 controlsLoaded=YES;
}
static void display_presentation(void) {
 if(displayTransition||(window.styleMask&NSWindowStyleMaskFullScreen))return;
 NSApp.presentationOptions=displayBorderless&&NSApp.active?(NSApplicationPresentationAutoHideDock|NSApplicationPresentationAutoHideMenuBar):NSApplicationPresentationDefault;
}
static void display_restore_window(void) {
 if(!displayBorderless)return;
 displayBorderless=NO;
 window.styleMask=NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable;
 window.level=NSNormalWindowLevel;display_presentation();
 if(!NSIsEmptyRect(displayWindowFrame)) [window setFrame:displayWindowFrame display:YES];
}
static void display_apply_mode(void) {
 if(displayTransition)return;
 BOOL full=(window.styleMask&NSWindowStyleMaskFullScreen)!=0;
 if(full){if(displayMode!=TOTK_FULLSCREEN){displayTransition=YES;[window toggleFullScreen:nil];}return;}
 if(displayMode==TOTK_FULLSCREEN){display_restore_window();displayTransition=YES;[window toggleFullScreen:nil];return;}
 if(displayMode==TOTK_BORDERLESS){
  NSScreen *screen=window.screen?:NSScreen.mainScreen;
  if(!screen)return;
  if(!displayBorderless){displayWindowFrame=window.frame;displayBorderless=YES;window.styleMask=NSWindowStyleMaskBorderless;}
  // A normal-level window fills this desktop without becoming an always-on-top overlay.
  if(!NSEqualRects(window.frame,screen.frame))[window setFrame:screen.frame display:YES];
 }else display_restore_window();
 display_presentation();front_sync_drawable();
}


static void controls_apply(void){
 if(!controlsLoaded)display_load();
 unsigned w=displaySizes[displayResolution][0],h=displaySizes[displayResolution][1];
 runtime_render_configure(w,h,controlQuality);
 { const char *env=getenv("TOTK_ASPECT"); if(!env||!*env)runtime_aspect_set_ratio(displayAspects[displayAspect]); }nvn_control_pacing(controlVsync,controlCap);mfx_set(controlFI?3:0);
 if(window&&metalLayer){front_sync_drawable();display_apply_mode();}
}
void runtime_controls_prepare(void){@autoreleasepool{if(!controlsLoaded)display_load();controls_apply();}}
void runtime_front_control_test(const char *command){
 if(!strncmp(command,"status",6)){dispatch_async(dispatch_get_main_queue(),^{uint64_t r=nvn_control_render_observed();fprintf(stderr,"[controls-status] frame=%llu scene=%ux%u canvas=%ux%u output=%.0fx%.0f MetalFX=%llu vsync=%d layer-sync=%d cap=%d FI=%d\n",(unsigned long long)dm_nvn_frame_count(),(unsigned)((r>>24)&4095),(unsigned)((r>>36)&4095),(unsigned)(r&4095),(unsigned)((r>>12)&4095),metalLayer.drawableSize.width,metalLayer.drawableSize.height,(unsigned long long)mfxup_completed(1),controlVsync,metalLayer.displaySyncEnabled,controlCap,controlFI);});return;}

 unsigned w,h;int q,s,cap,fi;if(sscanf(command,"%u %u %d %d %d %d",&w,&h,&q,&s,&cap,&fi)!=6||!totk_control_render(w,h,q)||cap<0||cap>240)return;
 dispatch_async(dispatch_get_main_queue(),^{for(unsigned i=0;i<4;i++)if(displaySizes[i][0]==w&&displaySizes[i][1]==h){displayResolution=(int)i;controlQuality=q;controlVsync=!!s;controlCap=cap;controlFI=!!fi;controls_apply();return;}});
}
