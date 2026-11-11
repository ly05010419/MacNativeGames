/* The settings panel's 排行榜 page (Mac; included by settings_panel.h; user 2026-11-11: a tab of its own, apart from the
   shaders; the switch "我要上榜" off by default, never asked or announced; the tab shows the latest board; with the
   switch on the player's own place is green; anonymous). The board comes from the server (board_upload.h
   totk_board_fetch), read when the page is shown and again at most once a minute while it stays open. One group of the
   board at a time: players are only ranked against the same frame lock and interpolation (the pop-up picks the group;
   it starts on the player's current one). A controller's A flips the switch. */
#pragma once

#define BOARD_TAB_REFETCH_S 60.0

@interface TOTKBoardPage:NSView<NSTableViewDataSource,NSTableViewDelegate>
@property(strong) NSSwitch *toggle;
@property(strong) NSTextField *state, *status, *note;
@property(strong) NSPopUpButton *groups;
@property(strong) NSTableView *table;
@property(strong) NSArray<NSDictionary *> *groupList;
@property(strong) NSArray<NSDictionary *> *entries;
@property double fetchedAt;
@property BOOL fetching, pickedByHand;
-(void)refresh;
@end

@implementation TOTKBoardPage
static NSTextField *board_tab_label(NSString *text, CGFloat size, CGFloat white, NSTextAlignment align) {
 NSTextField *l=[NSTextField labelWithString:text];l.font=[NSFont systemFontOfSize:size];l.alignment=align;
 l.textColor=[NSColor colorWithCalibratedWhite:white alpha:1];l.lineBreakMode=NSLineBreakByTruncatingTail;return l;
}
/* "不锁帧", "锁 60 帧", "插帧 30+30", "插帧（不锁）" */
static NSString *board_tab_group_name(NSDictionary *g) {
 int lock=[g[@"lock"] intValue],interp=[g[@"interp"] intValue];
 NSString *name=interp==1?settings_text("插帧（不锁）","Interpolated (uncapped)")
  :interp?[NSString stringWithFormat:settings_text("插帧 %d+%d","Interpolated %d+%d"),interp,interp]
  :lock?[NSString stringWithFormat:settings_text("锁 %d 帧","Locked at %d"),lock]:settings_text("不锁帧","Uncapped");
 return [NSString stringWithFormat:settings_text("%@ · %d 人","%@ · %d players"),name,[g[@"players"] intValue]];
}
static NSString *board_tab_fps(id v) { return [v isKindOfClass:NSNumber.class]?[NSString stringWithFormat:@"%.1f",[v doubleValue]]:@"–"; }

/* The columns: identifier, title, width (with the 2 pt spacing they fit the table beside its scroller). */
static NSArray<NSArray *> *board_tab_columns(void) {
 return @[@[@"rank",settings_text("名次","#"),@38],@[@"chip",settings_text("芯片","Chip"),@114],@[@"memory",settings_text("内存","Memory"),@50],
          @[@"out",settings_text("分辨率","Resolution"),@92],@[@"avg",settings_text("平均","Avg"),@46],@[@"median",settings_text("中位数","Median"),@50],
          @[@"low1",settings_text("1%最低","1% low"),@52],@[@"max",settings_text("最高","Max"),@44],@[@"interp",settings_text("插帧","Interp."),@42]];
}

