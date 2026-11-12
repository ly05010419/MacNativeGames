/* The settings panel's 着色器贡献 page (Mac; included by settings_panel.h; user 2026-11-10: a tab of its own for the
   player's contribution; 2026-11-11: laid out as the 性能排行榜 page, with the contributors' list and 查看排名).
   At the top the switch "自动贡献着色器" (the start-up question's choice, totk_contrib_upload.h: on starts the uploads at
   once, off stops them, each round checks it); under it the state and one line of counts:
   contributed (the count the uploads keep, totk_inbox_sent) and found (the rows still in the contribution file, read
   again only when the file changes, so the panel's tick twice a second costs a stat). Then the site's contributors for
   this game (GET /v1/stats, the list the site shows), read when the page is shown and again at most once a minute while
   it stays open; the player's own row (their pseudonym, worked out here from this file's install id) green and scrolled
   into view. The panel's 访问主页 (user 2026-11-11, settings_home_url) opens the site with the pseudonym, so the site
   marks the row too.
   A run without a contribution file (a development run) shows why the switch is off. A controller's A flips the switch. */
#pragma once
#include <sys/stat.h>
#include <CommonCrypto/CommonDigest.h>

#define CONTRIB_TAB_REFETCH_S 60.0
#define CONTRIB_TAB_TIMEOUT_S 15.0

/* The rows still to send; the file is read again only when its size or modification time changed. */
static NSUInteger contrib_tab_rows(NSString *path) {
 static NSString *seenPath;static off_t seenSize=-1;static struct timespec seenTime;static NSUInteger seenRows;
 struct stat st;
 if(!path||stat(path.fileSystemRepresentation,&st)){seenPath=nil;seenSize=-1;return 0;}
 if([seenPath isEqualToString:path]&&st.st_size==seenSize&&st.st_mtimespec.tv_sec==seenTime.tv_sec&&st.st_mtimespec.tv_nsec==seenTime.tv_nsec)return seenRows;
 seenRows=totk_contrib_read(path).count;seenPath=[path copy];seenSize=st.st_size;seenTime=st.st_mtimespec;
 return seenRows;
}

/* The player's pseudonym for this contribution file: the first 8 hex of SHA-256("player:" + its install id), as the server
   names a contributor (MacNativeGamesWeb lib/admin.js pseudonym); nil while it has no id (nothing sent yet). Read only:
   no id is made here. The pseudonym is public; the id, which counts the rows sent, never leaves the Mac this way. */
static NSString *contrib_tab_pseudonym(NSString *path) {
 NSString *install=path?[[NSString stringWithContentsOfFile:totk_contrib_side(path,@"install") encoding:NSUTF8StringEncoding error:nil]
  stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]:nil;
 if(install.length!=36)return nil;
 NSData *d=[[@"player:" stringByAppendingString:install] dataUsingEncoding:NSUTF8StringEncoding];unsigned char h[CC_SHA256_DIGEST_LENGTH];
 CC_SHA256(d.bytes,(CC_LONG)d.length,h);
 return [NSString stringWithFormat:@"%02x%02x%02x%02x",h[0],h[1],h[2],h[3]];
}
/* The site's root (TOTK_STATS_URL overrides it for a test: the root of a local inbox); nil without an inbox. */
static NSURLComponents *contrib_tab_root(void) {
 const char *e=getenv("TOTK_STATS_URL");
 if(!(e&&*e)&&!totk_inbox_configured())return nil;
 NSURLComponents *u=[NSURLComponents componentsWithString:e&&*e?@(e):@TOTK_CONTRIB_INBOX];
 if(!u.host)return nil;
 u.path=@"/";u.query=nil;return u;
}
/* The contributors for this game: GET /v1/stats?game= (the site's own list, most contributed first). */
static void contrib_tab_fetch(void (^done)(NSArray *players, NSString *error)) {
 NSURLComponents *u=contrib_tab_root();
 if(!u){done(nil,@"no inbox");return;}
 u.path=@"/v1/stats";u.queryItems=@[[NSURLQueryItem queryItemWithName:@"game" value:@GAME_INBOX_ID]];
 NSURLRequest *request=[NSURLRequest requestWithURL:u.URL cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:CONTRIB_TAB_TIMEOUT_S];
 [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
  NSInteger status=error?0:[(NSHTTPURLResponse *)response statusCode];
  id json=status==200&&data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
  id payload=[json isKindOfClass:NSDictionary.class]?json[@"data"]:nil;   /* the server's envelope: {"success": true, "data": {...}} */
  NSArray *players=[payload isKindOfClass:NSDictionary.class]&&[payload[@"players"] isKindOfClass:NSArray.class]?payload[@"players"]:nil;
  NSString *why=players?nil:error?error.localizedDescription:[NSString stringWithFormat:@"HTTP %ld",(long)status];
  if(why)fprintf(stderr,"[settings] the contributors were not read: %s\n",why.UTF8String);
  dispatch_async(dispatch_get_main_queue(),^{done(players,why);});
 }] resume];
}
@interface TOTKContribPage:NSView<NSTableViewDataSource,NSTableViewDelegate>
@property(copy) NSString *path;
@property(strong) NSTextField *counts, *state, *status, *note;
@property(strong) NSSwitch *toggle;
@property(strong) NSTableView *table;
@property(strong) NSArray<NSDictionary *> *players;
@property(copy) NSString *me;             /* the player's pseudonym, nil before the first upload */
@property double fetchedAt;
@property BOOL fetching, loaded;
-(void)refresh;
@end

