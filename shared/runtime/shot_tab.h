/* The settings panel's 截图分享 page (Mac; included by settings_panel.h; user 2026-11-11: a tab of its own; the switch
   "分享我的截图" off by default, never asked or announced; with it on, every screenshot the capture button saves is shared,
   shot_upload.h; anonymous; the player can take back their own). The game's shared pictures, newest first, four to a row:
   the thumbnail (aspect-fit in a 16:9 box), the anonymous name (the board's) and the day; the player's own have a green
   border and a Delete button. Read when the page is shown, again at most once a minute while it stays open, and at once
   after the switch changes or a picture is deleted; scrolling to the end reads the older ones (the server's "next"). A
   click on a thumbnail (A on a controller) shows the whole picture over the page; a click, Esc, or B or A on a controller
   closes it. A controller's A flips the switch and presses Delete.
   The 截屏 button at the top right (user 2026-11-11: "截图分享里 加一个按钮 截屏 ... 不包括设置面板") takes a screenshot the
   way the capture button does (screenshot.h screenshot_request): saved, and shared while the switch is on. The picture is
   the renderer's next game frame, copied from the game's own drawable; the panel is AppKit views over the game's layer and
   is never in it. The game keeps presenting while the panel is open (settings_show only takes the input), so the panel
   stays open and the picture comes with the next frame. A controller reaches the button after the switch.
   Below the state line, the player's nickname and its 修改 button (nick_row.h, the same row as on the 排行榜 page; the
   pictures carry the nickname instead of the anonymous code): the state line says how a change went, and once the server
   took one the list is read again. When one of the player's own pictures has just been shared (shot_upload.h
   TOTK_SHOT_SHARED_NOTE), the list is read at once too, not at the next minute.
   Under the nickname, where the capture button's pictures are saved and its 更改文件夹… (user 2026-11-11: moved here from
   the input page; settings_choose_screenshot_folder in settings_panel.h checks and keeps the choice). The site's photo
   wall is the panel's 访问主页, opened on it from this page (settings_home_url). */
#pragma once

#define SHOT_TAB_REFETCH_S 60.0
#define SHOT_TAB_COLUMNS 4
#define SHOT_TAB_GAP 10.0     /* between the cells */
#define SHOT_TAB_PAD 5.0      /* inside a cell, around the thumbnail and its text (the green border sits in it) */
#define SHOT_TAB_TEXT_H 32.0  /* the name and the day under a thumbnail */
#define SHOT_TAB_FLASH_S 5.0  /* a failed delete, in the state line */

@class TOTKShotsPage;

/* What the 截屏 button calls: the capture button's request (frontend.m includes screenshot.h before this header). A probe
   points it at its own. */
static void (*shot_tab_take)(void) = screenshot_request;

/* One picture of the grid. Also a controller target: isEnabled and performClick: are what the panel asks of one, and
   not being a button it is scrolled into view when a controller reaches it. */
@interface TOTKShotCell:NSView
@property(copy) NSString *shotId;
@property BOOL mine;
@property(strong) NSImageView *thumb;
@property(strong) NSTextField *missing;
@property(strong) NSButton *deleteButton;
@property(weak) TOTKShotsPage *owner;
@property(getter=isEnabled) BOOL enabled;
+(CGFloat)heightForWidth:(CGFloat)w;
@end

/* The whole picture over the page. */
@interface TOTKShotViewer:NSView
@property(strong) NSImageView *image;
@property(strong) NSTextField *status;
@property(strong) id keyMonitor;
@property(weak) TOTKShotsPage *owner;
@end

/* The scroll view's document: flipped, so the newest stay at the top and a longer list grows downwards. */
@interface TOTKShotsGrid:NSView
@end
@implementation TOTKShotsGrid
-(BOOL)isFlipped{return YES;}
@end

