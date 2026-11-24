/* The corner box (FPS, shader, load lines, playtime, controller battery, how to open the settings panel) and the FPS
   graph under it: one copy for every game (shared/README.md). Included by runtime/frontend.m after its hudBox,
   fpsGraphBox, fpsGraphEnabled and playtime; it needs settings_pad_family and settings_panel_pad_text from
   settings_panel.h and runtime_gamepad_battery_percent / _left_percent from gamepad.m. */
enum { FPS_GRAPH_SAMPLES = 61, FPS_GRAPH_WIDTH = 220, FPS_GRAPH_HEIGHT = 90 };
/* The system's frosted glass (macOS 26's NSGlassEffectView, its regular style) in the dark appearance with black at RUNTIME_GLASS_TINT over it for the type
   on it: the ground of this box and of the settings panel (user 2026-11-24, of the panel "很好", then "fps pannel 也是
   一样效果"). The clear style was too see-through over the game while its window was the active one. The class by
   name and its properties by key, so an SDK older than macOS 26 still builds this file; nil where the system has
   none (the caller keeps its plain dark ground). *tintView: the black layer's view, the glass's content. */
#define RUNTIME_GLASS_TINT 0.22   /* black over the glass, for the type on it */
enum { RUNTIME_GLASS_RADIUS = 20 };   /* one corner for every glass surface: the panel's and the box's are the same glass */
static NSView *runtime_glass_view(NSRect frame,NSView *__strong *tintView) {
 const CGFloat radius=RUNTIME_GLASS_RADIUS;
 Class glassClass=NSClassFromString(@"NSGlassEffectView");if(!glassClass)return nil;
 NSView *glass=[[glassClass alloc] initWithFrame:frame];[glass setValue:@0 forKey:@"style"];
 glass.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];   /* the glass follows its appearance: without this the box was the light glass, the panel the dark */[glass setValue:@(radius) forKey:@"cornerRadius"];
 NSView *dark=[[NSView alloc] initWithFrame:NSMakeRect(0,0,frame.size.width,frame.size.height)];dark.wantsLayer=YES;dark.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
 dark.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:RUNTIME_GLASS_TINT].CGColor;dark.layer.cornerRadius=radius;
 [glass setValue:dark forKey:@"contentView"];if(tintView)*tintView=dark;
 return glass;
}
@interface TOTKHudBox:NSView
@property(strong) NSTextField *label;
@property(strong) NSTimer *timer;
@property NSSize textSize;
-(void)position;
-(void)refresh;
@end
/* Compact HUD (default): one small box in the window's top-right corner with the FPS, shader and load lines (published by
   gfx/nvn/fps_overlay.c, refreshed twice a second); the settings panel's overlay row picks all, FPS + playtime or none.
   TOTK_HUD=corners: the lines stay in the picture's corners and there is no box. */
static int settings_pad_family(void);                 /* settings_panel.h: the controller family button names follow */
static NSString *settings_panel_pad_text(int family);  /* settings_panel.h: the controller's way to the panel */
/* The box's last line (user 2026-10-30): how to open the settings panel, as it is bound now: key 1, and with a
   controller connected its button or chord (L3 + R3 unless the key bindings set one). */
static NSString *hud_settings_hint(void) {
 int detected=-1;runtime_gamepad_type(&detected);
 if(detected<=GAMEPAD_TYPE_AUTO)return @(TOTK_UI("设置：键盘 1","Settings: key 1"));
 return [NSString stringWithFormat:@(TOTK_UI("设置：键盘 1 · 手柄 %@","Settings: key 1 · controller %@")),settings_panel_pad_text(settings_pad_family())];
}
/* The box's build line (user 2026-11-18: "版本号 旁边 显示 launcher的版本号（launcher版本号放第一）"): the version of the MNG
   Launcher that built or last updated the app, then the commit ("版本 1.0.6 · a0cd113"). launcher is what the app's
   Info.plist records (MNGLauncherVersion, build_app.launcher_version): a release's version, "dev" for a Launcher built
   from a checkout (as the Launcher's own label says 开发版 there), nothing for an app built without a Launcher or
   before this. nil without a commit. */
