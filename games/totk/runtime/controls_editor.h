/* macOS-only binding sheet, included by frontend.m (excluded from the iOS target).
   Persistence is controls.txt, shared with the launcher and runtime. */
#pragma once
static NSString *controls_key_label(unsigned key){
 static const char *const labels[128]={
  [0]="A",[1]="S",[2]="D",[3]="F",[4]="H",[5]="G",[6]="Z",[7]="X",[8]="C",[9]="V",[11]="B",
  [12]="Q",[13]="W",[14]="E",[15]="R",[16]="Y",[17]="T",[18]="1",[19]="2",[20]="3",[21]="4",[22]="6",[23]="5",
  [24]="=",[25]="9",[26]="7",[27]="-",[28]="8",[29]="0",[30]="]",[31]="O",[32]="U",[33]="[",[34]="I",[35]="P",
  [36]="Return",[37]="L",[38]="J",[39]="'",[40]="K",[41]=";",[42]="\\",[43]=",",[44]="/",[45]="N",[46]="M",[47]=".",
  [48]="Tab",[49]="Space",[50]="`",[51]="Delete",[53]="Escape",[54]="Right Command",[55]="Left Command",[56]="Left Shift",[57]="Caps Lock",
  [58]="Left Option",[59]="Left Control",[60]="Right Shift",[61]="Right Option",[62]="Right Control",[63]="Fn",
  [65]="Keypad .",[67]="Keypad *",[69]="Keypad +",[71]="Keypad Clear",[75]="Keypad /",[76]="Keypad Enter",[78]="Keypad -",[81]="Keypad =",
  [82]="Keypad 0",[83]="Keypad 1",[84]="Keypad 2",[85]="Keypad 3",[86]="Keypad 4",[87]="Keypad 5",[88]="Keypad 6",[89]="Keypad 7",[91]="Keypad 8",[92]="Keypad 9",
  [96]="F5",[97]="F6",[98]="F7",[99]="F3",[100]="F8",[101]="F9",[103]="F11",[109]="F10",[111]="F12",[115]="Home",[116]="Page Up",[117]="Forward Delete",[118]="F4",[119]="End",[120]="F2",[121]="Page Down",[122]="F1",[123]="Left",[124]="Right",[125]="Down",[126]="Up"};
 return labels[key]?[NSString stringWithUTF8String:labels[key]]:[NSString stringWithFormat:@"Key %u",key];
}
static NSTextField *controls_label(NSView *view,NSString *text,NSRect frame){
 NSTextField *label=[NSTextField labelWithString:text];label.frame=frame;[view addSubview:label];return label;
}
@interface TOTKControlsRows:NSView
@end
@implementation TOTKControlsRows
-(BOOL)isFlipped{return YES;}
@end
@interface TOTKControlsEditor:NSObject {
 @public ControlsLayout draft;
}
@property(strong) NSPanel *panel;
@property(strong) NSData *original;
@property(copy) NSString *path;
@property(strong) NSMutableArray<NSPopUpButton *> *keyTargets,*padTargets,*padModes,*padFire;
@property(strong) NSTextField *message;
-(void)open:(id)sender;
-(BOOL)loadLayout:(NSError **)error;
-(BOOL)saveLayout:(NSError **)error;
@end
static TOTKControlsEditor *controlsEditor;
@implementation TOTKControlsEditor
-(NSError *)error:(NSString *)message{return [NSError errorWithDomain:@"MacNativeGames.Controls" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];}
-(BOOL)loadLayout:(NSError **)error{
 const char *path=getenv("TOTK_CONTROLS");
 if(!path||!*path){if(error)*error=[self error:@(TOTK_UI("启动器未设置 controls.txt 路径。","The launcher did not set a controls.txt path."))];return NO;}
 self.path=@(path);NSError *problem=nil;NSData *data=[NSData dataWithContentsOfFile:self.path options:0 error:&problem];
 if(!data&&problem.code!=NSFileReadNoSuchFileError){if(error)*error=problem;return NO;}
 NSString *text=data?[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding]:@"";
 ControlsLayout next;controls_layout_default(&next);const char *why="invalid UTF-8 or embedded NUL";
 int line=!text||data.length>=16384||(data&&memchr(data.bytes,0,data.length))?1:controls_layout_parse(text.UTF8String,&next,&why);
 if(line){if(error)*error=[self error:[NSString stringWithFormat:@"controls.txt, line %d: %s",line,why]];return NO;}
 self.original=data;draft=next;return YES;
}
-(BOOL)saveLayout:(NSError **)error{
 char text[16384];if(!controls_layout_format(&draft,text,sizeof text)){if(error)*error=[self error:@"Invalid bindings"];return NO;}
 NSError *problem=nil;NSData *current=[NSData dataWithContentsOfFile:self.path options:0 error:&problem];
 if(!current&&problem.code!=NSFileReadNoSuchFileError){if(error)*error=problem;return NO;}
 if((current||self.original)&&![current isEqualToData:self.original]){
  if(error)*error=[self error:@(TOTK_UI("controls.txt 已被其他程序修改。取消后重新打开以读取新内容。","controls.txt changed outside the editor. Cancel and reopen to read it."))];return NO;
 }
 /* Keep the player's comments while replacing the binding lines with one canonical layout. */
 NSMutableString *output=[NSMutableString string];NSString *old=self.original?[[NSString alloc] initWithData:self.original encoding:NSUTF8StringEncoding]:@"";
 for(NSString *row in [old componentsSeparatedByString:@"\n"]){NSRange hash=[row rangeOfString:@"#"];if(hash.location!=NSNotFound)[output appendFormat:@"%@\n",[row substringFromIndex:hash.location]];}
 [output appendString:@(text)];NSData *data=[output dataUsingEncoding:NSUTF8StringEncoding];
 if(data.length>=16384){if(error)*error=[self error:@"controls.txt is too large (maximum 16383 bytes)"];return NO;}
 if(![data writeToFile:self.path options:NSDataWritingAtomic error:error])return NO;
 self.original=data;runtime_keyboard_layout_set(draft.keys);runtime_controller_layout_set(&draft);return YES;
}
-(NSPopUpButton *)popup:(NSView *)view frame:(NSRect)frame names:(NSArray<NSString *> *)names tag:(NSInteger)tag action:(SEL)action{
 NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];[p addItemsWithTitles:names];p.tag=tag;p.target=self;p.action=action;[view addSubview:p];return p;
}
-(NSView *)scrollPage:(NSTabView *)tabs name:(NSString *)name rows:(unsigned)rows{
 NSTabViewItem *item=[[NSTabViewItem alloc] initWithIdentifier:name];item.label=name;
 NSScrollView *scroll=[[NSScrollView alloc] initWithFrame:NSMakeRect(0,0,620,430)];scroll.hasVerticalScroller=YES;scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
 NSView *content=[[TOTKControlsRows alloc] initWithFrame:NSMakeRect(0,0,600,rows*32+12)];scroll.documentView=content;item.view=scroll;[tabs addTabViewItem:item];return content;
}
-(void)refresh{
 for(unsigned i=0;i<CONTROLS_KEYS;i++)[self.keyTargets[i] selectItemAtIndex:draft.keys[i]];
 for(unsigned i=0;i<CONTROLLER_BUTTONS;i++){
  unsigned t=0;while(t<16&&draft.map[i]!=(1u<<t))t++;
  [self.padTargets[i] selectItemAtIndex:t<16?t+1:0];[self.padModes[i] selectItemAtIndex:controller_mode_kind(draft.modes[i])];
  [self.padFire[i] selectItemAtIndex:controller_mode_fire(draft.modes[i])];
  self.padModes[i].enabled=draft.map[i]!=0;self.padFire[i].enabled=controller_mode_kind(draft.modes[i])==CONTROLLER_AIM;
 }
}
-(void)keyChanged:(NSPopUpButton *)sender{draft.keys[sender.tag]=(uint8_t)sender.indexOfSelectedItem;}
-(void)padChanged:(NSPopUpButton *)sender{
 unsigned i=(unsigned)sender.tag,t=(unsigned)self.padTargets[i].indexOfSelectedItem,k=(unsigned)self.padModes[i].indexOfSelectedItem,fire=(unsigned)self.padFire[i].indexOfSelectedItem;
 draft.map[i]=t?(uint16_t)(1u<<(t-1)):0;if(!t)k=CONTROLLER_HOLD;
 if(k==CONTROLLER_AIM&&fire==i)fire=(i+1)%CONTROLLER_BUTTONS;
 draft.modes[i]=(uint8_t)(k|(k==CONTROLLER_AIM?fire<<3:0));[self refresh];
}
-(void)reset:(id)sender{(void)sender;controls_layout_default(&draft);[self refresh];self.message.stringValue=@(TOTK_UI("默认布局将在应用后保存。","Defaults will be saved when you Apply."));}
-(void)cancel:(id)sender{(void)sender;[window endSheet:self.panel];}
-(void)apply:(id)sender{(void)sender;NSError *error=nil;if(![self saveLayout:&error]){self.message.stringValue=error.localizedDescription;return;}[window endSheet:self.panel];}
-(void)open:(id)sender{
 (void)sender;if(self.panel||textEntryOpen||window.attachedSheet)return;
 NSError *error=nil;if(![self loadLayout:&error]){NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("无法读取按键布局","Cannot read bindings"));alert.informativeText=error.localizedDescription;[alert beginSheetModalForWindow:window completionHandler:nil];return;}
 self.panel=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,660,580) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];self.panel.title=@(TOTK_UI("按键绑定","Input Bindings"));
 NSView *view=self.panel.contentView;
 controls_label(view,@(TOTK_UI("键盘、手柄与鼠标共用 controls.txt；应用后立即生效。","Keyboard, controller and mouse share controls.txt. Apply takes effect now.")),NSMakeRect(16,542,630,24));
 controls_label(view,@(TOTK_UI("鼠标仍用 7 键开关。改键同时影响游戏和菜单；1–7 保留为快捷键。","Key 7 toggles mouse mode. Bindings affect gameplay and menus; keys 1–7 stay shortcuts.")),NSMakeRect(16,518,630,24));
 NSTabView *tabs=[[NSTabView alloc] initWithFrame:NSMakeRect(16,78,628,432)];[view addSubview:tabs];
 NSMutableArray *targets=[NSMutableArray arrayWithObject:@(TOTK_UI("未分配","None"))];for(unsigned i=1;i<=CONTROLS_TARGETS;i++)[targets addObject:@(controls_target_name(i))];
 self.keyTargets=[NSMutableArray array];NSView *keyboard=[self scrollPage:tabs name:@(TOTK_UI("键盘（Mac 实体键位）","Keyboard (Mac physical keys)")) rows:CONTROLS_KEYS];
 for(unsigned i=0;i<CONTROLS_KEYS;i++){
  CGFloat y=i*32+6;controls_label(keyboard,[NSString stringWithFormat:@"%@  (key_%u)",controls_key_label(i),i],NSMakeRect(16,y+2,250,24));
  NSPopUpButton *p=[self popup:keyboard frame:NSMakeRect(282,y,280,28) names:targets tag:i action:@selector(keyChanged:)];p.enabled=!controls_key_reserved(i);[self.keyTargets addObject:p];
 }
 self.padTargets=[NSMutableArray array];self.padModes=[NSMutableArray array];self.padFire=[NSMutableArray array];
 NSView *pad=[self scrollPage:tabs name:@(TOTK_UI("手柄与鼠标","Controller and Mouse")) rows:CONTROLLER_BUTTONS];
 NSArray *buttons=[targets subarrayWithRange:NSMakeRange(0,17)];NSMutableArray *physical=[NSMutableArray array];for(unsigned i=0;i<CONTROLLER_BUTTONS;i++)[physical addObject:@(controller_physical_names[i])];
 for(unsigned i=0;i<CONTROLLER_BUTTONS;i++){
  CGFloat y=i*32+6;controls_label(pad,@(controller_physical_names[i]),NSMakeRect(12,y+2,130,24));
  [self.padTargets addObject:[self popup:pad frame:NSMakeRect(144,y,120,28) names:buttons tag:i action:@selector(padChanged:)]];
  [self.padModes addObject:[self popup:pad frame:NSMakeRect(270,y,108,28) names:@[@"hold",@"tap",@"toggle",@"aim"] tag:i action:@selector(padChanged:)]];
  [self.padFire addObject:[self popup:pad frame:NSMakeRect(384,y,184,28) names:physical tag:i action:@selector(padChanged:)]];
 }
 self.message=controls_label(view,@(TOTK_UI("hold：按住；tap：400 毫秒；toggle：切换；aim：按住瞄准、另一键射击。","hold: while down; tap: 400 ms; toggle: on/off; aim: hold to aim, another button fires.")),NSMakeRect(16,46,628,28));self.message.font=[NSFont systemFontOfSize:11];
 NSArray *titles=@[@(TOTK_UI("恢复默认","Reset Defaults")),@(TOTK_UI("取消","Cancel")),@(TOTK_UI("应用","Apply"))];SEL actions[]={@selector(reset:),@selector(cancel:),@selector(apply:)};
 for(unsigned i=0;i<3;i++){NSButton *b=[NSButton buttonWithTitle:titles[i] target:self action:actions[i]];b.frame=NSMakeRect(i?438+(i-1)*102:16,10,i?96:160,30);if(i==1)b.keyEquivalent=@"\e";if(i==2)b.keyEquivalent=@"\r";[view addSubview:b];}
 [self refresh];controlsOpen=YES;runtime_input_suspend(1);front_mouse_update();
 [window beginSheet:self.panel completionHandler:^(NSModalResponse response){(void)response;[self.panel orderOut:nil];self.panel=nil;controlsOpen=NO;runtime_input_suspend(0);front_mouse_update();}];
}
@end