@interface TOTKShotsPage:NSView
@property(strong) NSSwitch *toggle;
@property(strong) NSButton *takeButton;              /* 截屏 */
@property(strong) NSTextField *state, *status, *note;
@property(strong) TOTKNickRow *nick;
@property(strong) NSTextField *folderLabel;           /* 截图保存目录 */
@property(strong) NSButton *folderButton;             /* 更改文件夹… */
@property(strong) NSScrollView *scroll;
@property(strong) TOTKShotsGrid *grid;
@property(strong) NSArray<NSDictionary *> *shots;   /* {id, name, at, w, h, me}, newest first */
@property(strong) NSArray<TOTKShotCell *> *cells;
@property(copy) NSString *next;                      /* the server's "next": older pictures wait there */
@property(copy) NSString *error;                     /* the last read's failure */
@property(copy) NSString *flash;
@property double flashUntil;
@property double fetchedAt, moreAfter;
@property BOOL fetching, fetchingMore, loaded, dropTail;
@property NSUInteger generation;                     /* the switch or a delete: answers to older reads are dropped */
@property(strong) TOTKShotViewer *viewer;
@property(copy) void (^targetsChanged)(void);        /* the panel's controller targets follow the grid */
-(void)refresh;
-(void)show:(NSString *)shotId;
-(void)remove:(TOTKShotCell *)cell;
-(BOOL)closeViewer;
-(NSArray<NSView *> *)targets;
@end

static NSTextField *shot_tab_label(NSString *text, CGFloat size, CGFloat white, NSTextAlignment align) {
 NSTextField *l=[NSTextField labelWithString:text];l.font=[NSFont systemFontOfSize:size];l.alignment=align;
 l.textColor=[NSColor colorWithCalibratedWhite:white alpha:1];l.lineBreakMode=NSLineBreakByTruncatingTail;return l;
}
/* The server's time ("2026-11-11T09:30:12.345Z") as the player's day, yyyy-MM-dd; its first ten characters when it is
   not one. */
static NSString *shot_tab_day(id at) {
 if(![at isKindOfClass:NSString.class])return @"–";
 static NSISO8601DateFormatter *precise,*plain;static NSDateFormatter *day;static dispatch_once_t once;
 dispatch_once(&once,^{precise=[NSISO8601DateFormatter new];precise.formatOptions=NSISO8601DateFormatWithInternetDateTime|NSISO8601DateFormatWithFractionalSeconds;
  plain=[NSISO8601DateFormatter new];day=[NSDateFormatter new];day.locale=[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];day.dateFormat=@"yyyy-MM-dd";});
 NSDate *d=[precise dateFromString:at]?:[plain dateFromString:at];
 return d?[day stringFromDate:d]:[(NSString *)at length]>=10?[(NSString *)at substringToIndex:10]:at;
}
/* The pictures of a list answer that have an id (the server's are digits, as strings), in its order. */
static NSArray<NSDictionary *> *shot_tab_items(id list) {
 NSMutableArray *items=[NSMutableArray array];
 if([list isKindOfClass:NSArray.class])for(id s in list)if([s isKindOfClass:NSDictionary.class]&&[s[@"id"] isKindOfClass:NSString.class]&&[s[@"id"] length])[items addObject:s];
 return items;
}
static NSString *shot_tab_next(id next) { return [next isKindOfClass:NSString.class]&&[next length]?next:nil; }

