#pragma once
extern void mtl_grab_next_present(void (^cb)(CGImageRef img)) __attribute__((weak_import));

enum { SHOT_FLASH_MS = 550, SHOT_TOAST_MS = 2500, SHOT_PENDING_MS = 3000, SHOT_PULSE_MS = 80 };
#define SHOT_PULSE 0.5f

@interface TOTKShotFlash:NSView
@end
@implementation TOTKShotFlash
-(instancetype)initWithFrame:(NSRect)frame{
 if((self=[super initWithFrame:frame])){self.wantsLayer=YES;self.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  self.layer.borderWidth=6;self.layer.borderColor=[NSColor colorWithCalibratedRed:0.96 green:0.18 blue:0.16 alpha:0.95].CGColor;}
 return self;}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
@end
@interface TOTKShotToast:NSView
@property(strong) NSTextField *label;
@property BOOL failed;
-(instancetype)initWithText:(NSString *)text failed:(BOOL)failed width:(CGFloat)width;
@end
@implementation TOTKShotToast
-(instancetype)initWithText:(NSString *)text failed:(BOOL)failed width:(CGFloat)width{
 NSTextField *label=[NSTextField labelWithString:text];
 label.font=[NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium];label.lineBreakMode=NSLineBreakByTruncatingMiddle;label.maximumNumberOfLines=1;
 label.textColor=failed?[NSColor colorWithCalibratedRed:1 green:0.45 blue:0.4 alpha:1]:[NSColor colorWithCalibratedRed:80/255.0 green:1 blue:120/255.0 alpha:1];
 NSSize z=[label sizeThatFits:NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX)];CGFloat w=fmin(ceil(z.width)+28,fmax(120,width-40)),h=ceil(z.height)+14;
 if(!(self=[super initWithFrame:NSMakeRect(floor((width-w)/2),24,w,h)]))return nil;
 self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0.04 alpha:0.78].CGColor;self.layer.cornerRadius=8;
 self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.14].CGColor;
 self.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMaxYMargin;
 label.frame=NSMakeRect(14,7,w-28,ceil(z.height));[self addSubview:label];self.label=label;self.failed=failed;
 return self;}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
@end

static TOTKShotToast *shotToast;
static uint64_t shotPendingSince;
static dispatch_queue_t shotQueue;

