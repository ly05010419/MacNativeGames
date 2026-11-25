/* Unified macOS controls. Main-thread UI state; rendering requests latch on the guest present boundary. */
#pragma once
#include "control_policy.h"
#include "lm3_frame_policy.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_present_policy.h"
extern void mfx_set(int mode);
extern uint64_t mfxup_completed(int context);
extern uint64_t nvn_control_render_observed(void);

enum { TOTK_WINDOWED, TOTK_FULLSCREEN, TOTK_BORDERLESS };
static const unsigned displaySizes[][2]={{1280,720},{1600,900},{1920,1080},{2560,1440}};
static int displayResolution=2,displayMode=TOTK_WINDOWED,controlQuality=1,controlVsync=1,controlCap,controlFI;
static BOOL displayTransition,displayBorderless,controlsLoaded;
static NSRect displayWindowFrame;
static NSUserDefaults *displayDefaults;
static BOOL lm3DisplayPrepared;
static unsigned lm3StartWidth,lm3StartHeight;
static _Atomic uint64_t controlKeys[5];
static void display_apply_mode(void);
static void controls_apply(void);
static const unsigned defaultKeys[5]={122,18,19,20,21}; // F1 menu, 1 overlays, 2 interpolation, 3 upscale preset
static uint64_t hotkey_pack(unsigned code,unsigned mods){return (code+1)|((uint64_t)mods<<8);}
static unsigned hotkey_mods(NSEventModifierFlags flags){return ((flags&NSEventModifierFlagCommand)?1:0)|((flags&NSEventModifierFlagOption)?2:0)|((flags&NSEventModifierFlagControl)?4:0)|((flags&NSEventModifierFlagShift)?8:0);}
static NSString *hotkey_title(uint64_t key){
 if(!key)return @(TOTK_UI("未分配","Unassigned"));unsigned c=(unsigned)(key&255)-1,m=(unsigned)(key>>8);
 NSDictionary *names=@{@122:@"F1",@120:@"F2",@99:@"F3",@118:@"F4",@96:@"F5",@97:@"F6",@98:@"F7",@100:@"F8",@101:@"F9",@109:@"F10",@103:@"F11",@111:@"F12",@18:@"1",@19:@"2",@20:@"3",@21:@"4",@23:@"5",@22:@"6",@26:@"7",@28:@"8",@25:@"9",@29:@"0",@0:@"A",@11:@"B",@8:@"C",@2:@"D",@14:@"E",@3:@"F",@5:@"G",@4:@"H",@34:@"I",@38:@"J",@40:@"K",@37:@"L",@46:@"M",@45:@"N",@31:@"O",@35:@"P",@12:@"Q",@15:@"R",@1:@"S",@17:@"T",@32:@"U",@9:@"V",@13:@"W",@7:@"X",@16:@"Y",@6:@"Z",@49:@"Space",@43:@",",@48:@"Tab"};
 return [NSString stringWithFormat:@"%@%@%@%@%@",m&4?@"⌃":@"",m&2?@"⌥":@"",m&8?@"⇧":@"",m&1?@"⌘":@"",names[@(c)]?:[NSString stringWithFormat:@"Key %u",c]];
}
static void display_save(void){
 [displayDefaults setInteger:displayResolution forKey:@"resolution"];[displayDefaults setInteger:displayMode forKey:@"mode"];
 [displayDefaults setInteger:controlQuality forKey:@"upscalePreset"];[displayDefaults setBool:controlVsync forKey:@"vsync"];
 [displayDefaults setInteger:controlCap forKey:@"nativeFrameCap"];[displayDefaults setBool:controlFI forKey:@"frameGeneration"];
 NSMutableArray *keys=[NSMutableArray new];for(int i=0;i<5;i++)[keys addObject:@(atomic_load(&controlKeys[i]))];[displayDefaults setObject:keys forKey:@"shortcuts"];
}
static void display_load(void){
 const char *owner=getenv("TOTK_SLOT_OWNER");NSString *suite=owner&&*owner&&strcmp(owner,"user")?@"org.macnativegames.lm3.display.tests":@"org.macnativegames.lm3.display";
 displayDefaults=[[NSUserDefaults alloc] initWithSuiteName:suite];
 NSNumber *r=[displayDefaults objectForKey:@"resolution"],*m=[displayDefaults objectForKey:@"mode"],*q=[displayDefaults objectForKey:@"upscalePreset"];
 displayResolution=r&&r.integerValue>=0&&r.integerValue<4?r.intValue:(!strcmp(TOTK_GAME_DIR,"v140")?3:2);displayMode=m&&m.integerValue>=0&&m.integerValue<=2?m.intValue:0;
 (void)q;controlQuality=0;
 controlVsync=[displayDefaults objectForKey:@"vsync"]?[displayDefaults boolForKey:@"vsync"]:1;
 controlCap=(int)[displayDefaults integerForKey:@"nativeFrameCap"];if(controlCap!=30&&controlCap!=60)controlCap=60;
 if(runtime_lm3_target_fps()==30)controlCap=30;
 if(strcmp(TOTK_GAME_DIR,"v140"))controlCap=30;
 controlFI=0;
 NSArray *keys=[displayDefaults arrayForKey:@"shortcuts"];
 for(int i=0;i<5;i++){uint64_t k=keys.count>(NSUInteger)i?[keys[i] unsignedLongLongValue]:hotkey_pack(defaultKeys[i],0);if((k&255)>128||(k>>8)>15)k=hotkey_pack(defaultKeys[i],0);atomic_store(&controlKeys[i],k);}
 for(int i=0;i<5;i++)for(int j=0;j<i;j++)if(atomic_load(&controlKeys[i])&&atomic_load(&controlKeys[i])==atomic_load(&controlKeys[j]))atomic_store(&controlKeys[i],0);
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


@interface TOTKDisplaySettings:NSWindowController<NSWindowDelegate>
@property(strong) NSPopUpButton *resolution,*mode,*quality,*aa;
@property(strong) NSButton *vsync,*generation;
@property(strong) NSTextField *cap,*status,*error;
@property(strong) NSTabView *tabs;
@property(strong) NSArray<NSButton *> *hotkeyButtons;
@property(strong) NSMutableArray<NSNumber *> *draftKeys;
@property(strong) id keyMonitor;
@property(strong) NSTimer *statusTimer;
-(void)showSettings:(id)sender;
-(void)applySettings:(id)sender;
-(void)cancelSettings:(id)sender;
-(void)refreshStatus;
@end
static TOTKDisplaySettings *displaySettings;
static void controls_apply(void){
 if(!controlsLoaded)display_load();
 unsigned w=displaySizes[displayResolution][0],h=displaySizes[displayResolution][1];
 /* Guest textures are allocated once at launch. Save a changed selection for
    the next launch instead of resizing live pools from the UI thread. */
 if(!lm3DisplayPrepared){
  char size[32];snprintf(size,sizeof size,"%ux%u",w,h);if(!strcmp(TOTK_GAME_DIR,"v140"))setenv("LM3_RENDER_SIZE",size,0);
  const char *e=getenv("LM3_RENDER_SIZE");
  if(!e||sscanf(e,"%ux%u",&lm3StartWidth,&lm3StartHeight)!=2){lm3StartWidth=w;lm3StartHeight=h;}
  for(int i=0;i<4;i++)if(displaySizes[i][0]==lm3StartWidth&&displaySizes[i][1]==lm3StartHeight)displayResolution=i;
  lm3DisplayPrepared=YES;
 }
 nvn_control_pacing(controlVsync,controlCap);mfx_set(0);
 extern void dm_gfx_set_flag(const char *,int);dm_gfx_set_flag("mfx_upscale",0);dm_gfx_set_flag("mfx_output",0);
 if(window&&metalLayer){front_sync_drawable();display_apply_mode();}
}
void runtime_controls_prepare(void){@autoreleasepool{if(!controlsLoaded)display_load();controls_apply();}}
static NSView *controls_tab(NSTabView *tabs,NSString *name){NSTabViewItem *item=[[NSTabViewItem alloc] initWithIdentifier:name];item.label=name;item.view=[[NSView alloc] initWithFrame:NSMakeRect(0,0,600,390)];[tabs addTabViewItem:item];return item.view;}
static void controls_label(NSView *v,NSString *text,CGFloat y){NSTextField *l=[NSTextField labelWithString:text];l.frame=NSMakeRect(20,y,190,25);[v addSubview:l];}
static NSTextField *controls_hint(NSView *v,NSString *text,CGFloat y,CGFloat height){NSTextField *l=[NSTextField wrappingLabelWithString:text];l.frame=NSMakeRect(20,y,550,height);l.font=[NSFont systemFontOfSize:13];l.textColor=NSColor.secondaryLabelColor;[v addSubview:l];return l;}
static NSPopUpButton *controls_popup(NSView *v,NSArray *items,CGFloat y){NSPopUpButton *b=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(218,y,350,28) pullsDown:NO];[b addItemsWithTitles:items];[v addSubview:b];return b;}
@implementation TOTKDisplaySettings
-(instancetype)init{
 NSPanel *p=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,660,550) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
 if((self=[super initWithWindow:p])){
  p.title=@(TOTK_UI("控制菜单","Game Controls"));p.releasedWhenClosed=NO;p.delegate=self;
  self.tabs=[[NSTabView alloc] initWithFrame:NSMakeRect(20,100,620,425)];[p.contentView addSubview:self.tabs];
  NSView *graphics=controls_tab(self.tabs,@(TOTK_UI("画面","Graphics")));
  controls_label(graphics,@(TOTK_UI("目标分辨率","Target resolution")),326);
  self.resolution=controls_popup(graphics,@[@"1280 × 720",@"1600 × 900",@"1920 × 1080",@"2560 × 1440"],322);
  self.resolution.enabled=!strcmp(TOTK_GAME_DIR,"v140");
  controls_label(graphics,@(TOTK_UI("MetalFX 空间超分","MetalFX spatial upscaling")),273);
  self.quality=controls_popup(graphics,@[@(TOTK_UI("关闭 · 100% 渲染","Off · 100% rendering")),@(TOTK_UI("质量 · 约 67% 渲染","Quality · about 67%")),@(TOTK_UI("平衡 · 约 59% 渲染","Balanced · about 59%")),@(TOTK_UI("性能 · 50% 渲染","Performance · 50%"))],269);
  self.quality.enabled=NO;
  controls_label(graphics,@(TOTK_UI("抗锯齿","Antialiasing")),220);
  self.aa=controls_popup(graphics,@[@(TOTK_UI("FXAA（游戏原有）","FXAA (game's original)")),@(TOTK_UI("TAA / 时域超分（暂不可用）","TAA / temporal scaling (unavailable)"))],216);
  self.aa.autoenablesItems=NO;[self.aa itemAtIndex:1].enabled=NO;self.aa.toolTip=@(TOTK_UI("时域超分需要可靠的运动矢量和抖动采样；当前移植尚未提供完整输入。","Temporal scaling needs validated motion vectors and jittered samples, not fully provided by this port."));
  controls_label(graphics,@(TOTK_UI("窗口模式","Window mode")),167);
  self.mode=controls_popup(graphics,@[@(TOTK_UI("窗口","Windowed")),@(TOTK_UI("全屏（独立空间）","Full screen (separate Space)")),@(TOTK_UI("无边框窗口","Borderless window"))],163);
  controls_hint(graphics,@(TOTK_UI("1.4.0 按所选分辨率原生绘制，关闭动态降分辨率。\n分辨率更改在下次启动生效；窗口大小只改变显示大小。\n原生 2K 的性能取决于场景，30 帧可降低负载。","Version 1.4.0 renders at the selected native resolution with dynamic scaling off.\nResolution changes apply on the next launch; window size only affects presentation.\nNative 1440p performance varies by scene. Choose 30 FPS to reduce load.")),37,100);
  NSView *frames=controls_tab(self.tabs,@(TOTK_UI("帧率","Frame Rate")));
  self.vsync=[NSButton checkboxWithTitle:@(TOTK_UI("垂直同步","Vertical sync")) target:nil action:nil];self.vsync.frame=NSMakeRect(20,316,540,28);[frames addSubview:self.vsync];
  controls_label(frames,@(TOTK_UI("真实帧率上限","Real frame rate limit")),263);
  self.cap=[[NSTextField alloc] initWithFrame:NSMakeRect(218,261,100,27)];[frames addSubview:self.cap];controls_hint(frames,@(TOTK_UI("可填 30 或 60 FPS","Choose 30 or 60 FPS")),226,26);
  self.cap.enabled=!strcmp(TOTK_GAME_DIR,"v140");
  self.generation=[NSButton checkboxWithTitle:@(TOTK_UI("MetalFX 补帧（每个真实帧之间生成一帧）","MetalFX frame generation (one extra frame per pair)")) target:nil action:nil];self.generation.frame=NSMakeRect(20,175,560,30);[frames addSubview:self.generation];
  self.generation.enabled=NO;
  controls_hint(frames,@(TOTK_UI("60 帧使用游戏实际绘制的帧，不启用补帧。\n复杂场景和首次编译着色器时，帧率可能低于目标。\n30 帧启动模式需要重新启动才能提高到 60 帧。","60 FPS uses real game frames, with frame generation disabled.\nComplex scenes and first-time shader compilation can lower the achieved rate.\nA launch limited to 30 FPS must restart before increasing to 60 FPS.")),38,110);
  NSView *keys=controls_tab(self.tabs,@(TOTK_UI("快捷键","Shortcuts")));
  NSMutableArray *buttons=[NSMutableArray new];NSArray *actions=@[@(TOTK_UI("打开控制菜单","Open controls")),@(TOTK_UI("显示 / 隐藏提示","Show / hide overlays")),@(TOTK_UI("开关补帧","Toggle frame generation")),@(TOTK_UI("切换超分档位","Cycle upscale preset")),@(TOTK_UI("切换超宽屏（支持版本）","Toggle ultrawide (supported versions)"))];
  for(int i=0;i<5;i++){CGFloat y=332-i*48;controls_label(keys,actions[i],y);NSButton *b=[NSButton buttonWithTitle:@"" target:self action:@selector(recordShortcut:)];b.frame=NSMakeRect(218,y-3,350,30);b.tag=i;[keys addSubview:b];[buttons addObject:b];}self.hotkeyButtons=buttons;
  self.hotkeyButtons[2].enabled=NO;self.hotkeyButtons[3].enabled=NO;
  controls_hint(keys,@(TOTK_UI("点击右侧按钮，再按要绑定的按键或组合键。Esc 取消录入；Delete 清除。\n⌘, 始终可以打开菜单。补帧和超分快捷键暂不可用。","Click a button and press a key or chord. Esc cancels recording; Delete clears it.\nCommand+, always opens the menu. Frame-generation and upscale shortcuts are unavailable.")),27,92);
  self.status=controls_hint(p.contentView,@"",65,28);self.status.frame=NSMakeRect(26,65,608,30);
  self.error=controls_hint(p.contentView,@"",38,24);self.error.textColor=NSColor.systemRedColor;self.error.frame=NSMakeRect(26,38,405,24);
  NSButton *cancel=[NSButton buttonWithTitle:@(TOTK_UI("取消","Cancel")) target:self action:@selector(cancelSettings:)];cancel.frame=NSMakeRect(444,16,88,32);cancel.keyEquivalent=@"\e";[p.contentView addSubview:cancel];
  NSButton *apply=[NSButton buttonWithTitle:@(TOTK_UI("应用","Apply")) target:self action:@selector(applySettings:)];apply.frame=NSMakeRect(541,16,88,32);apply.keyEquivalent=@"\r";[p.contentView addSubview:apply];
 }return self;
}
-(void)stopRecording{if(self.keyMonitor){[NSEvent removeMonitor:self.keyMonitor];self.keyMonitor=nil;}for(int i=0;i<5;i++)self.hotkeyButtons[i].title=hotkey_title(self.draftKeys[i].unsignedLongLongValue);}
-(void)recordShortcut:(NSButton *)button{
 [self stopRecording];button.title=@(TOTK_UI("请按快捷键（Esc 取消）","Press a shortcut (Esc cancels)"));NSInteger index=button.tag;__weak __typeof__(self) weak=self;
 self.keyMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *e){
  __typeof__(self) strong=weak;if(!strong)return e;if(e.isARepeat)return nil;
  if(e.keyCode==53){[strong stopRecording];return nil;}
  unsigned mods=hotkey_mods(e.modifierFlags);uint64_t key=e.keyCode==51?0:hotkey_pack(e.keyCode,mods);
  if(key==hotkey_pack(12,1)||key==hotkey_pack(43,1)){strong.error.stringValue=@(TOTK_UI("⌘Q 和 ⌘, 是保留快捷键。","Command+Q and Command+, are reserved."));[strong stopRecording];return nil;}
  strong.draftKeys[index]=@(key);[strong stopRecording];return nil;
 }];
}
-(void)refreshStatus{
 uint64_t applied=nvn_control_render_observed();
 unsigned rw=(applied>>24)&4095,rh=(applied>>36)&4095,cw=applied&4095,ch=(applied>>12)&4095;
 BOOL pending=displaySizes[displayResolution][0]!=lm3StartWidth||displaySizes[displayResolution][1]!=lm3StartHeight;
 self.status.stringValue=applied?[NSString stringWithFormat:@(TOTK_UI("实际 3D %u×%u · 输出 %u×%u%@","Actual 3D %u×%u · output %u×%u%@")),rw,rh,cw,ch,pending?@(TOTK_UI(" · 分辨率重启后生效"," · restart to apply resolution")):@""]:@(TOTK_UI("等待游戏绘制 3D 场景…","Waiting for a rendered 3D scene…"));
}
-(void)showSettings:(id)sender{
 (void)sender;if(self.window.sheetParent||displayTransition)return;
 [self.resolution selectItemAtIndex:displayResolution];[self.mode selectItemAtIndex:displayMode];[self.quality selectItemAtIndex:controlQuality];
 self.vsync.state=controlVsync;self.cap.integerValue=controlCap;self.generation.state=controlFI;self.error.stringValue=@"";
 self.draftKeys=[NSMutableArray new];for(int i=0;i<5;i++)[self.draftKeys addObject:@(atomic_load(&controlKeys[i]))];[self stopRecording];[self refreshStatus];
 self.statusTimer=[NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(refreshStatus) userInfo:nil repeats:YES];
 runtime_input_clear();[window beginSheet:self.window completionHandler:nil];
}
-(BOOL)windowShouldClose:(NSWindow *)sender{(void)sender;[self cancelSettings:nil];return NO;}
-(void)cancelSettings:(id)sender{(void)sender;[self stopRecording];[self.statusTimer invalidate];self.statusTimer=nil;[window endSheet:self.window];[self.window orderOut:nil];}
-(void)applySettings:(id)sender{
 (void)sender;[self stopRecording];NSString *s=[self.cap.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];NSScanner *scan=[NSScanner scannerWithString:s];int cap=0;
 if(![scan scanInt:&cap]||!scan.isAtEnd||(cap!=30&&cap!=60)){self.error.stringValue=@(TOTK_UI("帧率上限应为 30 或 60。","Frame limit must be 30 or 60."));return;}
 if(cap>runtime_lm3_target_fps()&&runtime_lm3_target_fps()>0){self.error.stringValue=@(TOTK_UI("当前以 30 帧模式启动，请重启后使用 60 帧。","Restart without the 30 FPS launch limit to use 60 FPS."));return;}
 for(int i=0;i<5;i++)for(int j=0;j<i;j++)if(self.draftKeys[i].unsignedLongLongValue&&[self.draftKeys[i] isEqual:self.draftKeys[j]]){self.error.stringValue=@(TOTK_UI("快捷键重复，请更换后应用。","Duplicate shortcut; choose another binding."));return;}
 displayResolution=(int)self.resolution.indexOfSelectedItem;displayMode=(int)self.mode.indexOfSelectedItem;controlQuality=(int)self.quality.indexOfSelectedItem;controlCap=cap;controlVsync=self.vsync.state==NSControlStateValueOn;controlFI=self.generation.state==NSControlStateValueOn;
 for(int i=0;i<5;i++)atomic_store(&controlKeys[i],self.draftKeys[i].unsignedLongLongValue);
 display_save();[self cancelSettings:nil];controls_apply();
}
@end
static void controls_action(unsigned action){
 if(action==0){if(displaySettings.window.sheetParent)[displaySettings cancelSettings:nil];else [displaySettings showSettings:nil];}
 if(action==1)runtime_shortcut_help_toggle();
 if(action==2||action==3)return; /* Unvalidated interpolation/upscale paths stay off for LM3. */
 if(action==4){runtime_aspect_toggle();front_sync_drawable();}
}
int runtime_controls_active(void){return controlsLoaded;}
int runtime_controls_handle_key(unsigned code,unsigned modifiers){
 if(!controlsLoaded)return 0;uint64_t key=hotkey_pack(code,modifiers);
 for(unsigned i=0;i<5;i++)if(atomic_load(&controlKeys[i])==key){dispatch_async(dispatch_get_main_queue(),^{controls_action(i);});return 1;}
 return 0;
}
void runtime_front_control_test(const char *command){
 if(!strncmp(command,"status",6)){dispatch_async(dispatch_get_main_queue(),^{uint64_t r=nvn_control_render_observed();fprintf(stderr,"[controls-status] frame=%llu scene=%ux%u canvas=%ux%u output=%.0fx%.0f MetalFX=%llu vsync=%d layer-sync=%d cap=%d FI=%d\n",(unsigned long long)dm_nvn_frame_count(),(unsigned)((r>>24)&4095),(unsigned)((r>>36)&4095),(unsigned)(r&4095),(unsigned)((r>>12)&4095),metalLayer.drawableSize.width,metalLayer.drawableSize.height,(unsigned long long)mfxup_completed(1),controlVsync,metalLayer.displaySyncEnabled,controlCap,controlFI);});return;}

 unsigned w,h;int q,s,cap,fi;if(sscanf(command,"%u %u %d %d %d %d",&w,&h,&q,&s,&cap,&fi)!=6||!totk_control_render(w,h,q)||q||fi||(cap!=30&&cap!=60))return;
 dispatch_async(dispatch_get_main_queue(),^{for(unsigned i=0;i<4;i++)if(displaySizes[i][0]==w&&displaySizes[i][1]==h){displayResolution=(int)i;controlQuality=q;controlVsync=!!s;controlCap=cap;controlFI=!!fi;controls_apply();return;}});
}