@implementation TOTKShotCell
+(CGFloat)heightForWidth:(CGFloat)w{return SHOT_TAB_PAD+floor((w-2*SHOT_TAB_PAD)*9/16)+SHOT_TAB_TEXT_H+SHOT_TAB_PAD;}
-(instancetype)initWithShot:(NSDictionary *)shot width:(CGFloat)w owner:(TOTKShotsPage *)owner{
 CGFloat h=[TOTKShotCell heightForWidth:w],iw=w-2*SHOT_TAB_PAD,ih=floor(iw*9/16);
 if(!(self=[super initWithFrame:NSMakeRect(0,0,w,h)]))return nil;
 self.shotId=shot[@"id"];self.mine=[shot[@"me"] boolValue];self.owner=owner;self.enabled=YES;self.wantsLayer=YES;
 self.toolTip=settings_text("点击看大图","Click to see the whole picture");
 if(self.mine){   /* the player's own: a green border, inside the controller's highlight (the cell's own layer border) */
  NSView *mark=[[NSView alloc] initWithFrame:NSInsetRect(self.bounds,2,2)];mark.wantsLayer=YES;
  mark.layer.borderWidth=2;mark.layer.cornerRadius=5;mark.layer.borderColor=NSColor.systemGreenColor.CGColor;[self addSubview:mark];
 }
 NSView *box=[[NSView alloc] initWithFrame:NSMakeRect(SHOT_TAB_PAD,h-SHOT_TAB_PAD-ih,iw,ih)];box.wantsLayer=YES;
 box.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.35].CGColor;box.layer.cornerRadius=3;[self addSubview:box];
 NSImageView *thumb=[[NSImageView alloc] initWithFrame:box.bounds];thumb.imageScaling=NSImageScaleProportionallyUpOrDown;
 thumb.imageFrameStyle=NSImageFrameNone;thumb.editable=NO;[box addSubview:thumb];self.thumb=thumb;
 NSTextField *missing=shot_tab_label(settings_text("读不到","Not read"),10,0.5,NSTextAlignmentCenter);
 missing.frame=NSMakeRect(0,floor((ih-14)/2),iw,14);missing.hidden=YES;[box addSubview:missing];self.missing=missing;
 NSTextField *name=shot_tab_label([shot[@"name"] isKindOfClass:NSString.class]?shot[@"name"]:@"–",11,0.92,NSTextAlignmentLeft);   /* the nickname, else the anonymous name the server makes from the id */
 name.toolTip=name.stringValue;
 name.font=self.mine?[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightBold]:[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
 if(self.mine)name.textColor=NSColor.systemGreenColor;
 name.frame=NSMakeRect(SHOT_TAB_PAD,SHOT_TAB_PAD+15,iw,15);[self addSubview:name];
 NSTextField *day=shot_tab_label(shot_tab_day(shot[@"at"]),10,0.65,NSTextAlignmentLeft);
 day.font=[NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightRegular];day.frame=NSMakeRect(SHOT_TAB_PAD,SHOT_TAB_PAD,iw,14);[self addSubview:day];
 if(self.mine){
  NSButton *del=[NSButton buttonWithTitle:settings_text("删除","Delete") target:self action:@selector(deleteShot:)];
  del.controlSize=NSControlSizeMini;del.font=[NSFont systemFontOfSize:10];del.refusesFirstResponder=YES;[del sizeToFit];
  del.frame=NSMakeRect(w-SHOT_TAB_PAD-del.frame.size.width,SHOT_TAB_PAD-2,del.frame.size.width,del.frame.size.height);
  [self addSubview:del];self.deleteButton=del;day.frame=NSMakeRect(SHOT_TAB_PAD,SHOT_TAB_PAD,iw-del.frame.size.width-4,14);
 }
 __weak TOTKShotCell *weakSelf=self;
 totk_shot_image(self.shotId,YES,^(NSImage *img){
  TOTKShotCell *cell=weakSelf;if(!cell)return;
  cell.thumb.image=img;cell.missing.hidden=img!=nil;
 });
 return self;
}
/* A click anywhere on the cell but its Delete button shows the picture. */
-(NSView *)hitTest:(NSPoint)p{NSView *v=[super hitTest:p];return !v||v==self.deleteButton?v:self;}
-(void)mouseDown:(NSEvent *)e{(void)e;}
-(void)mouseUp:(NSEvent *)e{if(NSPointInRect([self convertPoint:e.locationInWindow fromView:nil],self.bounds))[self.owner show:self.shotId];}
-(void)performClick:(id)sender{(void)sender;[self.owner show:self.shotId];}   /* a controller's A */
-(void)deleteShot:(NSButton *)button{(void)button;[self.owner remove:self];}
@end