-(instancetype)initWithFrame:(NSRect)frame{
 if(!(self=[super initWithFrame:frame]))return nil;
 CGFloat W=frame.size.width,H=frame.size.height,top=H-12;
 /* the switch, its title and state on the left; the group on the right */
 NSSwitch *toggle=[NSSwitch new];[toggle sizeToFit];NSSize s=toggle.frame.size;
 toggle.frame=NSMakeRect(18,top-s.height,s.width,s.height);toggle.target=self;toggle.action=@selector(toggled:);toggle.refusesFirstResponder=YES;
 [self addSubview:toggle];self.toggle=toggle;
 NSTextField *title=board_tab_label(settings_text("我要上榜","Join the board"),15,1.0,NSTextAlignmentLeft);title.font=[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
 title.frame=NSMakeRect(NSMaxX(toggle.frame)+10,top-s.height+1,120,22);[self addSubview:title];
 NSPopUpButton *groups=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(W-18-210,top-26,210,26) pullsDown:NO];
 groups.target=self;groups.action=@selector(groupPicked:);groups.refusesFirstResponder=YES;[self addSubview:groups];self.groups=groups;
 NSTextField *state=board_tab_label(@"",11,0.7,NSTextAlignmentLeft);state.frame=NSMakeRect(18,top-s.height-20,W-36-220,16);[self addSubview:state];self.state=state;
 /* the board */
 CGFloat noteH=30,tableTop=NSMinY(state.frame)-8,tableBottom=8+noteH+4;
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(14,tableBottom,W-28,tableTop-tableBottom)];
 scroll.hasVerticalScroller=YES;scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;
 NSTableView *table=[[NSTableView alloc] initWithFrame:scroll.bounds];
 for(NSArray *c in board_tab_columns()){NSTableColumn *col=[[NSTableColumn alloc] initWithIdentifier:c[0]];col.title=c[1];col.width=[c[2] doubleValue];
  col.headerCell.alignment=[c[0] isEqualToString:@"chip"]||[c[0] isEqualToString:@"out"]?NSTextAlignmentLeft:NSTextAlignmentRight;[table addTableColumn:col];}
 table.dataSource=self;table.delegate=self;table.rowHeight=20;table.intercellSpacing=NSMakeSize(2,2);
 table.style=NSTableViewStylePlain;   /* the default (inset) style shrinks the first column to 10 pt: the 名次 title was cut */table.backgroundColor=NSColor.clearColor;table.allowsColumnSelection=NO;
 table.columnAutoresizingStyle=NSTableViewNoColumnAutoresizing;table.refusesFirstResponder=YES;table.selectionHighlightStyle=NSTableViewSelectionHighlightStyleNone;
 scroll.documentView=table;[self addSubview:scroll];self.table=table;
 NSTextField *status=board_tab_label(@"",12,0.7,NSTextAlignmentCenter);status.frame=NSMakeRect(20,NSMidY(scroll.frame)-10,W-40,20);[self addSubview:status];self.status=status;
 NSTextField *note=board_tab_label(settings_text("匿名：只上传配置、画面设置、帧数和一个随机编号。帧数只算进入世界后的游玩，暂停和切到后台不算；同一锁帧和插帧设置的玩家放在一组比较。",
  "Anonymous: only the hardware, picture settings, frame rates and a random id are sent. Only play in the world counts, not pauses or time in the background; players are ranked against the same frame lock and interpolation."),
  10.5,0.6,NSTextAlignmentCenter);
 note.lineBreakMode=NSLineBreakByWordWrapping;note.maximumNumberOfLines=2;note.frame=NSMakeRect(30,8,W-60,noteH);[self addSubview:note];self.note=note;
 [self refresh];
 return self;
}

-(BOOL)usable{return totk_board_dir()!=nil;}

/* The switch and its state (twice a second with the panel's tick); the board again when it is a minute old. */
-(void)refresh{
 BOOL usable=[self usable],on=usable&&totk_board_on();
 self.toggle.enabled=usable;self.toggle.state=on?NSControlStateValueOn:NSControlStateValueOff;
 self.state.stringValue=!usable?settings_text("这次运行不参加排行榜","This run takes no part in the board")
  :on?[NSString stringWithFormat:settings_text("已开启：玩满 %d 分钟后，每 5 分钟上传一次这局的成绩","On: after %d minutes of play, this session's numbers are sent every 5 minutes"),TOTK_BOARD_MIN_SECONDS/60]
  :settings_text("已关闭：不上传任何东西","Off: nothing is sent");
 double now=CACurrentMediaTime();
 if(!self.fetching&&(!self.fetchedAt||now-self.fetchedAt>BOARD_TAB_REFETCH_S))[self fetch];
}
-(void)fetch{
 self.fetching=YES;if(!self.groupList)self.status.stringValue=settings_text("正在读取榜单…","Reading the board…");
 __weak TOTKBoardPage *weakSelf=self;
 totk_board_fetch(^(NSDictionary *board,NSString *error){
  TOTKBoardPage *page=weakSelf;if(!page)return;
  page.fetching=NO;page.fetchedAt=CACurrentMediaTime();
  if(!board){if(!page.groupList)page.status.stringValue=[NSString stringWithFormat:settings_text("读不到榜单（%@），一分钟后再试","The board could not be read (%@); trying again in a minute"),error];return;}
  [page show:board[@"groups"]];
 });
}
/* The group shown: the one picked by hand, else the player's current pacing, else the biggest. */
-(void)show:(NSArray *)groups{
 NSMutableArray<NSDictionary *> *list=[NSMutableArray array];
 for(id g in groups)if([g isKindOfClass:NSDictionary.class]&&[g[@"entries"] isKindOfClass:NSArray.class])[list addObject:g];
 NSInteger keep=self.pickedByHand?self.groups.indexOfSelectedItem:-1;NSDictionary *was=keep>=0&&keep<(NSInteger)self.groupList.count?self.groupList[keep]:nil;
 self.groupList=list;[self.groups removeAllItems];
 for(NSDictionary *g in list)[self.groups addItemWithTitle:board_tab_group_name(g)];
 NSInteger pick=list.count?0:-1;int lock=0,interp=0;nvn_board_pacing(&lock,&interp);
 for(NSUInteger i=0;i<list.count;i++){
  BOOL same=was?[list[i][@"lock"] isEqual:was[@"lock"]]&&[list[i][@"interp"] isEqual:was[@"interp"]]:[list[i][@"lock"] intValue]==lock&&[list[i][@"interp"] intValue]==interp;
  if(same){pick=(NSInteger)i;break;}
 }
 if(pick>=0)[self.groups selectItemAtIndex:pick];
 self.groups.enabled=list.count>1;
 [self showGroup:pick];
}
-(void)showGroup:(NSInteger)i{
 NSMutableArray *rows=[NSMutableArray array];
 if(i>=0&&i<(NSInteger)self.groupList.count)for(id e in self.groupList[i][@"entries"])if([e isKindOfClass:NSDictionary.class])[rows addObject:e];
 self.entries=rows;[self.table reloadData];
 self.status.stringValue=rows.count?@"":settings_text("还没有人上榜","No one is on the board yet");
 for(NSUInteger k=0;k<rows.count;k++)if([rows[k][@"me"] boolValue]){[self.table scrollRowToVisible:(NSInteger)k];break;}
}
-(void)groupPicked:(NSPopUpButton *)p{self.pickedByHand=YES;[self showGroup:p.indexOfSelectedItem];}
-(void)toggled:(NSSwitch *)toggle{
 if(![self usable]){[self refresh];return;}
 if(totk_board_set(toggle.state==NSControlStateValueOn))self.fetchedAt=0;   /* the board again: with or without the player's row */
 [self refresh];
}