static NSString *screenshot_folder(void){
 const char *e=getenv("TOTK_SCREENSHOT_DIR");
 if(e&&*e)return [@(e) stringByExpandingTildeInPath];
 NSArray<NSString *> *desktop=NSSearchPathForDirectoriesInDomains(NSDesktopDirectory,NSUserDomainMask,YES);
 return desktop.count?desktop[0]:[NSHomeDirectory() stringByAppendingPathComponent:@"Desktop"];
}
static NSString *screenshot_path(NSString *folder,NSDate *at){
 static NSDateFormatter *f;
 if(!f){f=[NSDateFormatter new];f.locale=[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];f.dateFormat=@"yyyy-MM-dd 'at' HH.mm.ss";}
 NSString *stem=[NSString stringWithFormat:@"%s %@",GAME_NAME,[f stringFromDate:at]];
 for(unsigned n=1;n<1000;n++){
  NSString *name=n==1?[stem stringByAppendingString:@".png"]:[NSString stringWithFormat:@"%@ (%u).png",stem,n];
  NSString *path=[folder stringByAppendingPathComponent:name];
  if(![NSFileManager.defaultManager fileExistsAtPath:path])return path;
 }
 return nil;
}
static CGImageRef screenshot_copy(CGImageRef img){
 if(!img)return NULL;
 CGDataProviderRef from=CGImageGetDataProvider(img);
 CFDataRef data=from?CGDataProviderCopyData(from):NULL;
 CGDataProviderRef provider=data?CGDataProviderCreateWithCFData(data):NULL;
 CGImageRef copy=provider?CGImageCreate(CGImageGetWidth(img),CGImageGetHeight(img),CGImageGetBitsPerComponent(img),CGImageGetBitsPerPixel(img),
  CGImageGetBytesPerRow(img),CGImageGetColorSpace(img),CGImageGetBitmapInfo(img),provider,NULL,false,kCGRenderingIntentDefault):NULL;
 if(provider)CGDataProviderRelease(provider);
 if(data)CFRelease(data);
 return copy;
}
static NSString *screenshot_write(CGImageRef img,NSString *folder,NSDate *at,NSString **why){
 NSError *err=nil;
 if(!img){*why=@(TOTK_UI("游戏没给出画面","no picture came from the game"));return nil;}
 if(![NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:&err]){
  *why=[NSString stringWithFormat:@(TOTK_UI("建不了文件夹 %@（%@）","cannot make the folder %@ (%@)")),folder,err.localizedDescription?:@"?"];return nil;}
 NSString *path=screenshot_path(folder,at);
 if(!path){*why=@(TOTK_UI("这一秒的文件名都用完了","every name for this second is taken"));return nil;}
 NSBitmapImageRep *rep=[[NSBitmapImageRep alloc] initWithCGImage:img];
 NSData *png=rep?[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]:nil;
 if(!png){*why=@(TOTK_UI("画面编码不成 PNG","the picture could not be encoded as PNG"));return nil;}
 if(![png writeToFile:path options:NSDataWritingAtomic error:&err]){
  *why=[NSString stringWithFormat:@(TOTK_UI("写不进 %@（%@）","cannot write into %@ (%@)")),folder,err.localizedDescription?:@"?"];return nil;}
 return path;
}
static void screenshot_toast(NSString *text,BOOL failed){
 if(!window)return;
 [shotToast removeFromSuperview];
 NSView *cv=window.contentView;TOTKShotToast *t=[[TOTKShotToast alloc] initWithText:text failed:failed width:cv.bounds.size.width];
 [cv addSubview:t];shotToast=t;
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,SHOT_TOAST_MS*NSEC_PER_MSEC),dispatch_get_main_queue(),^{
  if(shotToast!=t)return;
  [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx){ctx.duration=0.4;t.animator.alphaValue=0;} completionHandler:nil];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,450*NSEC_PER_MSEC),dispatch_get_main_queue(),^{if(shotToast==t)shotToast=nil;[t removeFromSuperview];});});
}
static void screenshot_done(NSString *path,NSString *why){
 shotPendingSince=0;
 if(path){
  const char *e=getenv("TOTK_SCREENSHOT_DIR");
  screenshot_toast(e&&*e?[NSString stringWithFormat:@(TOTK_UI("截图已保存：%@","Screenshot saved: %@")),path]
   :[NSString stringWithFormat:@(TOTK_UI("截图已保存到桌面：%@","Screenshot saved to the Desktop: %@")),path.lastPathComponent],NO);
  fprintf(stderr,"[totk] screenshot saved: %s\n",path.UTF8String);
 }else{
  screenshot_toast([NSString stringWithFormat:@(TOTK_UI("截图没能保存：%@","Screenshot not saved: %@")),why?:@"?"],YES);
  fprintf(stderr,"[totk] screenshot not saved: %s\n",(why?:@"?").UTF8String);
 }
}
static void screenshot_flash(void){
 NSView *cv=window.contentView;TOTKShotFlash *flash=[[TOTKShotFlash alloc] initWithFrame:cv.bounds];[cv addSubview:flash];
 [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx){ctx.duration=SHOT_FLASH_MS/1000.0;flash.animator.alphaValue=0;} completionHandler:nil];
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(SHOT_FLASH_MS+50)*NSEC_PER_MSEC),dispatch_get_main_queue(),^{[flash removeFromSuperview];});
}
static void screenshot_capture(void){
 if(!window)return;
 uint64_t now=runtime_now_ns();
 if(shotPendingSince&&now-shotPendingSince<SHOT_PENDING_MS*1000000ull){fprintf(stderr,"[totk] screenshot: the last one is still on its way\n");return;}
 screenshot_flash();
 runtime_rumble_pulse(SHOT_PULSE,SHOT_PULSE_MS);
 if(!mtl_grab_next_present||(bootOverlay&&!bootOverlay.hidden)){screenshot_done(nil,@(TOTK_UI("游戏还没出画面","the game has not shown a picture yet")));return;}
 if(!shotQueue)shotQueue=dispatch_queue_create("totk.screenshot",DISPATCH_QUEUE_SERIAL);
 shotPendingSince=now;
 NSString *folder=screenshot_folder();NSDate *at=[NSDate date];
 mtl_grab_next_present(^(CGImageRef img){
  CGImageRef copy=screenshot_copy(img);
  dispatch_async(shotQueue,^{
   NSString *why=nil;NSString *path=screenshot_write(copy,folder,at,&why);
   if(copy)CGImageRelease(copy);
   dispatch_async(dispatch_get_main_queue(),^{screenshot_done(path,why);});
  });
 });
}
static void screenshot_request(void){dispatch_async(dispatch_get_main_queue(),^{screenshot_capture();});}