@implementation TOTKShotViewer
-(instancetype)initWithFrame:(NSRect)frame shot:(NSString *)shotId owner:(TOTKShotsPage *)owner{
 if(!(self=[super initWithFrame:frame]))return nil;
 self.owner=owner;self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0 alpha:0.92].CGColor;
 NSImageView *image=[[NSImageView alloc] initWithFrame:NSInsetRect(self.bounds,8,8)];image.imageScaling=NSImageScaleProportionallyUpOrDown;
 image.imageFrameStyle=NSImageFrameNone;image.editable=NO;[self addSubview:image];self.image=image;
 NSTextField *status=shot_tab_label(settings_text("正在读取截图…","Reading the picture…"),12,0.7,NSTextAlignmentCenter);
 status.frame=NSMakeRect(20,NSMidY(self.bounds)-10,frame.size.width-40,20);[self addSubview:status];self.status=status;
 self.toolTip=settings_text("点击或按 Esc 关闭","Click or press Esc to close");
 __weak TOTKShotViewer *weakSelf=self;
 totk_shot_image(shotId,NO,^(NSImage *img){
  TOTKShotViewer *viewer=weakSelf;if(!viewer)return;
  viewer.image.image=img;viewer.status.hidden=img!=nil;
  if(!img)viewer.status.stringValue=settings_text("读不到这张截图","The picture could not be read");
 });
 return self;
}
-(NSView *)hitTest:(NSPoint)p{return [super hitTest:p]?self:nil;}   /* the image view takes no click: the whole cover closes */
-(void)mouseDown:(NSEvent *)e{(void)e;[self.owner closeViewer];}
/* Esc closes the picture, not the panel: the panel's own Esc is the game view's key handling, after this monitor. */
-(void)viewDidMoveToWindow{
 [super viewDidMoveToWindow];
 if(self.window&&!self.keyMonitor){
  __weak TOTKShotViewer *weakSelf=self;
  self.keyMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *e){
   TOTKShotViewer *viewer=weakSelf;
   if(!viewer||e.keyCode!=53||e.window!=viewer.window||viewer.hiddenOrHasHiddenAncestor)return e;
   dispatch_async(dispatch_get_main_queue(),^{[viewer.owner closeViewer];});   /* not inside the monitor it removes */
   return nil;
  }];
 }else if(!self.window&&self.keyMonitor){[NSEvent removeMonitor:self.keyMonitor];self.keyMonitor=nil;}
}
@end

