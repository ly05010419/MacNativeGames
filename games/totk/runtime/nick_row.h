/* The player's nickname on the 性能排行榜 and 截图分享 pages (Mac; included by settings_panel.h before board_tab.h and
   shot_tab.h; user 2026-11-11: one optional nickname per player, shown instead of the anonymous code on the board and
   the shared screenshots, unique on the site, changeable at any time; setting one turns on no upload switch).
   One row on each page, the same: 「昵称：<name>」 or 「昵称：未设置（显示为匿名代号）」 and a 修改 button (a click, or A on
   a controller). 修改 opens the game's text bar (runtime_front_text_input, text_keyboard.h: the Mac keyboard and the
   controller type into it, as for a new user on the 用户 page; the panel closes while it is up and comes back on the
   same page), filled with the nickname; confirming sends it (board_upload.h totk_name_set), and an empty one clears it.
   The page's state line says what happens: saving, then the new name, or in orange why the server refused it.
   Both rows show one nickname kept here, since the panel is made again at every opening: the first row shown in an
   opening reads it, 修改 sets it, and every row follows TOTK_NICK_CHANGED_NOTE. A change the server took also asks the
   pages to read their lists again (the note's "listed"), so the new name shows there at once. */
#pragma once

#define NICK_ROW_H 22.0
#define NICK_ROW_FLASH_S 6.0      /* the result in the state line */
#define NICK_ROW_SENDING_S 60.0   /* "saving…" until the answer, at most this long */
#define NICK_ROW_MAX 16           /* the server's limit (lib/name-validate.js NAME_LENGTH) */
/* The text bar's keys left out: @ % / \ (text_keyboard.h's disabled bits); the server refuses them anyway. */
#define NICK_ROW_DISABLED ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5))
#define TOTK_NICK_CHANGED_NOTE @"TOTKNickChanged"
enum { NICK_ROW_FLASH_PLAIN, NICK_ROW_FLASH_DONE, NICK_ROW_FLASH_REFUSED };

static void settings_show(int open);   /* settings_panel.h */

static NSString *nick_row_name;               /* the nickname as last read or set; nil: none */
static NSString *nick_row_error;              /* the last read's failure */
static BOOL nick_row_known, nick_row_fetching;
static BOOL nick_row_busy;                    /* the text bar is up, or a change is on its way */
static NSUInteger nick_row_generation;        /* a change: an answer to a read made before it is dropped */
static double nick_row_fetched_at;
static NSString *nick_row_flash;
static int nick_row_flash_kind;
static double nick_row_flash_until;

/* What the state line shows about the nickname now (nil: nothing); *kind: NICK_ROW_FLASH_*. */
static NSString *nick_row_flash_text(int *kind) {
 if(!nick_row_flash||CACurrentMediaTime()>=nick_row_flash_until)return nil;
 if(kind)*kind=nick_row_flash_kind;
 return nick_row_flash;
}
static NSColor *nick_row_flash_color(int kind) {
 return kind==NICK_ROW_FLASH_REFUSED?NSColor.systemOrangeColor:kind==NICK_ROW_FLASH_DONE?NSColor.systemGreenColor:[NSColor colorWithCalibratedWhite:0.7 alpha:1];
}
static void nick_row_show(NSString *text, int kind, double seconds) {
 nick_row_flash=text;nick_row_flash_kind=kind;nick_row_flash_until=CACurrentMediaTime()+seconds;
}
static void nick_row_note(BOOL listed) {
 [NSNotificationCenter.defaultCenter postNotificationName:TOTK_NICK_CHANGED_NOTE object:nil userInfo:@{@"listed":@(listed)}];
}

static void nick_row_fetch(void) {
 if(nick_row_fetching)return;
 nick_row_fetching=YES;NSUInteger generation=nick_row_generation;
 totk_name_fetch(^(NSString *name,NSString *error){
  nick_row_fetching=NO;nick_row_fetched_at=CACurrentMediaTime();
  if(generation!=nick_row_generation)return;   /* changed meanwhile: the change's answer holds */
  if(!error){nick_row_name=name;nick_row_known=YES;}   /* a failed read keeps what was known */
  nick_row_error=error;
  nick_row_note(NO);
 });
}

