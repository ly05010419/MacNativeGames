/* The settings panel's 更新 page (Mac; included by settings_panel.h; PR #1404). User 2026-10-31: show only that the
   version is the newest, otherwise each newer commit's message; a button updates for the player, and what cannot be
   updated says why and that the player updates the code and builds again by hand. Opening the page starts the launcher's
   check (app/auto_update.h, --update-check: the checkout that made the app is fetched, not changed); the Update button
   starts its install (--update-install: the game closes, the app is updated, the game opens again). Both run in a
   cut-off copy of the launcher (TOTK_UPDATE_LAUNCHER, posix_spawn: this process's address space is not copied); the
   page reads what they write, update/status.json in the game's data folder (the folder of TOTK_CONTROLS). An agent's
   test session (TOTK_SLOT_OWNER other than user) starts neither. A controller's L1 / R1 reach the page, A presses the
   button and up / down scroll the text. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

enum { UPDATE_TAB_MAX_BYTES = 64 * 1024, UPDATE_TAB_STRIP_H = 46, UPDATE_TAB_SCROLL = 48, UPDATE_TAB_RECHECK = 600 };

static NSString *update_tab_folder(void) {
 const char *controls=getenv("TOTK_CONTROLS");
 return controls&&*controls?[@(controls).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"update"]:nil;
}
static BOOL update_tab_held(void) { const char *owner=getenv("TOTK_SLOT_OWNER");return owner&&*owner&&strcmp(owner,"user"); }
/* The launcher with these arguments, cut off from the game, which goes on. A test passes its own (UPDATE_TAB_SPAWN). */
#ifndef UPDATE_TAB_SPAWN
static BOOL update_tab_spawn(const char *mode, const char *arg) {
 const char *launcher=getenv("TOTK_UPDATE_LAUNCHER");if(!launcher||!*launcher)return NO;
 char *const arguments[]={(char *)launcher,(char *)mode,(char *)arg,NULL};
 posix_spawn_file_actions_t actions;posix_spawnattr_t attributes;
 posix_spawn_file_actions_init(&actions);posix_spawnattr_init(&attributes);
 posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSID|POSIX_SPAWN_CLOEXEC_DEFAULT);
 for(int fd=0;fd<3;fd++)posix_spawn_file_actions_addopen(&actions,fd,"/dev/null",O_RDWR,0);
 extern char **environ;pid_t pid=0;
 BOOL ok=!posix_spawn(&pid,launcher,&actions,&attributes,arguments,environ);
 if(ok)dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND,0),^{while(waitpid(pid,NULL,0)<0&&errno==EINTR){}});
 posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attributes);
 return ok;
}
#define UPDATE_TAB_SPAWN update_tab_spawn
#endif
static NSData *update_tab_read(void) {
 NSString *path=[update_tab_folder() stringByAppendingPathComponent:@"status.json"];if(!path)return nil;
 NSDictionary *about=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
 if(![about.fileType isEqualToString:NSFileTypeRegular]||about.fileSize>UPDATE_TAB_MAX_BYTES)return nil;
 NSData *data=[NSData dataWithContentsOfFile:path];return data.length<=UPDATE_TAB_MAX_BYTES?data:nil;
}
static BOOL update_tab_number(id v) { return [v isKindOfClass:NSNumber.class]&&CFGetTypeID((__bridge CFTypeRef)v)!=CFBooleanGetTypeID(); }
/* The document, or nil for anything that is not what the launcher writes. */
static NSDictionary *update_tab_parse(NSData *data) {
 NSDictionary *d=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
 if(![d isKindOfClass:NSDictionary.class]||![d[@"version"] isEqual:@1]||!update_tab_number(d[@"at"]))return nil;
 if(![@[@"latest",@"behind",@"blocked",@"error",@"closing",@"updating",@"updated",@"failed"] containsObject:d[@"state"]])return nil;
 for(NSString *k in @[@"reason",@"detail",@"installed",@"from",@"to"])if(d[k]&&![d[k] isKindOfClass:NSString.class])return nil;
 if(d[@"commits"]){if(![d[@"commits"] isKindOfClass:NSArray.class])return nil;for(id v in d[@"commits"])if(![v isKindOfClass:NSString.class])return nil;}
 if(d[@"total"]&&!update_tab_number(d[@"total"]))return nil;
 return d;
}
/* Why an update cannot go in (auto_update.h's reasons). */
static NSString *update_tab_reason(NSString *reason) {
 if([reason isEqualToString:@"no_repo"])return settings_text("这个 App 不知道打包它的项目文件夹在哪（或文件夹不在了）","This app does not know where the checkout that made it is (or it is gone)");
 if([reason isEqualToString:@"no_access"])return settings_text("macOS 没有允许这个 App 访问项目文件夹（系统设置 › 隐私与安全性 › 文件与文件夹）","macOS did not let this app reach the checkout (System Settings › Privacy & Security › Files & Folders)");
 if([reason isEqualToString:@"git_failed"])return settings_text("git 没能读项目文件夹","git could not read the checkout");
 if([reason isEqualToString:@"fetch_failed"])return settings_text("连不上 GitHub","GitHub could not be reached");
 if([reason isEqualToString:@"no_upstream"])return settings_text("项目的 main 没有跟踪上游","The checkout's main tracks no upstream");
 if([reason isEqualToString:@"not_main"])return settings_text("项目文件夹不在 main 分支上","The checkout is not on main");
 if([reason isEqualToString:@"local_changes"])return settings_text("项目文件夹里有没提交的改动","The checkout has uncommitted changes");
 if([reason isEqualToString:@"unpushed"])return settings_text("项目文件夹里有上游没有的提交","The checkout has commits upstream does not");
 if([reason isEqualToString:@"update_failed"])return settings_text("更新 App 没有成功，App 保持原样（日志：update/update.log）","Updating the app did not work and it was left as it was (log: update/update.log)");
 return reason?:@"";
}
static NSString *update_tab_by_hand(void) {
 return settings_text("请自己更新代码再重新编译：在项目文件夹里运行 git pull，然后 python3.14 update_runtime.py \"<这个 App>\"（不行就用 build_app.py 重新打包）。",
  "Please update the code and build again yourself: git pull in your checkout, then python3.14 update_runtime.py \"<this app>\" (or build the app again with build_app.py).");
}