@implementation TOTKContribPage
static NSTextField *contrib_tab_label(NSString *text, CGFloat size, CGFloat white, NSTextAlignment align) {
 NSTextField *l=[NSTextField labelWithString:text];l.font=[NSFont systemFontOfSize:size];l.alignment=align;
 l.textColor=[NSColor colorWithCalibratedWhite:white alpha:1];l.lineBreakMode=NSLineBreakByTruncatingTail;return l;
}
static NSString *contrib_tab_number(NSUInteger n) {
 static NSNumberFormatter *f;if(!f){f=[NSNumberFormatter new];f.numberStyle=NSNumberFormatterDecimalStyle;
  f.locale=[NSLocale localeWithLocaleIdentifier:totk_ui_zh()?@"zh_CN":@"en_US"];}   /* 1,234 in the panel's language, not the Mac's region */
 return [f stringFromNumber:@(n)];
}
/* The columns, as the site's table: identifier, title, width; the numbers right-aligned. */
static NSArray<NSArray *> *contrib_tab_columns(void) {
 return @[@[@"rank",settings_text("名次","#"),@40],@[@"player",settings_text("玩家","Player"),@96],@[@"shaders",settings_text("着色器","Shaders"),@80],
          @[@"pipelines",settings_text("管线","Pipelines"),@80],@[@"firsts",settings_text("首次","Firsts"),@70],@[@"uploads",settings_text("上传次数","Uploads"),@80]];
}