@implementation TOTKShotsPage
-(instancetype)initWithFrame:(NSRect)frame{
 if(!(self=[super initWithFrame:frame]))return nil;
 CGFloat W=frame.size.width,H=frame.size.height,top=H-12;
 self.shots=@[];self.cells=@[];
 /* the switch, its title and the state line, as on the 排行榜 page */
 NSSwitch *toggle=[NSSwitch new];[toggle sizeToFit];NSSize s=toggle.frame.size;
 toggle.frame=NSMakeRect(18,top-s.height,s.width,s.height);toggle.target=self;toggle.action=@selector(toggled:);toggle.refusesFirstResponder=YES;
 [self addSubview:toggle];self.toggle=toggle;
 NSTextField *title=shot_tab_label(settings_text("分享我的截图","Share my screenshots"),15,1.0,NSTextAlignmentLeft);title.font=[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
 title.frame=NSMakeRect(NSMaxX(toggle.frame)+10,top-s.height+1,300,22);[self addSubview:title];
 NSTextField *state=shot_tab_label(@"",11,0.7,NSTextAlignmentLeft);state.frame=NSMakeRect(18,top-s.height-20,W-36,16);[self addSubview:state];self.state=state;
 /* 截屏, at the right of the switch's line */
 NSButton *take=[NSButton buttonWithTitle:settings_text("截屏","Take screenshot") target:self action:@selector(takeShot:)];
 take.refusesFirstResponder=YES;[take sizeToFit];NSSize b=take.frame.size;
 take.frame=NSMakeRect(W-18-b.width,top-s.height+floor((s.height-b.height)/2),b.width,b.height);
 take.toolTip=settings_text("保存现在的游戏画面（不含设置面板）；分享开着时也分享","Saves the game picture now, without this panel; shared too while sharing is on");
 [self addSubview:take];self.takeButton=take;
 title.frame=NSMakeRect(NSMaxX(toggle.frame)+10,top-s.height+1,fmin(300,NSMinX(take.frame)-NSMaxX(toggle.frame)-20),22);
 /* the nickname (nick_row.h) */
 TOTKNickRow *nick=[[TOTKNickRow alloc] initWithFrame:NSMakeRect(18,NSMinY(state.frame)-4-NICK_ROW_H,W-36,NICK_ROW_H)];[self addSubview:nick];self.nick=nick;
 [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(nickChanged:) name:TOTK_NICK_CHANGED_NOTE object:nil];
 [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(shared:) name:TOTK_SHOT_SHARED_NOTE object:nil];
 /* where the pictures are saved, and the button that changes it */
 NSButton *folderButton=[NSButton buttonWithTitle:settings_text("更改文件夹…","Change Folder…") target:self action:@selector(changeFolder:)];
 folderButton.refusesFirstResponder=YES;[folderButton sizeToFit];NSSize fb=folderButton.frame.size;
 CGFloat folderY=NSMinY(nick.frame)-6-fb.height;
 folderButton.frame=NSMakeRect(W-18-fb.width,folderY,fb.width,fb.height);[self addSubview:folderButton];self.folderButton=folderButton;
 NSTextField *folderLabel=shot_tab_label(@"",12,0.85,NSTextAlignmentLeft);folderLabel.lineBreakMode=NSLineBreakByTruncatingMiddle;
 folderLabel.frame=NSMakeRect(18,folderY+floor((fb.height-17)/2),NSMinX(folderButton.frame)-18-10,17);[self addSubview:folderLabel];self.folderLabel=folderLabel;
 [self showFolder];
 /* the pictures */
 CGFloat noteH=30,gridTop=folderY-6,gridBottom=8+noteH+4;
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(14,gridBottom,W-28,fmax(0,gridTop-gridBottom))];
 scroll.hasVerticalScroller=YES;scroll.autohidesScrollers=YES;scroll.scrollerStyle=NSScrollerStyleOverlay;scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;
 TOTKShotsGrid *grid=[[TOTKShotsGrid alloc] initWithFrame:NSMakeRect(0,0,scroll.contentSize.width,scroll.contentSize.height)];
 scroll.documentView=grid;[self addSubview:scroll];self.scroll=scroll;self.grid=grid;
 scroll.contentView.postsBoundsChangedNotifications=YES;
 [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(scrolled:) name:NSViewBoundsDidChangeNotification object:scroll.contentView];
 NSTextField *status=shot_tab_label(@"",12,0.7,NSTextAlignmentCenter);status.frame=NSMakeRect(20,NSMidY(scroll.frame)-10,W-40,20);[self addSubview:status];self.status=status;
 NSTextField *note=shot_tab_label(settings_text("匿名：只上传游戏画面（缩到 1920 宽的 JPEG）和一个随机编号算出的匿名名字，不含用户名、存档和机器信息。自己的截图可以删除。",
  "Anonymous: only the game picture (a JPEG at most 1920 wide) and a random id, shown as an anonymous name, are sent; no user name, saves or machine details. You can delete your own."),
  10.5,0.6,NSTextAlignmentCenter);
 note.lineBreakMode=NSLineBreakByWordWrapping;note.maximumNumberOfLines=2;note.frame=NSMakeRect(30,8,W-60,noteH);[self addSubview:note];self.note=note;
 [self showState];[self showStatus];   /* nothing is read until the page is shown (refresh) */
 return self;
}
-(void)dealloc{[NSNotificationCenter.defaultCenter removeObserver:self];}

