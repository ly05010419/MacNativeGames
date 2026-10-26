/* macOS-only binding sheet, included by frontend.m (excluded from the iOS target).
   Persistence is controls.txt, shared with the launcher and runtime. The app menu opens every page; the settings panel's
   controls row (settings_panel.h, user 2026-10-26) opens one part: the keyboard and the mouse's buttons, or the
   controller's buttons with the controller type and the player actions (action_editor.h). */
#pragma once
static NSString *settings_pad_auto_text(void);   /* settings_panel.h */
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
@interface TOTKControlsEditor:NSObject {
 @public ControlsLayout draft;
}
@property(strong) NSPanel *panel;
@property(strong) NSData *original;
@property(copy) NSString *path;
@property(strong) NSMutableArray<NSPopUpButton *> *keyTargets,*padTargets,*padModes,*padFire;
@property(strong) NSTextField *message;
@property(strong) NSPopUpButton *padType;   /* CONTROLS_PAGE_CONTROLLER: the controller type, set on Apply */
@property int page;
@property BOOL actions;   /* the player actions page is in this sheet */
@property(strong) NSMutableArray<NSScrollView *> *pages;   /* one list per segment, the others hidden */
-(void)open:(id)sender;
-(void)openPage:(int)page;
-(BOOL)loadLayout:(NSError **)error;
-(BOOL)saveLayout:(NSError **)error;
@end
enum { CONTROLS_PAGE_ALL, CONTROLS_PAGE_KEYBOARD, CONTROLS_PAGE_CONTROLLER };
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
-(NSPopUpButton *)popup:(NSView *)band x:(CGFloat)x w:(CGFloat)w names:(NSArray<NSString *> *)names tag:(NSInteger)tag action:(SEL)action{
 NSPopUpButton *p=panel_popup(band,names,x,w,PANEL_ROW_H);p.tag=tag;p.target=self;p.action=action;return p;
}
/* A segment's list in the sheet's list area; a header line of column names first when there is one. */
-(NSView *)scrollPage:(NSRect)frame rows:(unsigned)rows header:(NSArray<NSString *> *)header{
 CGFloat top=header?20:0;NSScrollView *scroll;
 NSView *content=panel_scroll(self.panel.contentView,frame,top+rows*(PANEL_ROW_H+PANEL_GAP)-PANEL_GAP,&scroll);
 for(NSUInteger i=0;i<header.count;i++){NSTextField *t=[NSTextField labelWithString:header[i]];t.font=[NSFont systemFontOfSize:11];
  t.textColor=[NSColor colorWithCalibratedWhite:0.7 alpha:1];t.frame=NSMakeRect((CGFloat[]){12,150,280,386}[i],0,130,16);[content addSubview:t];}
 scroll.hidden=self.pages.count>0;[self.pages addObject:scroll];return content;
}
-(void)showPage:(NSSegmentedControl *)sender{for(NSUInteger i=0;i<self.pages.count;i++)self.pages[i].hidden=(NSInteger)i!=sender.selectedSegment;}
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
-(void)reset:(id)sender{(void)sender;controls_layout_default(&draft);[self refresh];
 if(self.actions){[actionEditor reset:nil];}
 self.message.stringValue=@(TOTK_UI("默认布局将在应用后保存。","Defaults will be saved when you Apply."));}
-(void)cancel:(id)sender{(void)sender;[window endSheet:self.panel];}
-(void)apply:(id)sender{(void)sender;NSError *error=nil;if(![self saveLayout:&error]){self.message.stringValue=error.localizedDescription;return;}
 if(self.padType)runtime_gamepad_type_set((int)self.padType.selectedTag);
 if(self.actions){RuntimeActionBindings now;runtime_action_bindings_get(&now);   /* untouched actions write no file and need no restart */
  if(memcmp(&now,&actionEditor->draft,sizeof now)){NSString *problem=[actionEditor save];if(problem){self.message.stringValue=problem;return;}}}
 [window endSheet:self.panel];}