@interface TOTKUpdatePage:NSView
@property(strong) NSScrollView *scroll;
@property(strong) NSTextField *text, *status;
@property(strong) NSButton *updateButton;
@property(strong) NSDictionary *summary;   /* the document on screen; nil: none yet */
@property(strong) NSData *shownData;
@property double checkedAt;   /* when this run last asked for a check (0: never) */
@property BOOL shown, pressed;
-(void)show;
-(void)refresh;
-(void)scrollBy:(CGFloat)distance;
@end
@implementation TOTKUpdatePage
-(instancetype)initWithFrame:(NSRect)frame{
 if(!(self=[super initWithFrame:frame]))return nil;
 CGFloat w=frame.size.width,h=frame.size.height;
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,UPDATE_TAB_STRIP_H,w,fmax(0,h-UPDATE_TAB_STRIP_H))];
 scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;
 TOTKFlippedView *list=[[TOTKFlippedView alloc] initWithFrame:NSMakeRect(0,0,w,0)];
 NSTextField *text=[NSTextField wrappingLabelWithString:@""];text.selectable=NO;text.refusesFirstResponder=YES;[list addSubview:text];
 scroll.documentView=list;[self addSubview:scroll];self.scroll=scroll;self.text=text;
 NSTextField *status=[NSTextField labelWithString:@""];status.font=[NSFont systemFontOfSize:12 weight:NSFontWeightMedium];status.textColor=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1];
 status.lineBreakMode=NSLineBreakByTruncatingTail;status.refusesFirstResponder=YES;status.frame=NSMakeRect(18,6,fmax(0,w-200),17);[self addSubview:status];self.status=status;
 NSButton *button=[NSButton buttonWithTitle:@"" target:self action:@selector(updateClicked:)];button.refusesFirstResponder=YES;button.font=[NSFont systemFontOfSize:12];button.wantsLayer=YES;[self addSubview:button];self.updateButton=button;
 return self;
}
/* The page opened: a check, unless one of the last UPDATE_TAB_RECHECK seconds is here or an update is under way. */
-(void)show{
 double now=(double)time(NULL);NSString *state=update_tab_parse(update_tab_read())[@"state"];
 if(!update_tab_held()&&!self.pressed&&![@[@"closing",@"updating"] containsObject:state?:@""]&&(!self.checkedAt||now-self.checkedAt>=UPDATE_TAB_RECHECK)){
  NSString *folder=update_tab_folder();if(folder)[NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
  if(UPDATE_TAB_SPAWN("--update-check",NULL))self.checkedAt=now;
 }
 self.shown=NO;[self refresh];
}
-(void)refresh{
 NSData *data=update_tab_read();
 if(self.shown&&(data==self.shownData||[data isEqual:self.shownData]))return;   /* the scroll position stays */
 self.shown=YES;self.shownData=data;NSDictionary *d=self.summary=update_tab_parse(data);
 NSMutableAttributedString *all=[NSMutableAttributedString new];
 void (^add)(NSString *,NSFont *,NSColor *,CGFloat)=^(NSString *text,NSFont *font,NSColor *color,CGFloat indent){
  NSMutableParagraphStyle *style=[NSMutableParagraphStyle new];style.lineSpacing=2;style.paragraphSpacing=5;style.headIndent=indent;
  [all appendAttributedString:[[NSAttributedString alloc] initWithString:[(all.length?@"\n":@"") stringByAppendingString:text]
   attributes:@{NSFontAttributeName:font,NSForegroundColorAttributeName:color,NSParagraphStyleAttributeName:style}]];};
 NSFont *plain=[NSFont systemFontOfSize:12.5],*strong=[NSFont systemFontOfSize:13 weight:NSFontWeightSemibold],*small=[NSFont systemFontOfSize:11.5];
 NSColor *white=[NSColor colorWithCalibratedWhite:0.92 alpha:1],*dim=[NSColor colorWithCalibratedWhite:0.7 alpha:1],*accent=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1],
  *warn=[NSColor colorWithCalibratedRed:1 green:0.8 blue:0.55 alpha:1];
 const char *c=getenv("TOTK_SOURCE_COMMIT");NSString *running=c&&*c?@(c):nil,*state=d[@"state"];
 NSString *version=[settings_text("当前版本 ","Version ") stringByAppendingString:running?:settings_text("未知","unknown")];
 BOOL newest=[state isEqualToString:@"latest"]||([state isEqualToString:@"updated"]&&running&&[d[@"to"] hasPrefix:running]);
 if(newest)add([version stringByAppendingString:settings_text(" · 已是最新版"," · the newest")],strong,accent,0);
 else{
  add(version,strong,white,0);
  NSArray<NSString *> *commits=d[@"commits"];long long total=[d[@"total"] longLongValue];
  if(commits.count&&![state isEqualToString:@"updated"]){
   add(total>(long long)commits.count?[NSString stringWithFormat:@(TOTK_UI("最新版 %@ 多了 %lld 个提交（下面是最新的 %lu 个）：","The newest, %@, has %lld more commits (the newest %lu below):")),d[@"to"],total,(unsigned long)commits.count]:
    [NSString stringWithFormat:@(TOTK_UI("最新版 %@ 多了这些提交：","The newest, %@, adds:")),d[@"to"]],plain,white,0);
   for(NSString *title in commits)add([@"•  " stringByAppendingString:title],plain,white,12);
  }
  if([@[@"blocked",@"error",@"failed"] containsObject:state?:@""]){
   add([update_tab_reason(d[@"reason"]) stringByAppendingString:settings_text("，没法替你更新。",", so it cannot be updated for you.")],plain,warn,0);
   if([d[@"detail"] length])add(d[@"detail"],small,dim,0);
   add(update_tab_by_hand(),plain,warn,0);
  }
 }
 BOOL held=update_tab_held();
 self.status.stringValue=held?settings_text("测试运行：不检查也不更新","A test run: no check, no update"):
  [state isEqualToString:@"closing"]||self.pressed?settings_text("正在关闭游戏去更新…","Closing the game to update…"):
  [state isEqualToString:@"updating"]?settings_text("正在更新…","Updating…"):
  [state isEqualToString:@"updated"]&&!newest?[NSString stringWithFormat:@"%@ %@%@",settings_text("已更新到","Updated to"),d[@"to"],settings_text("，下次启动生效","; from the next start")]:
  !d&&self.checkedAt?settings_text("正在检查…","Checking…"):@"";
 self.updateButton.hidden=held||self.pressed||![state isEqualToString:@"behind"];
 self.updateButton.title=settings_text("更新（会关闭游戏，更新完自动重新打开）","Update (closes the game, opens it again when done)");
 CGFloat w=self.frame.size.width;[self.updateButton sizeToFit];CGFloat bw=self.updateButton.frame.size.width;self.updateButton.frame=NSMakeRect(w-14-bw,2,bw,26);
 self.text.attributedStringValue=all;
 CGFloat width=fmax(0,w-36),height=ceil([self.text.cell cellSizeForBounds:NSMakeRect(0,0,width,CGFLOAT_MAX)].height),room=self.scroll.contentSize.height;
 self.text.frame=NSMakeRect(18,2,width,height);self.scroll.documentView.frame=NSMakeRect(0,0,w,fmax(height+8,room));
 [self.scroll.documentView scrollPoint:NSZeroPoint];
}
-(void)updateClicked:(id)sender{(void)sender;
 if(self.pressed)return;   /* once: the panel drops the button from its targets at its next tick */
 char pid[24];snprintf(pid,sizeof pid,"%d",(int)getpid());
 if(UPDATE_TAB_SPAWN("--update-install",pid)){self.pressed=YES;self.shown=NO;[self refresh];}
 [self.window makeFirstResponder:self.window.contentView];   /* key 1 and Esc keep reaching the window */
}
-(void)scrollBy:(CGFloat)distance{
 NSClipView *clip=self.scroll.contentView;CGFloat most=fmax(0,self.scroll.documentView.frame.size.height-clip.bounds.size.height);
 [clip scrollToPoint:NSMakePoint(0,fmin(most,fmax(0,clip.bounds.origin.y+distance)))];[self.scroll reflectScrolledClipView:clip];
}
@end