-(BOOL)usable{return totk_board_dir()!=nil;}
-(void)showFolder{
 char now[PATH_MAX];screenshot_directory(now,sizeof now);
 self.folderLabel.stringValue=[NSString stringWithFormat:settings_text("截图保存目录：%@","Screenshot folder: %@"),@(now)];self.folderLabel.toolTip=@(now);
}
-(void)changeFolder:(NSButton *)button{(void)button;settings_choose_screenshot_folder();[self showFolder];}
-(void)showState{
 BOOL usable=[self usable],on=usable&&totk_shot_on();double now=CACurrentMediaTime();
 self.toggle.enabled=usable;self.toggle.state=on?NSControlStateValueOn:NSControlStateValueOff;
 BOOL flash=self.flash&&now<self.flashUntil;int kind=0;NSString *nickFlash=flash?nil:nick_row_flash_text(&kind);   /* a nickname change */
 self.state.stringValue=flash?self.flash:nickFlash?nickFlash:!usable?settings_text("这次运行不分享截图","This run shares no screenshots")
  :on?[NSString stringWithFormat:settings_text("已开启：每张截图自动分享，每天最多 %d 张","On: every screenshot is shared, up to %d a day"),TOTK_SHOT_PENDING_MAX]
  :settings_text("已关闭：不上传任何东西","Off: nothing is sent");
 self.state.textColor=flash?NSColor.systemOrangeColor:nickFlash?nick_row_flash_color(kind):[NSColor colorWithCalibratedWhite:0.7 alpha:1];
}
-(void)showStatus{
 self.status.stringValue=self.shots.count?@""
  :self.error?[NSString stringWithFormat:settings_text("读不到截图（%@），一分钟后再试","The screenshots could not be read (%@); trying again in a minute"),self.error]
  :!self.loaded?settings_text("正在读取截图…","Reading the screenshots…")
  :settings_text("还没有人分享截图","No one has shared a screenshot yet");
}

/* The switch and its state (twice a second with the panel's tick, while the page is shown); the newest pictures again
   when the list is a minute old; the older ones when the end is in view. */
-(void)refresh{
 [self showState];[self.nick refresh];[self showFolder];
 double now=CACurrentMediaTime();
 if(!self.fetching&&(!self.fetchedAt||now-self.fetchedAt>SHOT_TAB_REFETCH_S))[self fetch];
 else [self moreIfAtEnd];
}
/* Answers to reads made before now are dropped; the next refresh reads the newest again. */
-(void)reset:(BOOL)dropTail{self.generation++;self.fetching=self.fetchingMore=NO;self.fetchedAt=self.moreAfter=0;self.dropTail=dropTail;}
-(void)fetch{
 self.fetching=YES;NSUInteger generation=self.generation;
 __weak TOTKShotsPage *weakSelf=self;
 totk_shot_fetch(nil,^(NSDictionary *data,NSString *error){
  TOTKShotsPage *page=weakSelf;if(!page||generation!=page.generation)return;
  page.fetching=NO;page.fetchedAt=CACurrentMediaTime();page.error=error;
  if(data)[page takeNewest:data];else [page showStatus];
 });
}
/* The newest pictures: what was read further down stays below them (it holds the ones older than this answer's last),
   unless the switch changed (the player's own are marked again from the top). */
