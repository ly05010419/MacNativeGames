/* The settings panel's 着色器贡献 page (Mac; included by settings_panel.h; user 2026-11-10: a tab of its own that shows how
   many shaders the player has contributed and how many new ones were found to contribute, with one big switch in the
   middle). The switch is the start-up question's
   choice (totk_contrib_upload.h): on starts the uploads at once, off stops them (each round checks it). Contributed is the count the uploads keep (totk_inbox_sent); found is the
   rows still in the contribution file, read again only when the file changes (its size or time), so the panel's tick twice
   a second costs a stat. A run without a contribution file (a development run) shows why the switch is off. A
   controller's A flips the switch. */
#pragma once
#include <sys/stat.h>

/* The rows still to send; the file is read again only when its size or modification time changed. */
static NSUInteger contrib_tab_rows(NSString *path) {
 static NSString *seenPath;static off_t seenSize=-1;static struct timespec seenTime;static NSUInteger seenRows;
 struct stat st;
 if(!path||stat(path.fileSystemRepresentation,&st)){seenPath=nil;seenSize=-1;return 0;}
 if([seenPath isEqualToString:path]&&st.st_size==seenSize&&st.st_mtimespec.tv_sec==seenTime.tv_sec&&st.st_mtimespec.tv_nsec==seenTime.tv_nsec)return seenRows;
 seenRows=totk_contrib_read(path).count;seenPath=[path copy];seenSize=st.st_size;seenTime=st.st_mtimespec;
 return seenRows;
}

@interface TOTKContribPage:NSView
@property(copy) NSString *path;
@property(strong) NSTextField *sent, *found, *foundCaption, *state, *note;
@property(strong) NSSwitch *toggle;
-(void)refresh;
@end