-(instancetype)initWithFrame:(NSRect)frame{
 if(!(self=[super initWithFrame:frame]))return nil;
 self.path=totk_contrib_path();self.me=contrib_tab_pseudonym(self.path);
 CGFloat W=frame.size.width,H=frame.size.height,top=H-12;
 /* the switch and its title; the state and the counts under them */
 NSSwitch *toggle=[NSSwitch new];[toggle sizeToFit];NSSize s=toggle.frame.size;
 toggle.frame=NSMakeRect(18,top-s.height,s.width,s.height);toggle.target=self;toggle.action=@selector(toggled:);toggle.refusesFirstResponder=YES;
 [self addSubview:toggle];self.toggle=toggle;
 NSTextField *title=contrib_tab_label(settings_text("自动贡献着色器","Contribute shaders automatically"),15,1.0,NSTextAlignmentLeft);
 title.font=[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
 title.frame=NSMakeRect(NSMaxX(toggle.frame)+10,top-s.height+1,W-18-NSMaxX(toggle.frame)-10,22);[self addSubview:title];
 NSTextField *state=contrib_tab_label(@"",11,0.7,NSTextAlignmentLeft);state.frame=NSMakeRect(18,top-s.height-20,W-36,16);[self addSubview:state];self.state=state;
 NSTextField *counts=contrib_tab_label(@"",12,0.92,NSTextAlignmentLeft);counts.frame=NSMakeRect(18,NSMinY(state.frame)-4-18,W-36,18);[self addSubview:counts];self.counts=counts;
 /* the contributors */
 CGFloat noteH=30,tableTop=NSMinY(counts.frame)-6,tableBottom=8+noteH+4;
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(14,tableBottom,W-28,tableTop-tableBottom)];
 scroll.hasVerticalScroller=YES;scroll.drawsBackground=NO;scroll.borderType=NSNoBorder;
 NSTableView *table=[[NSTableView alloc] initWithFrame:scroll.bounds];
 for(NSArray *c in contrib_tab_columns()){NSTableColumn *col=[[NSTableColumn alloc] initWithIdentifier:c[0]];col.title=c[1];col.width=[c[2] doubleValue];
  col.headerCell.alignment=[c[0] isEqualToString:@"player"]?NSTextAlignmentLeft:NSTextAlignmentRight;[table addTableColumn:col];}
 table.dataSource=self;table.delegate=self;table.rowHeight=20;table.intercellSpacing=NSMakeSize(2,2);
 table.style=NSTableViewStylePlain;table.backgroundColor=NSColor.clearColor;table.allowsColumnSelection=NO;
 table.columnAutoresizingStyle=NSTableViewNoColumnAutoresizing;table.refusesFirstResponder=YES;table.selectionHighlightStyle=NSTableViewSelectionHighlightStyleNone;
 scroll.documentView=table;[self addSubview:scroll];self.table=table;
 NSTextField *status=contrib_tab_label(@"",12,0.7,NSTextAlignmentCenter);status.frame=NSMakeRect(20,NSMidY(scroll.frame)-10,W-40,20);[self addSubview:status];self.status=status;
 /* what is sent, at the bottom */
 NSTextField *note=contrib_tab_label(settings_text("只发送着色器和管线的标识、游戏和 App 版本、设备型号和系统版本号，以及一个随机编号和排行榜用的随机编号（贡献榜上显示你的昵称），不含游戏内容、存档或个人信息。",
  "Only shader and pipeline identifiers, the game and app versions, the device model and system version, and a random id plus the board's (so the contributors show your nickname) are sent: no game data, saves or personal information."),
  10.5,0.6,NSTextAlignmentCenter);
 note.lineBreakMode=NSLineBreakByWordWrapping;note.maximumNumberOfLines=2;note.frame=NSMakeRect(30,8,W-60,noteH);[self addSubview:note];self.note=note;
 [self refresh];
 return self;
}
-(BOOL)usable{return self.path&&totk_inbox_configured();}
-(void)refresh{
 BOOL usable=[self usable],on=usable&&[totk_inbox_choice(self.path) isEqualToString:@"auto"];
 NSUInteger rows=usable?contrib_tab_rows(self.path):0;
 self.toggle.enabled=usable;
 self.toggle.state=on?NSControlStateValueOn:NSControlStateValueOff;
 self.state.stringValue=!usable?settings_text("这次运行不记录着色器贡献","This run keeps no shader contribution")
  :on?settings_text("已开启：游戏会在后台自动贡献，不打扰游玩","On: the game contributes them in the background, without interrupting play")
  :settings_text("已关闭：不会上传任何东西","Off: nothing is uploaded");
 self.counts.stringValue=!usable?@"":[NSString stringWithFormat:settings_text("已贡献 %@ 个着色器和管线 · %@","%@ shaders and pipelines contributed · %@"),
  contrib_tab_number(totk_inbox_sent(self.path)),rows?[NSString stringWithFormat:settings_text("又发现 %@ 个新的，可以贡献","%@ new ones found to contribute"),contrib_tab_number(rows)]
  :settings_text("暂时没有新发现","nothing new found yet")];
 NSString *me=contrib_tab_pseudonym(self.path);   /* the first upload makes the id */
 if(!(me==self.me||[me isEqualToString:self.me])){self.me=me;[self.table reloadData];}
 double now=CACurrentMediaTime();
 if(!self.fetching&&(!self.fetchedAt||now-self.fetchedAt>CONTRIB_TAB_REFETCH_S))[self fetch];
}
-(void)fetch{
 if(!contrib_tab_root()){self.status.stringValue=settings_text("这次运行不连接网站","This run does not reach the site");return;}
 self.fetching=YES;if(!self.loaded)self.status.stringValue=settings_text("正在读取贡献榜…","Reading the contributors…");
 __weak TOTKContribPage *weakSelf=self;
 contrib_tab_fetch(^(NSArray *players,NSString *error){
  TOTKContribPage *page=weakSelf;if(!page)return;
  page.fetching=NO;page.fetchedAt=CACurrentMediaTime();
  if(!players){if(!page.loaded)page.status.stringValue=[NSString stringWithFormat:settings_text("读不到贡献榜（%@），一分钟后再试","The contributors could not be read (%@); trying again in a minute"),error];return;}
  [page show:players];
 });
}
-(void)show:(NSArray *)players{
 NSMutableArray *rows=[NSMutableArray array];
 for(id p in players)if([p isKindOfClass:NSDictionary.class]&&[p[@"player"] isKindOfClass:NSString.class])[rows addObject:p];
 self.loaded=YES;self.players=rows;[self.table reloadData];
 self.status.stringValue=rows.count?@"":settings_text("还没有人贡献","No one has contributed yet");
 for(NSUInteger k=0;k<rows.count;k++)if(self.me&&[rows[k][@"player"] isEqualToString:self.me]){[self.table scrollRowToVisible:(NSInteger)k];break;}
}
-(void)toggled:(NSSwitch *)toggle{
 if(![self usable]){[self refresh];return;}
 BOOL on=toggle.state==NSControlStateValueOn;
 totk_inbox_choose(self.path,on);if(on)totk_inbox_start(self.path,@"mac");
 fprintf(stderr,"[settings] contribute shaders automatically %s\n",on?"on":"off");
 [self refresh];
}