-(void)takeNewest:(NSDictionary *)data{
 NSMutableArray *all=[shot_tab_items(data[@"shots"]) mutableCopy];NSString *next=shot_tab_next(data[@"next"]);
 BOOL kept=NO;
 if(next&&!self.dropTail){long long last=next.longLongValue;
  for(NSDictionary *s in self.shots)if([s[@"id"] longLongValue]<last){[all addObject:s];kept=YES;}}
 if(!kept)self.next=next;   /* else the tail's own "next" still holds */
 self.dropTail=NO;self.loaded=YES;
 [self showShots:all];
}
-(void)moreIfAtEnd{
 if(!self.next||self.fetching||self.fetchingMore||CACurrentMediaTime()<self.moreAfter)return;
 CGFloat cellH=[TOTKShotCell heightForWidth:[self cellWidth]];
 if(NSMaxY(self.scroll.contentView.bounds)<NSHeight(self.grid.frame)-cellH)return;   /* the last row not reached */
 self.fetchingMore=YES;NSUInteger generation=self.generation;NSString *before=self.next;
 __weak TOTKShotsPage *weakSelf=self;
 totk_shot_fetch(before,^(NSDictionary *data,NSString *error){
  (void)error;TOTKShotsPage *page=weakSelf;if(!page||generation!=page.generation)return;
  page.fetchingMore=NO;
  if(!data){page.moreAfter=CACurrentMediaTime()+SHOT_TAB_REFETCH_S;return;}   /* the error is in the log */
  if(![page.next isEqualToString:before])return;   /* the newest were read again meanwhile */
  NSMutableSet *have=[NSMutableSet set];for(NSDictionary *s in page.shots)[have addObject:s[@"id"]];
  NSMutableArray *all=[page.shots mutableCopy];
  for(NSDictionary *s in shot_tab_items(data[@"shots"]))if(![have containsObject:s[@"id"]])[all addObject:s];
  page.next=shot_tab_next(data[@"next"]);[page showShots:all];
 });
}
-(void)scrolled:(NSNotification *)n{(void)n;if(!self.hiddenOrHasHiddenAncestor)[self moreIfAtEnd];}

-(CGFloat)cellWidth{return floor((self.scroll.contentSize.width-(SHOT_TAB_COLUMNS-1)*SHOT_TAB_GAP)/SHOT_TAB_COLUMNS);}
/* The grid made again only when the pictures or their marks changed; a cell that stays keeps its thumbnail. */
-(void)showShots:(NSArray<NSDictionary *> *)all{
 BOOL same=all.count==self.shots.count;
 for(NSUInteger i=0;same&&i<all.count;i++)same=[all[i][@"id"] isEqual:self.shots[i][@"id"]]&&[all[i][@"me"] boolValue]==[self.shots[i][@"me"] boolValue];
 self.shots=all;
 if(!same){
  NSMutableDictionary<NSString *,TOTKShotCell *> *old=[NSMutableDictionary dictionary];for(TOTKShotCell *c in self.cells)old[c.shotId]=c;
  CGFloat w=[self cellWidth],h=[TOTKShotCell heightForWidth:w];
  NSMutableArray<TOTKShotCell *> *cells=[NSMutableArray array];
  for(NSDictionary *s in all){TOTKShotCell *c=old[s[@"id"]];
   if(c&&c.mine==[s[@"me"] boolValue])[old removeObjectForKey:s[@"id"]];else c=[[TOTKShotCell alloc] initWithShot:s width:w owner:self];
   [cells addObject:c];}
  for(TOTKShotCell *c in old.allValues)[c removeFromSuperview];
  NSUInteger rows=(cells.count+SHOT_TAB_COLUMNS-1)/SHOT_TAB_COLUMNS;
  CGFloat docH=fmax(self.scroll.contentSize.height,rows?rows*(h+SHOT_TAB_GAP)-SHOT_TAB_GAP:0);
  self.grid.frame=NSMakeRect(0,0,self.scroll.contentSize.width,docH);
  for(NSUInteger i=0;i<cells.count;i++){
   cells[i].frame=NSMakeRect((i%SHOT_TAB_COLUMNS)*(w+SHOT_TAB_GAP),(i/SHOT_TAB_COLUMNS)*(h+SHOT_TAB_GAP),w,h);
   if(cells[i].superview!=self.grid)[self.grid addSubview:cells[i]];
  }
  self.cells=cells;
  if(self.targetsChanged)self.targetsChanged();
 }
 [self showStatus];[self moreIfAtEnd];
}