-(void)open:(id)sender{(void)sender;[self openPage:CONTROLS_PAGE_ALL];}
-(void)openPage:(int)page{
 if(self.panel||textEntryOpen||window.attachedSheet)return;
 NSError *error=nil;if(![self loadLayout:&error]){NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("无法读取按键布局","Cannot read bindings"));alert.informativeText=error.localizedDescription;[alert beginSheetModalForWindow:window completionHandler:nil];return;}
 self.page=page;self.actions=page==CONTROLS_PAGE_CONTROLLER&&actionEditor&&runtime_action_bindings_supported()&&![actionEditor load];
 CGFloat w=PANEL_W-2*PANEL_MARGIN,h=600,top=h-PANEL_SHEET_TOP;
 self.panel=panel_sheet(page==CONTROLS_PAGE_KEYBOARD?@(TOTK_UI("键鼠","Keyboard and Mouse")):page==CONTROLS_PAGE_CONTROLLER?@(TOTK_UI("手柄","Controller")):@(TOTK_UI("按键绑定","Input Bindings")),PANEL_W,h);
 NSView *view=self.panel.contentView;
 panel_note(view,page==CONTROLS_PAGE_CONTROLLER?@(TOTK_UI("键盘、手柄与鼠标共用 controls.txt；应用后立即生效。","Keyboard, controller and mouse share controls.txt. Apply takes effect now.")):
  @(TOTK_UI("键盘、手柄与鼠标共用 controls.txt；应用后立即生效。改键同时影响游戏和菜单；1–9 与 Command 保留给应用。鼠标开关在设置中。","Keyboard, controller and mouse share controls.txt; Apply takes effect now. Bindings affect gameplay and menus; keys 1–9 and Command stay reserved. Mouse mode is in Settings.")),
  NSMakeRect(PANEL_MARGIN+4,top-30,w-8,30));
 CGFloat y=top-38;self.padType=nil;
 if(page==CONTROLS_PAGE_CONTROLLER){
  NSView *band=panel_band(view,NSMakeRect(PANEL_MARGIN,y-PANEL_ROW_H,w,PANEL_ROW_H));y-=PANEL_ROW_H+10;
  panel_label(band,@(TOTK_UI("手柄类型（按键图标）","Controller type (button icons)")),12,w-260-26,PANEL_ROW_H);
  NSPopUpButton *type=panel_popup(band,@[settings_pad_auto_text(),@"Xbox",@"PlayStation",@"Switch"],w-260-8,260,PANEL_ROW_H);
  int tags[]={GAMEPAD_TYPE_AUTO,GAMEPAD_TYPE_XBOX,GAMEPAD_TYPE_PLAYSTATION,GAMEPAD_TYPE_SWITCH};
  for(int i=0;i<4;i++)[type itemAtIndex:i].tag=tags[i];
  [type selectItemWithTag:runtime_gamepad_type(NULL)];self.padType=type;
 }
 /* The segments, as the tabs were: the keyboard (not on the controller sheet), the buttons, the player actions. */
 NSMutableArray *names=[NSMutableArray array];if(page!=CONTROLS_PAGE_CONTROLLER)[names addObject:@(TOTK_UI("键盘（Mac 实体键位）","Keyboard (Mac physical keys)"))];
 [names addObject:page==CONTROLS_PAGE_KEYBOARD?@(TOTK_UI("鼠标按键","Mouse buttons")):page==CONTROLS_PAGE_CONTROLLER?@(TOTK_UI("手柄按键","Controller buttons")):@(TOTK_UI("手柄与鼠标","Controller and Mouse"))];
 if(self.actions)[names addObject:@(TOTK_UI("玩家动作","Player actions"))];
 if(names.count>1){NSSegmentedControl *seg=[NSSegmentedControl segmentedControlWithLabels:names trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(showPage:)];
  seg.selectedSegment=0;seg.refusesFirstResponder=YES;[seg sizeToFit];seg.frame=NSMakeRect(floor((PANEL_W-seg.frame.size.width)/2),y-24,seg.frame.size.width,24);[view addSubview:seg];y-=24+10;}
 NSRect list=NSMakeRect(PANEL_MARGIN,86,w,y-86);self.pages=[NSMutableArray array];
 NSMutableArray *targets=[NSMutableArray arrayWithObject:@(TOTK_UI("未分配","None"))];for(unsigned i=1;i<=CONTROLS_TARGETS;i++)[targets addObject:@(controls_target_name(i))];
 self.keyTargets=[NSMutableArray array];
 NSView *keyboard=page==CONTROLS_PAGE_CONTROLLER?nil:[self scrollPage:list rows:CONTROLS_KEYS header:nil];
 for(unsigned i=0;i<CONTROLS_KEYS;i++){
  NSView *band=keyboard?panel_band(keyboard,NSMakeRect(0,i*(PANEL_ROW_H+PANEL_GAP),keyboard.frame.size.width,PANEL_ROW_H)):nil;
  CGFloat bw=band.frame.size.width;if(band)panel_label(band,[NSString stringWithFormat:@"%@  (key_%u)",controls_key_label(i),i],12,bw-260-26,PANEL_ROW_H);
  NSPopUpButton *p=[self popup:band x:bw-260-8 w:260 names:targets tag:i action:@selector(keyChanged:)];p.autoresizingMask=NSViewMinXMargin;p.enabled=!controls_key_reserved(i);[self.keyTargets addObject:p];
 }
 /* The controller's buttons and the mouse's (CONTROLLER_MOUSE_FIRST on) on their own pages in the panel's sheets. Rows a
    page leaves out keep their pop-ups off screen, so refresh and the draft see every button. */
 unsigned first=page==CONTROLS_PAGE_KEYBOARD?CONTROLLER_MOUSE_FIRST:0,last=page==CONTROLS_PAGE_CONTROLLER?CONTROLLER_MOUSE_FIRST:CONTROLLER_BUTTONS;
 self.padTargets=[NSMutableArray array];self.padModes=[NSMutableArray array];self.padFire=[NSMutableArray array];
 NSView *pad=[self scrollPage:list rows:last-first header:@[@(TOTK_UI("按键","Button")),@(TOTK_UI("映射为","Sends")),@(TOTK_UI("方式","Mode")),@(TOTK_UI("瞄准时开火","Fires while aiming"))]];
 NSArray *buttons=[targets subarrayWithRange:NSMakeRange(0,17)];NSMutableArray *physical=[NSMutableArray array];for(unsigned i=0;i<CONTROLLER_BUTTONS;i++)[physical addObject:@(controller_physical_names[i])];
 for(unsigned i=0;i<CONTROLLER_BUTTONS;i++){
  NSView *band=i>=first&&i<last?panel_band(pad,NSMakeRect(0,20+(i-first)*(PANEL_ROW_H+PANEL_GAP),pad.frame.size.width,PANEL_ROW_H)):nil;
  if(band)panel_label(band,@(controller_physical_names[i]),12,128,PANEL_ROW_H);
  [self.padTargets addObject:[self popup:band x:146 w:124 names:buttons tag:i action:@selector(padChanged:)]];
  [self.padModes addObject:[self popup:band x:276 w:100 names:@[@"hold",@"tap",@"toggle",@"aim"] tag:i action:@selector(padChanged:)]];
  NSPopUpButton *fire=[self popup:band x:382 w:pad.frame.size.width-8-382 names:physical tag:i action:@selector(padChanged:)];fire.autoresizingMask=NSViewWidthSizable;[self.padFire addObject:fire];
 }
 if(self.actions)[actionEditor addRowsTo:[self scrollPage:list rows:ACTION_BINDING_COUNT header:nil] y0:0];
 self.message=panel_note(view,@(TOTK_UI("hold：按住；tap：400 毫秒；toggle：切换；aim：按住瞄准、另一键射击。","hold: while down; tap: 400 ms; toggle: on/off; aim: hold to aim, another button fires.")),NSMakeRect(PANEL_MARGIN+4,48,w-8,30));
 SEL actions[]={@selector(reset:),@selector(cancel:),@selector(apply:)};
 panel_buttons(view,PANEL_W,@[@(TOTK_UI("恢复默认","Reset Defaults")),@(TOTK_UI("取消","Cancel")),@(TOTK_UI("应用","Apply"))],self,actions);
 [self refresh];controlsOpen=YES;runtime_input_suspend(1);front_mouse_update();
 [window beginSheet:self.panel completionHandler:^(NSModalResponse response){(void)response;[self.panel orderOut:nil];self.panel=nil;controlsOpen=NO;runtime_input_suspend(0);front_mouse_update();
  if(self.actions)[actionEditor noticeRestart];}];
}
@end