-(NSInteger)numberOfRowsInTableView:(NSTableView *)t{(void)t;return (NSInteger)self.entries.count;}
-(NSString *)cellText:(NSDictionary *)e column:(NSString *)c{
 if([c isEqualToString:@"rank"])return [NSString stringWithFormat:@"%@",e[@"rank"]?:@"–"];
 if([c isEqualToString:@"chip"]){NSString *chip=[e[@"chip"] isKindOfClass:NSString.class]?e[@"chip"]:([e[@"device"] isKindOfClass:NSString.class]?e[@"device"]:@"?");
  if([chip hasPrefix:@"Apple "])chip=[chip substringFromIndex:6];   /* "M4 Max": every chip here is Apple's */
  return [e[@"gpu_cores"] isKindOfClass:NSNumber.class]?[NSString stringWithFormat:settings_text("%@ · %@ 核","%@ · %@ GPU"),chip,e[@"gpu_cores"]]:chip;}
 if([c isEqualToString:@"memory"])return [e[@"memory_gb"] isKindOfClass:NSNumber.class]?[NSString stringWithFormat:@"%@ GB",e[@"memory_gb"]]:@"–";
 if([c isEqualToString:@"out"]){NSString *out=[e[@"out"] isKindOfClass:NSString.class]?[e[@"out"] stringByReplacingOccurrencesOfString:@"x" withString:@"×"]:@"–";
  return [e[@"metalfx"] boolValue]?[out stringByAppendingString:@" MFX"]:out;}
 if([c isEqualToString:@"interp"])return [e[@"interp_fps"] doubleValue]>0?[NSString stringWithFormat:@"+%.0f",[e[@"interp_fps"] doubleValue]]:@"–";
 return board_tab_fps(e[c]);
}
-(NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row{
 NSTextField *cell=[t makeViewWithIdentifier:col.identifier owner:self];
 if(!cell){cell=[NSTextField labelWithString:@""];cell.identifier=col.identifier;cell.lineBreakMode=NSLineBreakByTruncatingTail;}
 NSDictionary *e=self.entries[(NSUInteger)row];BOOL me=[e[@"me"] boolValue];
 cell.stringValue=[self cellText:e column:col.identifier];
 cell.alignment=col.headerCell.alignment;
 cell.font=me?[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightBold]:[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
 cell.textColor=me?NSColor.systemGreenColor:[NSColor colorWithCalibratedWhite:0.92 alpha:1];
 cell.toolTip=[col.identifier isEqualToString:@"out"]&&[e[@"scene"] isKindOfClass:NSString.class]
  ?[NSString stringWithFormat:settings_text("内部分辨率 %@","Internal resolution %@"),e[@"scene"]]:nil;
 return cell;
}
@end

@interface TOTKSettingsPanel (BoardTab)
-(NSView *)boardPageWithFrame:(NSRect)frame;
-(void)refreshBoardPage;
-(NSArray<NSView *> *)boardPageTargets;
@end
@implementation TOTKSettingsPanel (BoardTab)
-(NSView *)boardPageWithFrame:(NSRect)frame{return [[TOTKBoardPage alloc] initWithFrame:frame];}
-(void)refreshBoardPage{[(TOTKBoardPage *)self.pages[SETTINGS_PAGE_BOARD] refresh];}
/* what a controller reaches on the page: the switch (A flips it) */
-(NSArray<NSView *> *)boardPageTargets{TOTKBoardPage *page=(TOTKBoardPage *)self.pages[SETTINGS_PAGE_BOARD];return page.toggle.enabled?@[page.toggle]:@[];}
@end