/* A nickname change: the state line now; once the server took it, the list again (its pictures carry the names). */
-(void)nickChanged:(NSNotification *)n{
 if([n.userInfo[@"listed"] boolValue])[self reset:NO];
 if(!self.hiddenOrHasHiddenAncestor)[self refresh];else [self showState];
}
/* One of the player's own pictures was just shared: the list again at once while the page is shown (else when it is). */
-(void)shared:(NSNotification *)n{(void)n;[self reset:NO];if(!self.hiddenOrHasHiddenAncestor)[self refresh];}
-(void)takeShot:(NSButton *)button{(void)button;fprintf(stderr,"[totk] settings: 截屏\n");if(shot_tab_take)shot_tab_take();}
-(void)toggled:(NSSwitch *)toggle{
 if(![self usable]){[self showState];return;}
 if(totk_shot_set(toggle.state==NSControlStateValueOn))[self reset:YES];   /* read again: the player's own marked, or not */
 [self refresh];
}
-(void)remove:(TOTKShotCell *)cell{
 cell.deleteButton.enabled=NO;NSString *shotId=cell.shotId;
 __weak TOTKShotsPage *weakSelf=self;
 totk_shot_delete(shotId,^(BOOL ok){
  TOTKShotsPage *page=weakSelf;if(!page)return;
  if(ok){NSMutableArray *rest=[NSMutableArray array];for(NSDictionary *s in page.shots)if(![s[@"id"] isEqual:shotId])[rest addObject:s];
   if(page.viewer)[page closeViewer];[page showShots:rest];}
  else{cell.deleteButton.enabled=YES;page.flash=settings_text("没有删掉，稍后再试","Not deleted; try again later");page.flashUntil=CACurrentMediaTime()+SHOT_TAB_FLASH_S;}
  [page reset:NO];if(!page.hiddenOrHasHiddenAncestor)[page refresh];
 });
}

-(void)show:(NSString *)shotId{
 [self closeViewer];
 TOTKShotViewer *viewer=[[TOTKShotViewer alloc] initWithFrame:self.bounds shot:shotId owner:self];
 [self addSubview:viewer];self.viewer=viewer;
}
-(BOOL)closeViewer{if(!self.viewer)return NO;[self.viewer removeFromSuperview];self.viewer=nil;return YES;}

/* What a controller reaches: the switch, 截屏, the nickname's 修改, 更改文件夹…, then each picture and, after the player's own, its
   Delete button (a run without the launcher's folder has no switch and no 修改). */
-(NSArray<NSView *> *)targets{
 NSMutableArray<NSView *> *targets=[NSMutableArray array];
 if(self.toggle.enabled)[targets addObject:self.toggle];
 [targets addObject:self.takeButton];
 if(self.toggle.enabled)[targets addObject:self.nick.button];
 [targets addObject:self.folderButton];
 for(TOTKShotCell *c in self.cells){[targets addObject:c];if(c.deleteButton)[targets addObject:c.deleteButton];}
 return targets;
}
@end

/* The panel's updateTargets (settings_panel.h), called when the grid changes while the page is shown. */
@interface TOTKSettingsPanel (ShotsTabTargets)
-(void)updateTargets;
@end
@interface TOTKSettingsPanel (ShotsTab)
-(NSView *)shotsPageWithFrame:(NSRect)frame;
-(void)refreshShotsPage;
-(NSArray<NSView *> *)shotsPageTargets;
-(BOOL)shotsPageCloseViewer;
@end
@implementation TOTKSettingsPanel (ShotsTab)
-(NSView *)shotsPageWithFrame:(NSRect)frame{
 TOTKShotsPage *page=[[TOTKShotsPage alloc] initWithFrame:frame];
 __weak TOTKSettingsPanel *weakSelf=self;
 page.targetsChanged=^{TOTKSettingsPanel *panel=weakSelf;if(panel&&panel.page==SETTINGS_PAGE_SHOTS)[panel updateTargets];};
 return page;
}
-(void)refreshShotsPage{[(TOTKShotsPage *)self.pages[SETTINGS_PAGE_SHOTS] refresh];}
-(NSArray<NSView *> *)shotsPageTargets{return [(TOTKShotsPage *)self.pages[SETTINGS_PAGE_SHOTS] targets];}
/* B or A while the whole picture is shown closes it (YES), not the panel. */
-(BOOL)shotsPageCloseViewer{return [(TOTKShotsPage *)self.pages[SETTINGS_PAGE_SHOTS] closeViewer];}
@end