@implementation TOTKContribPage
static NSTextField *contrib_tab_label(NSString *text, NSFont *font, CGFloat white) {
 NSTextField *l=[NSTextField labelWithString:text];l.font=font;l.alignment=NSTextAlignmentCenter;
 l.textColor=[NSColor colorWithCalibratedWhite:white alpha:1];l.lineBreakMode=NSLineBreakByWordWrapping;l.maximumNumberOfLines=0;return l;
}
static NSString *contrib_tab_number(NSUInteger n) {
 static NSNumberFormatter *f;if(!f){f=[NSNumberFormatter new];f.numberStyle=NSNumberFormatterDecimalStyle;
  f.locale=[NSLocale localeWithLocaleIdentifier:totk_ui_zh()?@"zh_CN":@"en_US"];}   /* 1,234 in the panel's language, not the Mac's region */
 return [f stringFromNumber:@(n)];
}
-(instancetype)initWithFrame:(NSRect)frame{
 if(!(self=[super initWithFrame:frame]))return nil;
 self.path=totk_contrib_path();
 CGFloat W=frame.size.width,H=frame.size.height,half=floor((W-40)/2);
 /* two numbers side by side (contributed so far, newly found), the switch, its title and state: one block, centred in
    the room above the note */
 NSSwitch *toggle=[NSSwitch new];[toggle sizeToFit];NSSize s=toggle.frame.size;CGFloat k=2;
 CGFloat noteTop=46,block=56+36+20+s.height*k+8+22+18,top=floor((noteTop+H-6+block)/2);
 NSFont *big=[NSFont monospacedDigitSystemFontOfSize:44 weight:NSFontWeightSemibold],*small=[NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
 NSTextField *sent=contrib_tab_label(@"0",big,1.0);sent.frame=NSMakeRect(20,top-56,half,56);[self addSubview:sent];self.sent=sent;
 NSTextField *sentCaption=contrib_tab_label(settings_text("已经贡献的着色器和管线","shaders and pipelines contributed"),small,0.8);
 sentCaption.frame=NSMakeRect(20,NSMinY(sent.frame)-36,half,34);[self addSubview:sentCaption];
 NSTextField *found=contrib_tab_label(@"0",big,1.0);found.frame=NSMakeRect(20+half,top-56,half,56);[self addSubview:found];self.found=found;
 NSTextField *foundCaption=contrib_tab_label(@"",small,0.8);
 foundCaption.frame=NSMakeRect(20+half,NSMinY(found.frame)-36,half,34);[self addSubview:foundCaption];self.foundCaption=foundCaption;
 NSBox *rule=[[NSBox alloc] initWithFrame:NSMakeRect(floor(W/2),NSMinY(sentCaption.frame)+4,1,56+32)];rule.boxType=NSBoxSeparator;[self addSubview:rule];
 /* the big switch in the middle: a system switch drawn at twice its size (the holder's bounds are half its frame, so
    drawing and clicks scale together) */
 NSView *holder=[[NSView alloc] initWithFrame:NSMakeRect(floor((W-s.width*k)/2),NSMinY(sentCaption.frame)-20-s.height*k,s.width*k,s.height*k)];
 [holder setBoundsSize:s];toggle.frame=NSMakeRect(0,0,s.width,s.height);
 toggle.target=self;toggle.action=@selector(toggled:);toggle.refusesFirstResponder=YES;
 [holder addSubview:toggle];[self addSubview:holder];self.toggle=toggle;
 NSTextField *title=contrib_tab_label(settings_text("自动贡献着色器","Contribute shaders automatically"),[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold],1.0);
 title.frame=NSMakeRect(20,NSMinY(holder.frame)-8-22,W-40,22);[self addSubview:title];
 NSTextField *state=contrib_tab_label(@"",[NSFont systemFontOfSize:12],0.7);
 state.frame=NSMakeRect(20,NSMinY(title.frame)-20,W-40,18);[self addSubview:state];self.state=state;
 /* what is sent, at the bottom */
 NSTextField *note=contrib_tab_label(settings_text("只发送着色器和管线的标识、游戏和 App 版本、设备型号和系统版本号，以及一个随机编号，不含游戏内容、存档或个人信息。",
  "Only shader and pipeline identifiers, the game and app versions, the device model and system version, and a random id are sent: no game data, saves or personal information."),
  [NSFont systemFontOfSize:11],0.6);
 note.frame=NSMakeRect(40,8,W-80,34);[self addSubview:note];self.note=note;
 [self refresh];
 return self;
}
-(BOOL)usable{return self.path&&totk_inbox_configured();}
-(void)refresh{
 BOOL usable=[self usable],on=usable&&[totk_inbox_choice(self.path) isEqualToString:@"auto"];
 NSUInteger rows=usable?contrib_tab_rows(self.path):0;
 self.sent.stringValue=usable?contrib_tab_number(totk_inbox_sent(self.path)):@"–";
 self.found.stringValue=usable?contrib_tab_number(rows):@"–";
 self.foundCaption.stringValue=!usable?settings_text("这次运行不记录着色器贡献","This run keeps no shader contribution")
  :rows?settings_text("又发现的新着色器和管线，可以贡献","newly found shaders and pipelines to contribute")
  :settings_text("暂时没有新发现","Nothing new found yet");
 self.toggle.enabled=usable;
 self.toggle.state=on?NSControlStateValueOn:NSControlStateValueOff;
 self.state.stringValue=!usable?@"":on?settings_text("已开启：游戏会在后台自动贡献，不打扰游玩","On: the game contributes them in the background, without interrupting play")
  :settings_text("已关闭：不会上传任何东西","Off: nothing is uploaded");
}
-(void)toggled:(NSSwitch *)toggle{
 if(![self usable]){[self refresh];return;}
 BOOL on=toggle.state==NSControlStateValueOn;
 totk_inbox_choose(self.path,on);if(on)totk_inbox_start(self.path,@"mac");
 fprintf(stderr,"[settings] contribute shaders automatically %s\n",on?"on":"off");
 [self refresh];
}
@end

@interface TOTKSettingsPanel (ContribTab)
-(NSView *)contribPageWithFrame:(NSRect)frame;
-(void)refreshContribPage;
-(NSArray<NSView *> *)contribPageTargets;
@end
@implementation TOTKSettingsPanel (ContribTab)
-(NSView *)contribPageWithFrame:(NSRect)frame{return [[TOTKContribPage alloc] initWithFrame:frame];}
-(void)refreshContribPage{[(TOTKContribPage *)self.pages[SETTINGS_PAGE_CONTRIB] refresh];}
/* what a controller reaches on the page: the switch (A flips it) */
-(NSArray<NSView *> *)contribPageTargets{TOTKContribPage *page=(TOTKContribPage *)self.pages[SETTINGS_PAGE_CONTRIB];return page.toggle.enabled?@[page.toggle]:@[];}
@end