/* What the text bar answered (main queue): nil when it was cancelled. The panel comes back; the change goes. */
static void nick_row_typed(NSString *typed) {
 NSString *name=[typed stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
 if(!typed||(nick_row_known&&[name isEqualToString:nick_row_name?:@""])){   /* cancelled or unchanged: nothing sent (each try counts toward the server's hourly limit) */
  nick_row_busy=NO;settings_show(1);nick_row_note(NO);return;
 }
 nick_row_generation++;
 nick_row_show(name.length?settings_text("正在保存昵称…","Saving the nickname…"):settings_text("正在清除昵称…","Clearing the nickname…"),NICK_ROW_FLASH_PLAIN,NICK_ROW_SENDING_S);
 settings_show(1);nick_row_note(NO);
 fprintf(stderr,"[totk] settings: nickname %s\n",name.length?"sent":"cleared");
 totk_name_set(name,^(NSString *stored,NSString *error,NSInteger status){
  nick_row_busy=NO;
  if(!error){
   nick_row_name=stored;nick_row_known=YES;nick_row_error=nil;nick_row_fetched_at=CACurrentMediaTime();
   nick_row_show(stored?[NSString stringWithFormat:settings_text("昵称已改为“%@”","Nickname set to \"%@\""),stored]
                       :settings_text("昵称已清除，显示为匿名代号","Nickname cleared: the anonymous code shows"),NICK_ROW_FLASH_DONE,NICK_ROW_FLASH_S);
  }else nick_row_show([NSString stringWithFormat:settings_text("昵称没有改成：%@","Nickname not changed: %@"),totk_name_problem(status,error)],NICK_ROW_FLASH_REFUSED,NICK_ROW_FLASH_S);
  nick_row_note(!error);   /* taken: the pages read their lists again */
 });
}

/* 修改: the text bar, filled with the nickname (on a background queue: it waits for the player). */
static void nick_row_change(void) {
 if(nick_row_busy||!totk_board_dir())return;
 nick_row_busy=YES;nick_row_note(NO);
 NSString *start=nick_row_known&&nick_row_name?nick_row_name:@"";
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  char first[128];snprintf(first,sizeof first,"%s",start.UTF8String?start.UTF8String:"");
  RuntimeTextRequest req={TOTK_UI("昵称（2 到 16 个字，留空就清除；回车确认，Esc 取消）","Nickname (2 to 16 characters, empty clears it; Return confirms, Escape cancels)"),
                          first,NICK_ROW_MAX,0,0,0,NICK_ROW_DISABLED};
  char typed[256];int named=runtime_front_text_input(&req,typed,sizeof typed);
  NSString *text=named?([NSString stringWithUTF8String:typed]?:@""):nil;
  dispatch_async(dispatch_get_main_queue(),^{nick_row_typed(text);});
 });
}

@interface TOTKNickRow:NSView
@property(strong) NSTextField *label;
@property(strong) NSButton *button;   /* 修改 */
@property double madeAt;
-(void)refresh;
@end
@implementation TOTKNickRow
-(instancetype)initWithFrame:(NSRect)frame{
 if(!(self=[super initWithFrame:frame]))return nil;
 self.madeAt=CACurrentMediaTime();
 NSTextField *label=[NSTextField labelWithString:@""];label.font=[NSFont systemFontOfSize:12];
 label.textColor=[NSColor colorWithCalibratedWhite:0.88 alpha:1];label.lineBreakMode=NSLineBreakByTruncatingTail;[self addSubview:label];self.label=label;
 NSButton *button=[NSButton buttonWithTitle:settings_text("修改","Change") target:self action:@selector(change:)];
 button.controlSize=NSControlSizeSmall;button.font=[NSFont systemFontOfSize:11];button.refusesFirstResponder=YES;[button sizeToFit];
 [self addSubview:button];self.button=button;
 self.toolTip=settings_text("昵称代替匿名代号显示在排行榜和截图分享里；三个游戏共用，全站唯一，随时可改。设置昵称不会打开任何上传开关。",
  "Shown instead of the anonymous code on the board and the shared screenshots; the same in the three games, unique on the site, changeable at any time. Setting it turns on no upload switch.");
 [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(changed:) name:TOTK_NICK_CHANGED_NOTE object:nil];
 [self show];
 return self;
}
-(void)dealloc{[NSNotificationCenter.defaultCenter removeObserver:self];}
-(BOOL)usable{return totk_board_dir()!=nil;}
/* The page is shown (and twice a second while it is): the nickname read once in this opening. */
-(void)refresh{
 if([self usable]&&!nick_row_fetching&&nick_row_fetched_at<self.madeAt)nick_row_fetch();
 [self show];
}
-(void)changed:(NSNotification *)n{(void)n;[self show];}
-(NSString *)text{
 if(![self usable])return settings_text("昵称：这次运行不能设置","Nickname: not available in this run");
 if(nick_row_known)return nick_row_name?[NSString stringWithFormat:settings_text("昵称：%@","Nickname: %@"),nick_row_name]
                                       :settings_text("昵称：未设置（显示为匿名代号）","Nickname: not set (the anonymous code shows)");
 return nick_row_error?settings_text("昵称：读不到","Nickname: could not be read"):settings_text("昵称：正在读取…","Nickname: reading…");
}
/* The name, and 修改 right after it (the name cut short before it would push the button out). */
-(void)show{
 self.label.stringValue=[self text];self.label.toolTip=self.label.stringValue;
 self.button.enabled=[self usable]&&!nick_row_busy;
 CGFloat W=NSWidth(self.bounds),H=NSHeight(self.bounds);NSSize b=self.button.frame.size;
 CGFloat text=ceil([self.label.stringValue sizeWithAttributes:@{NSFontAttributeName:self.label.font}].width)+6;   /* + the field's own inset */
 CGFloat w=fmax(0,fmin(text,W-b.width-8));
 self.label.frame=NSMakeRect(0,floor((H-16)/2),w,16);
 self.button.frame=NSMakeRect(w+8,floor((H-b.height)/2),b.width,b.height);
}
-(void)change:(NSButton *)button{(void)button;nick_row_change();}
@end