-(NSInteger)numberOfRowsInTableView:(NSTableView *)t{(void)t;return (NSInteger)self.players.count;}
-(NSString *)cellText:(NSDictionary *)p column:(NSString *)c row:(NSInteger)row{
 if([c isEqualToString:@"rank"])return [NSString stringWithFormat:@"%ld",(long)row+1];
 if([c isEqualToString:@"player"])return [p[@"name"] isKindOfClass:NSString.class]?p[@"name"]:p[@"player"];   /* the nickname, else the pseudonym */
 return [p[c] isKindOfClass:NSNumber.class]?contrib_tab_number([p[c] unsignedIntegerValue]):@"–";
}
-(NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row{
 NSTextField *cell=[t makeViewWithIdentifier:col.identifier owner:self];
 if(!cell){cell=[NSTextField labelWithString:@""];cell.identifier=col.identifier;cell.lineBreakMode=NSLineBreakByTruncatingTail;}
 NSDictionary *p=self.players[(NSUInteger)row];BOOL me=self.me&&[p[@"player"] isEqualToString:self.me];
 cell.stringValue=[self cellText:p column:col.identifier row:row];
 cell.alignment=col.headerCell.alignment;
 cell.font=me?[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightBold]:[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
 if([col.identifier isEqualToString:@"player"]&&![p[@"nick"] boolValue])cell.font=[NSFont monospacedSystemFontOfSize:12 weight:me?NSFontWeightBold:NSFontWeightRegular];
 if([col.identifier isEqualToString:@"player"])cell.toolTip=cell.stringValue;   /* a long nickname whole */
 cell.textColor=me?NSColor.systemGreenColor:[NSColor colorWithCalibratedWhite:0.92 alpha:1];
 return cell;
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
