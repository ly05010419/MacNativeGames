/* Optional native player-cheat panel. The application supplies a private catalog;
   no game addresses, instructions, keys or player data belong in this repository. */
#import "cheat_native.h"
@interface TOTKCheatEntry:NSObject {
 @public CvProgram program;BOOL selected,applied,pure,hotkeys;uint64_t lastKeys;int result,hostIssue;
}
@property NSDictionary *record;
@property NSString *status;
@end
@implementation TOTKCheatEntry
-(void)dealloc{cv_free(&program);}
@end
@interface TOTKCheatPanel:NSObject<NSTableViewDataSource,NSTableViewDelegate,NSSearchFieldDelegate,NSWindowDelegate> {
 CnMemory *_memory;unsigned _cursor;BOOL _master,_valid;NSArray<TOTKCheatEntry *> *_entries,*_visible;
 uint64_t _padLast;BOOL _padReady;
 NSPanel *_panel;NSTableView *_table;NSSearchField *_search;NSPopUpButton *_category;
 NSTextField *_detail,*_summary;NSButton *_pause;NSTimer *_timer;NSString *_configPath,*_catalogError;
}
-(void)show;
-(void)dismiss;
@end
static TOTKCheatPanel *cheatPanel;
static NSBundle *cheat_bundle(void){
 NSString *path=NSProcessInfo.processInfo.arguments.firstObject.stringByStandardizingPath;
 if(!path.isAbsolutePath)path=[NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path];
 path=path.stringByResolvingSymlinksInPath;
 while(path.length>1){if([path.pathExtension.lowercaseString isEqualToString:@"app"])return [NSBundle bundleWithPath:path];path=path.stringByDeletingLastPathComponent;}return NSBundle.mainBundle;
}
@implementation TOTKCheatPanel
-(instancetype)init{
 if(!(self=[super init]))return nil;
 _memory=calloc(1,sizeof *_memory);if(!_memory)return self;
 NSString *override=NSProcessInfo.processInfo.environment[@"TOTK_CHEAT_CATALOG"];
 NSString *path=override?:[cheat_bundle().resourcePath stringByAppendingPathComponent:@"PlayerCheats/v121/catalog.json"];
 NSData *data=[NSData dataWithContentsOfFile:path];NSDictionary *catalog=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
 if(![catalog isKindOfClass:NSDictionary.class])catalog=@{};
 NSString *bidPath=[NSString stringWithFormat:@"extracted/%s/exefs/main.build-id",TOTK_GAME_DIR];
 NSString *bid=[[NSString stringWithContentsOfFile:bidPath encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
 _valid=[catalog isKindOfClass:NSDictionary.class]&&[catalog[@"build_id"] isKindOfClass:NSString.class]&&[bid.uppercaseString isEqualToString:[catalog[@"build_id"] uppercaseString]]&&[catalog[@"entries"] isKindOfClass:NSArray.class];
 _catalogError=_valid?@"":@"未找到匹配本游戏版本的金手指目录；未启用任何修改。";
 const char *profile=getenv("TOTK_PROFILE_DIR");NSString *dir=profile?[@(profile) stringByDeletingLastPathComponent]:[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames/TOTK"];
 _configPath=[dir stringByAppendingPathComponent:@"cheats.json"];
 NSDictionary *saved=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:_configPath]?:[NSData data] options:0 error:nil];
 if(![saved isKindOfClass:NSDictionary.class])saved=@{};
 NSSet *enabled=[NSSet setWithArray:[saved[@"enabled"] isKindOfClass:NSArray.class]?saved[@"enabled"]:@[]];_master=saved[@"master"]?[saved[@"master"] boolValue]:YES;
 NSMutableArray *entries=[NSMutableArray new];
 if(_valid)for(NSDictionary *record in catalog[@"entries"]){
  if(entries.count==CN_ENTRIES||![record isKindOfClass:NSDictionary.class]||![record[@"name"] isKindOfClass:NSString.class]||![record[@"id"] isKindOfClass:NSString.class])continue;
  TOTKCheatEntry *e=[TOTKCheatEntry new];e.record=record;e.status=@"关闭";e->pure=YES;
  NSArray *words=record[@"words"];
  if([words isKindOfClass:NSArray.class]&&words.count&&words.count<=65536){
   uint32_t *raw=calloc(words.count,4);BOOL good=raw!=NULL;
   for(NSUInteger i=0;good&&i<words.count;i++){unsigned x=0;NSScanner *s=[NSScanner scannerWithString:[words[i] description]];good=[s scanHexInt:&x]&&s.isAtEnd;raw[i]=x;}
   if(good&&cv_compile(&e->program,raw,words.count)){
    e->selected=[enabled containsObject:record[@"id"]];
    for(size_t i=0;i<e->program.count;i++){unsigned op=e->program.code[i].w[0]>>28;if(op!=0&&op!=2&&op!=8)e->pure=NO;if(op==8)e->hotkeys=YES;}
   }else e.status=@"格式不支持";
   free(raw);
  }else e.status=record[@"reason"]?:@"待适配：缺少 1.2.1 代码";
  [entries addObject:e];
 }
 _entries=entries;_visible=entries;
 NSArray *guards=catalog[@"guards"];
 if(_valid&&[guards isKindOfClass:NSArray.class]&&guards.count<=CN_PATCHES){
  _memory->guards=calloc(guards.count,sizeof(CnGuard));_memory->nguards=_memory->guards?(unsigned)guards.count:0;
  if(guards.count&&!_memory->guards){_valid=NO;_catalogError=@"无法分配金手指核对表";}
  for(NSUInteger i=0;i<_memory->nguards;i++){_memory->guards[i].offset=[guards[i][@"offset"] unsignedLongLongValue];_memory->guards[i].original=[guards[i][@"original"] unsignedIntValue];}
 }
 /* Idle with no selected entries: no guest suspends and no memory scans. */
 _timer=[NSTimer timerWithTimeInterval:1.0/30 repeats:YES block:^(NSTimer *t){(void)t;[self tick];}];
 [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];return self;
}
-(void)save{
 NSMutableArray *ids=[NSMutableArray new];for(TOTKCheatEntry *e in _entries)if(e->selected)[ids addObject:e.record[@"id"]];
 NSData *data=[NSJSONSerialization dataWithJSONObject:@{@"master":@(_master),@"enabled":ids,@"schema":@1} options:NSJSONWritingPrettyPrinted error:nil];
 [NSFileManager.defaultManager createDirectoryAtPath:_configPath.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
 if(data&&![data writeToFile:_configPath options:NSDataWritingAtomic error:nil])_catalogError=@"开关保存失败，请检查磁盘空间。";
}
-(void)tick{
 if(_panel.isKeyWindow){
  RuntimeGamepadState pad;runtime_gamepad_text_read(&pad);uint64_t buttons=pad.buttons;
  if(pad.ly>16384)buttons|=1u<<13;if(pad.ly< -16384)buttons|=1u<<15;
  if(!buttons)_padReady=YES;uint64_t edge=_padReady?(buttons&~_padLast):0;_padLast=buttons;
  if(edge&2){[_panel performClose:nil];return;}
  if(edge&((1u<<13)|(1u<<15))){NSInteger row=_table.selectedRow+(edge&(1u<<15)?1:-1);if(row<0)row=0;if((NSUInteger)row<_visible.count){[_table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];[_table scrollRowToVisible:row];}}
  if((edge&1)&&_table.selectedRow>=0){NSButton *sender=[NSButton new];sender.tag=_table.selectedRow;[self toggle:sender];}
 }
 if(!_valid||!_memory||!runtime_cheat_module||!runtime_cheat_thread_ports||!runtime_cheat_region_bases)return;
 Module *module=runtime_cheat_module();if(!module)return;_memory->module=module;
 uint64_t keys=(runtime_gamepad_peek_buttons?runtime_gamepad_peek_buttons():0)|runtime_keyboard_buttons();
 BOOL needed=NO;for(TOTKCheatEntry *e in _entries){BOOL want=_master&&e->selected&&e->program.code;if((e->applied&&!want)||(want&&(!e->pure||!e->applied||(e->hotkeys&&e->lastKeys!=keys)))){needed=YES;break;}}
 if(!needed)return;
 /* Foundation and allocations finish BEFORE suspending any game thread. */
 TOTKCheatEntry *__unsafe_unretained objects[CN_ENTRIES];unsigned n=(unsigned)_entries.count;for(unsigned i=0;i<n;i++)objects[i]=_entries[i];
 uint32_t ports[512];unsigned count=runtime_cheat_thread_ports(ports,512);if(count==UINT32_MAX)return;
 CvHost host={.context=_memory,.read=cn_read,.write=cn_write,.keys=keys,.budget=50000};runtime_cheat_region_bases(host.bases);host.bases[0]=(uintptr_t)module->base;
 if(!count)return;
 unsigned suspended=0;for(;suspended<count;suspended++)if(thread_suspend(ports[suspended]))break;
 _memory->deadline=runtime_now_ns()+UINT64_C(4000000);_memory->timedout=0;
 if(suspended==count){
  /* Pure C only in this section, including failure/rollback paths. */
  for(unsigned k=0;k<n;k++){
   unsigned i=(_cursor+k)%n;TOTKCheatEntry *__unsafe_unretained e=objects[i];BOOL want=_master&&e->selected&&e->program.code;
   if(e->applied&&!want){if(cn_release(_memory,i))e->applied=NO;else e->result=CV_MEMORY;}
   if(want&&(!e->pure||!e->applied||(e->hotkeys&&e->lastKeys!=keys))){
    _memory->entry=i;_memory->npages=_memory->nwrites=0;_memory->issue=0;
    e->result=cv_execute(&e->program,&host);
    if(e->result==CV_OK){if(cn_commit(_memory)){e->applied=YES;e->lastKeys=keys;}else e->result=CV_MEMORY;}
    e->hostIssue=_memory->issue;
   }
   if(runtime_now_ns()>=_memory->deadline){_cursor=(i+1)%n;break;}
  }
 }
 for(unsigned i=0;i<suspended;i++)if(thread_resume(ports[i]))_exit(75);
 for(unsigned i=0;i<count;i++)mach_port_deallocate(mach_task_self(),ports[i]);
 /* Guest threads are running again: UI/status and Foundation are safe now. */
 for(TOTKCheatEntry *e in _entries)if(e->program.code){
  if(!e->selected)e.status=e->applied?@"等待恢复":@"关闭";
  else if(!_master)e.status=@"已暂停";
  else if(e->result==CV_BUDGET)e.status=@"超出执行预算";
  else if(e->result==CV_MEMORY)e.status=e->hostIssue==3?@"需要重启以切换代码区":e->hostIssue==2?@"与已选代码冲突":e->hostIssue==1?@"原值核对失败，已拒绝修改":@"等待游戏数据";
  else if(e->result==CV_BAD_CODE)e.status=@"不支持的指令参数";
  else e.status=e->applied?@"已应用 · 效果待游戏验证":@"等待游戏载入";
 }
 if(_panel.visible)[self refreshStatus];
}
-(void)filter:(id)sender{
 (void)sender;NSString *q=_search.stringValue,*category=_category.titleOfSelectedItem;
 NSPredicate *p=[NSPredicate predicateWithBlock:^BOOL(TOTKCheatEntry *e,NSDictionary *bindings){(void)bindings;
  BOOL match=!q.length||[[e.record[@"name"] stringByAppendingFormat:@" %@",e.record[@"description"]?:@""] localizedCaseInsensitiveContainsString:q];
  return match&&([category isEqualToString:@"全部"]||([category isEqualToString:@"已选"]?e->selected:[category isEqualToString:e.record[@"category"]]));}];
 _visible=[_entries filteredArrayUsingPredicate:p];[_table reloadData];[self refreshStatus];
}
-(void)controlTextDidChange:(NSNotification *)note{(void)note;[self filter:nil];}
-(void)refreshStatus{
 NSUInteger active=0,available=0;for(TOTKCheatEntry *e in _entries){if(e->selected)active++;if(e->program.code)available++;}
 _summary.stringValue=_catalogError.length?_catalogError:[NSString stringWithFormat:@"1.2.1 · %lu 项代码 · 已选 %lu · %@ · 手柄 ↑↓ 选择 / A 开关 / B 关闭",(unsigned long)available,(unsigned long)active,_master?@"运行中":@"已暂停"];
 _pause.title=_master?@"暂停全部":@"恢复已选";
 NSIndexSet *rows=[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0,_visible.count)];[_table reloadDataForRowIndexes:rows columnIndexes:[NSIndexSet indexSetWithIndex:2]];
}
-(void)pause:(id)sender{(void)sender;_master=!_master;[self save];[self tick];[self refreshStatus];}
-(void)toggle:(NSButton *)sender{
 NSInteger row=sender.tag;if(row<0||(NSUInteger)row>=_visible.count)return;TOTKCheatEntry *e=_visible[row];if(!e->program.code)return;
 e->selected=!e->selected;
 if(e->selected){
  NSString *group=e.record[@"group"];NSDictionary *writes=e.record[@"writes"];
  for(TOTKCheatEntry *other in _entries)if(other!=e&&other->selected){
   BOOL conflict=group.length&&[group isEqualToString:other.record[@"group"]];
   NSDictionary *ow=other.record[@"writes"];for(NSString *address in writes)if(ow[address]&&![ow[address] isEqual:writes[address]]){conflict=YES;break;}
   if(conflict)other->selected=NO;
  }
 }
 [self save];[self tick];[self filter:nil];
}
-(NSInteger)numberOfRowsInTableView:(NSTableView *)table{(void)table;return _visible.count;}
-(NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row{
 (void)table;TOTKCheatEntry *e=_visible[row];NSString *key=column.identifier;
 if([key isEqualToString:@"toggle"]){NSButton *b=[NSButton checkboxWithTitle:@"" target:self action:@selector(toggle:)];b.state=e->selected?NSControlStateValueOn:NSControlStateValueOff;b.enabled=e->program.code!=NULL;b.tag=row;b.toolTip=e.record[@"name"];return b;}
 NSTextField *f=[NSTextField labelWithString:[key isEqualToString:@"name"]?e.record[@"name"]:e.status];f.lineBreakMode=NSLineBreakByTruncatingTail;f.textColor=e->program.code?NSColor.labelColor:NSColor.secondaryLabelColor;f.font=[NSFont systemFontOfSize:13];return f;
}
-(void)tableViewSelectionDidChange:(NSNotification *)notification{
 (void)notification;NSInteger row=_table.selectedRow;if(row<0||(NSUInteger)row>=_visible.count){_detail.stringValue=@"选择一个项目查看说明。";return;}
 TOTKCheatEntry *e=_visible[row];_detail.stringValue=[NSString stringWithFormat:@"%@\n%@\n%@",e.record[@"name"],e.record[@"description"]?:@"停止开关会停止持续写入。道具、地图与任务变化可能随存档保留。",e.record[@"source"]?:@""];
}
-(void)dismiss{if(_panel.visible)[_panel performClose:nil];}
-(void)windowWillClose:(NSNotification *)note{
 (void)note;runtime_gamepad_text_capture(0);runtime_input_clear();[window makeKeyAndOrderFront:nil];front_mouse_update();
}
-(void)show{
 if(!_panel){
  _panel=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,900,650) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  _panel.title=@"王国之泪 · 金手指";_panel.delegate=self;_panel.releasedWhenClosed=NO;_panel.minSize=NSMakeSize(740,500);_panel.collectionBehavior=NSWindowCollectionBehaviorFullScreenAuxiliary;
  NSView *v=_panel.contentView;CGFloat w=v.bounds.size.width,h=v.bounds.size.height;
  _search=[[NSSearchField alloc] initWithFrame:NSMakeRect(20,h-56,400,30)];_search.placeholderString=@"搜索功能 / 倍率 / 热键";_search.delegate=self;_search.autoresizingMask=NSViewMinYMargin;[v addSubview:_search];
  _category=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(435,h-56,170,30)];[_category addItemsWithTitles:@[@"全部",@"已选",@"战斗",@"移动",@"物品",@"生存",@"地图",@"贤者",@"更多"]];_category.target=self;_category.action=@selector(filter:);_category.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;[v addSubview:_category];
  _pause=[NSButton buttonWithTitle:@"暂停全部" target:self action:@selector(pause:)];_pause.frame=NSMakeRect(w-155,h-56,135,30);_pause.autoresizingMask=NSViewMinXMargin|NSViewMinYMargin;[v addSubview:_pause];
  NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(20,155,w-40,h-230)];scroll.hasVerticalScroller=YES;scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  _table=[[NSTableView alloc] initWithFrame:scroll.bounds];_table.rowHeight=31;_table.usesAlternatingRowBackgroundColors=YES;_table.delegate=self;_table.dataSource=self;
  NSArray *ids=@[@"toggle",@"name",@"status"],*titles=@[@"开关",@"功能",@"状态"];
  for(unsigned i=0;i<3;i++){NSTableColumn *c=[[NSTableColumn alloc] initWithIdentifier:ids[i]];c.title=titles[i];c.width=i==0?50:i==1?480:260;[_table addTableColumn:c];}
  scroll.documentView=_table;[v addSubview:scroll];
  _detail=[NSTextField wrappingLabelWithString:@"选择一个项目查看说明。新项目默认关闭；同类倍率会自动互斥。"];
  _detail.frame=NSMakeRect(22,50,w-44,92);_detail.autoresizingMask=NSViewWidthSizable|NSViewMaxYMargin;_detail.textColor=NSColor.secondaryLabelColor;[v addSubview:_detail];
  _summary=[NSTextField labelWithString:@""];_summary.frame=NSMakeRect(22,18,w-44,24);_summary.autoresizingMask=NSViewWidthSizable|NSViewMaxYMargin;[v addSubview:_summary];[_panel center];
 }
 if(settingsOpen)settings_show(0);[window makeFirstResponder:nil];runtime_input_clear();runtime_gamepad_text_capture(1);_padReady=NO;_padLast=0;[_panel makeKeyAndOrderFront:nil];front_mouse_update();[self filter:nil];
}
@end
static void cheat_panel_show(void){if(!cheatPanel)cheatPanel=[TOTKCheatPanel new];[cheatPanel show];}

static void cheat_panel_dismiss(void){[cheatPanel dismiss];}