static NSString *hud_build_text(const char *launcher,const char *commit) {
 if(!commit||!*commit)return nil;
 if(!launcher||!*launcher)return [NSString stringWithFormat:@(TOTK_UI("版本 %s","build %s")),commit];
 if(!strcmp(launcher,"dev"))return [NSString stringWithFormat:@(TOTK_UI("版本 开发版 · %s","dev · build %s")),commit];
 return [NSString stringWithFormat:@(TOTK_UI("版本 %s · %s","Launcher %s · build %s")),launcher,commit];
}
@implementation TOTKHudBox
-(instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame])){self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0.04 alpha:0.72].CGColor;self.layer.cornerRadius=7;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.14].CGColor;
 /* the same glass as the settings panel, where the system has it: the plain dark ground and its border go */
 NSView *glass=runtime_glass_view(self.bounds,NULL);
 if(glass){glass.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;self.layer.backgroundColor=NSColor.clearColor.CGColor;self.layer.borderWidth=0;self.layer.cornerRadius=RUNTIME_GLASS_RADIUS;[self addSubview:glass];}
 self.label=[NSTextField labelWithString:@""];self.label.alignment=NSTextAlignmentLeft;self.label.maximumNumberOfLines=0;self.label.lineBreakMode=NSLineBreakByClipping;[self addSubview:self.label];
 [self refresh];if(hud_compact()){__weak TOTKHudBox *weak=self;self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;[weak refresh];}];}}return self;}
-(void)refresh{
 BOOL showText=hud_compact()&&fps_overlay_hud_mode()!=HUD_MODE_OFF;
 self.hidden=!showText&&!fpsGraphEnabled;self.label.hidden=!showText;
 if(showText){
  /* "极简 FPS" (HUD_MODE_BARE, user 2026-11-18: "只显示 fps和分辨率，放大3倍（只有这个 放大3倍）"): the FPS line alone, three
     times the size; no playtime, battery, build or settings hint under it. */
  BOOL bare=fps_overlay_hud_mode()==HUD_MODE_BARE;
  NSFont *mono=[NSFont monospacedSystemFontOfSize:bare?10.5*HUD_BARE_SCALE:10.5 weight:NSFontWeightMedium];
  NSColor *green=[NSColor colorWithCalibratedRed:80/255.0 green:1 blue:120/255.0 alpha:1],*yellow=[NSColor colorWithCalibratedRed:1 green:230/255.0 blue:40/255.0 alpha:1];
  NSMutableAttributedString *a=[NSMutableAttributedString new];
  // hud_line_shown: which lines the overlay choice shows (the hidden ones keep their last text, so they are skipped)
  static const int order[]=HUD_ORDER;   // FPS, shaders, libraries, render pipelines, in play, the other kinds, the rest, load (user 2026-11-16: "都要显示")
  for(int o=0;o<(int)(sizeof order/sizeof *order);o++){int i=order[o];char t[HUD_TEXT_MAX];int hl=0;if(!hud_line_shown(i)||!hud_snapshot(i,t,sizeof t,&hl))continue;
   [a appendAttributedString:[[NSAttributedString alloc] initWithString:[([NSString stringWithUTF8String:t] ?: @"") stringByAppendingString:@"\n"] attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:hl?yellow:green}]];}
  if(!a.length)[a appendAttributedString:[[NSAttributedString alloc] initWithString:@"FPS --\n" attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:green}]];
  if(playtime&&!bare)[a appendAttributedString:[[NSAttributedString alloc] initWithString:[playtime.text stringByAppendingString:@"\n"]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:green}]];
  int bpct=bare?-1:runtime_gamepad_battery_percent(),bleft=bare?-1:runtime_gamepad_battery_left_percent();
  if(bpct>=0)[a appendAttributedString:[[NSAttributedString alloc] initWithString:bleft>=0?[NSString stringWithFormat:@(TOTK_UI("手柄电量 左 %d%% 右 %d%%\n","Controller L %d%% R %d%%\n")),bleft,bpct]:[NSString stringWithFormat:@(TOTK_UI("手柄电量 %d%%\n","Controller %d%%\n")),bpct]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:green}]];
  /* The commit the app was built from (user 2026-10-25), last and grey: TOTK_SOURCE_COMMIT from the launcher; before it
     the version of the MNG Launcher that built the app (hud_build_text). */
  NSString *build=bare?nil:hud_build_text(getenv("TOTK_LAUNCHER_VERSION"),getenv("TOTK_SOURCE_COMMIT"));
  if(build)[a appendAttributedString:[[NSAttributedString alloc] initWithString:[build stringByAppendingString:@"\n"]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.6 alpha:1]}]];
  if(!bare)[a appendAttributedString:[[NSAttributedString alloc] initWithString:[hud_settings_hint() stringByAppendingString:@"\n"]
   attributes:@{NSFontAttributeName:mono,NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.6 alpha:1]}]];
  [a deleteCharactersInRange:NSMakeRange(a.length-1,1)];   // no empty line under the last one
  BOOL changed=![a isEqualToAttributedString:self.label.attributedStringValue];
  if(changed)self.label.attributedStringValue=a;
  if(changed||self.textSize.height==0){
   NSSize z=[self.label sizeThatFits:NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX)];
   self.textSize=NSMakeSize(ceil(z.width),ceil(z.height));
  }
 }else self.textSize=NSZeroSize;
 [self position];}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