@interface TOTKSettingsPanel (UpdateTab)
-(NSView *)updatePageWithFrame:(NSRect)frame;
-(void)showUpdatePage;
-(void)refreshUpdatePage;
-(void)scrollUpdatePage:(int)step;
@end
@implementation TOTKSettingsPanel (UpdateTab)
-(NSView *)updatePageWithFrame:(NSRect)frame{self.updatePageTargets=[NSMutableArray array];return [[TOTKUpdatePage alloc] initWithFrame:frame];}
/* selectPage: opens the page (a check); the panel's tick only redraws it from the document. */
-(void)updatePageTargetsFrom:(TOTKUpdatePage *)page{[self.updatePageTargets removeAllObjects];if(!page.updateButton.hidden)[self.updatePageTargets addObject:page.updateButton];}
-(void)showUpdatePage{TOTKUpdatePage *page=(TOTKUpdatePage *)self.pages[SETTINGS_PAGE_UPDATE];[page show];[self updatePageTargetsFrom:page];}
-(void)refreshUpdatePage{TOTKUpdatePage *page=(TOTKUpdatePage *)self.pages[SETTINGS_PAGE_UPDATE];[page refresh];[self updatePageTargetsFrom:page];}
-(void)scrollUpdatePage:(int)step{[(TOTKUpdatePage *)self.pages[SETTINGS_PAGE_UPDATE] scrollBy:step*UPDATE_TAB_SCROLL];}
@end