-(void)viewDidMoveToSuperview{[super viewDidMoveToSuperview];[self position];}
-(void)layout{[super layout];[self position];}
-(void)position{if(!self.superview)return;
 CGFloat graphH=fpsGraphEnabled?FPS_GRAPH_HEIGHT:0,graphW=fpsGraphEnabled?FPS_GRAPH_WIDTH:0;
 CGFloat contentW=fmax(self.textSize.width,graphW);
 /* the text and graph 12 x 8 in from the edge (were 8 x 5): clear of the glass's round corners */
 CGFloat w=contentW+24,h=self.textSize.height+graphH+16,m=10;
 NSRect f=NSMakeRect(floor(self.superview.bounds.size.width-w-m),floor(self.superview.bounds.size.height-h-m),w,h);
 if(!NSEqualRects(self.frame,f))self.frame=f;
 self.label.frame=NSMakeRect(12,graphH+8,self.textSize.width,self.textSize.height);
 if(fpsGraphEnabled)fpsGraphBox.frame=NSMakeRect(12,8,contentW,FPS_GRAPH_HEIGHT);}
@end
@interface TOTKFpsGraph:NSView {
 int _samples[FPS_GRAPH_SAMPLES];
 NSUInteger _sampleCount;
}
@property(strong) NSTimer *timer;
-(void)setEnabled:(BOOL)enabled;
-(void)sample;
@end
@implementation TOTKFpsGraph
-(instancetype)initWithFrame:(NSRect)frame {
 if((self=[super initWithFrame:frame]))self.hidden=YES;
 return self;
}
-(void)setEnabled:(BOOL)enabled {
 self.hidden=!enabled;
 [self.timer invalidate];
 self.timer=nil;
 _sampleCount=0;
 if(!enabled)return;

 [self sample];
 __weak TOTKFpsGraph *weak=self;
 self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;[weak sample];}];
}
-(void)sample {
 int fps=fps_overlay_value();
 if(_sampleCount<FPS_GRAPH_SAMPLES)_samples[_sampleCount++]=fps;
 else {
  for(NSUInteger i=1;i<FPS_GRAPH_SAMPLES;i++)_samples[i-1]=_samples[i];
  _samples[FPS_GRAPH_SAMPLES-1]=fps;
 }
 [self setNeedsDisplay:YES];
}
-(void)drawRect:(NSRect)dirtyRect {
 (void)dirtyRect;
 int minFPS=-1,maxFPS=-1;
 for(NSUInteger i=0;i<_sampleCount;i++) {
  int fps=_samples[i];
  if(fps<0)continue;
  if(minFPS<0||fps<minFPS)minFPS=fps;
  if(fps>maxFPS)maxFPS=fps;
 }

 int currentFPS=_sampleCount?_samples[_sampleCount-1]:-1;
 NSString *currentText=currentFPS<0?@"--":[NSString stringWithFormat:@"%d",currentFPS];
 NSString *stats=maxFPS<0?@"min --   max --   cur --":
  [NSString stringWithFormat:@"min %d   max %d   cur %@",minFPS,maxFPS,currentText];
 NSDictionary *statsStyle=@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightMedium],
  NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.88 alpha:1]};
 [stats drawAtPoint:NSMakePoint(8,58) withAttributes:statsStyle];
 if(maxFPS<0)return;

 NSRect plot=NSMakeRect(40,12,self.bounds.size.width-50,36);
 double axisLow=0,axisHigh=fmax(30,ceil(maxFPS*1.10/30)*30);
 double axisMid=(axisLow+axisHigh)/2;

 NSColor *axisColor=[NSColor colorWithCalibratedWhite:0.75 alpha:0.65];
 NSColor *gridColor=[NSColor colorWithCalibratedWhite:0.75 alpha:0.18];
 NSBezierPath *grid=[NSBezierPath bezierPath];
 CGFloat midY=NSMinY(plot)+(axisMid-axisLow)/(axisHigh-axisLow)*plot.size.height;
 [grid moveToPoint:NSMakePoint(NSMinX(plot),midY)];
 [grid lineToPoint:NSMakePoint(NSMaxX(plot),midY)];
 [gridColor setStroke];
 grid.lineWidth=0.5;
 [grid stroke];

 NSBezierPath *axis=[NSBezierPath bezierPath];
 [axis moveToPoint:NSMakePoint(35,NSMinY(plot))];
 [axis lineToPoint:NSMakePoint(35,NSMaxY(plot))];
 NSDictionary *axisStyle=@{NSFontAttributeName:[NSFont monospacedSystemFontOfSize:8 weight:NSFontWeightRegular],
  NSForegroundColorAttributeName:axisColor};
 double axisLabels[]={axisLow,axisMid,axisHigh};
 for(int i=0;i<3;i++) {
  CGFloat y=NSMinY(plot)+(axisLabels[i]-axisLow)/(axisHigh-axisLow)*plot.size.height;
  [axis moveToPoint:NSMakePoint(35,y)];
  [axis lineToPoint:NSMakePoint(39,y)];
  [[NSString stringWithFormat:@"%.0f",axisLabels[i]] drawAtPoint:NSMakePoint(8,y-5) withAttributes:axisStyle];
 }
 [axisColor setStroke];
 axis.lineWidth=0.75;
 [axis stroke];

 NSBezierPath *line=[NSBezierPath bezierPath];
 NSPoint last=NSZeroPoint;
 NSUInteger points=0;
 for(NSUInteger i=0;i<_sampleCount;i++) {
  int fps=_samples[i];
  if(fps<0)continue;
  double xFraction=(double)i/(FPS_GRAPH_SAMPLES-1);
  double yFraction=fmax(0,fmin(1,(fps-axisLow)/(axisHigh-axisLow)));
  NSPoint point=NSMakePoint(NSMinX(plot)+xFraction*plot.size.width,
                            NSMinY(plot)+yFraction*plot.size.height);
  if(points)[line lineToPoint:point];else[line moveToPoint:point];
  last=point;
  points++;
 }

 NSColor *totk_colour=[NSColor colorWithCalibratedRed:0.541 green:0.969 blue:0.588 alpha:1];
 [totk_colour setStroke];
 [totk_colour setFill];
 line.lineWidth=1.5;
 [line stroke];
 if(points==1)[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(last.x-1,last.y-1,2,2)] fill];
}
@end
/* "hudshot <path>" in the input file (diagnostic): the corner box as it is drawn, as a PNG, and its text in the log. A
   window screenshot of the game picture (wshot) holds no AppKit view, so a test of what the box shows needs this. */
void runtime_front_hud_shot(const char *path) {
 NSString *p=[NSString stringWithUTF8String:path?path:""]?:@"";
 dispatch_async(dispatch_get_main_queue(),^{
  TOTKHudBox *box=(TOTKHudBox *)hudBox;
  if(!box||!p.length){fprintf(stderr,"[totk] hudshot: no corner box (TOTK_HUD=corners) or no path\n");return;}
  [box refresh];
  if(box.hidden){fprintf(stderr,"[totk] hudshot: the corner box is hidden (overlay off)\n");return;}
  NSBitmapImageRep *rep=[box bitmapImageRepForCachingDisplayInRect:box.bounds];[box cacheDisplayInRect:box.bounds toBitmapImageRep:rep];
  BOOL ok=[[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:p atomically:YES];
  NSFont *font=box.label.attributedStringValue.length?[box.label.attributedStringValue attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL]:nil;
  fprintf(stderr,"[totk] hudshot: %s %s; %.0f x %.0f points, font %.1f: %s\n",p.UTF8String,ok?"written":"FAILED",box.bounds.size.width,box.bounds.size.height,
          font.pointSize,[box.label.stringValue stringByReplacingOccurrencesOfString:@"\n" withString:@" | "].UTF8String);
 });
}
