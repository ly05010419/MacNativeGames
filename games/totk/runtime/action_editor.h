/* Mac frontend only. Physical bindings remain in controls.txt; this file edits
   only the optional, version-guarded gameplay action table. */
#pragma once
@interface TOTKActionEditor:NSObject {
 @public RuntimeActionBindings draft;
}
@property(strong) NSPanel *panel;
@property(strong) NSMutableArray<NSPopUpButton *> *targets;
@property(strong) NSTextField *message;
@property(copy) NSString *path;
@property(strong) NSData *original;
@property BOOL restartNotice;
-(void)open:(id)sender;
-(void)reset:(id)sender;
-(NSString *)load;
-(NSString *)save;
-(void)addRowsTo:(NSView *)view y0:(CGFloat)y0;
-(void)noticeRestart;
@end
static TOTKActionEditor *actionEditor;
@implementation TOTKActionEditor
-(void)changed:(NSPopUpButton *)sender{draft.target[sender.tag]=action_binding_code_at((unsigned)sender.indexOfSelectedItem);}
-(void)reset:(id)sender{(void)sender;action_bindings_default(&draft);for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)[self.targets[i] selectItemAtIndex:action_binding_code_index(draft.target[i])];}
-(void)cancel:(id)sender{(void)sender;[window endSheet:self.panel];}
/* The checks and the write, shared by this sheet and the settings panel's controller sheet (controls_editor.h): nil when
   saved, else what to tell the player. */
-(NSString *)save{
 if(!action_bindings_valid(&draft))return @(TOTK_UI("动作绑定无效。","Invalid action bindings."));
 NSError *error=nil;NSData *current=[NSData dataWithContentsOfFile:self.path options:0 error:&error];
 if(!current&&error.code!=NSFileReadNoSuchFileError)return error.localizedDescription;
 if((current||self.original)&&![current isEqualToData:self.original])return @(TOTK_UI("文件已被其他程序修改，请取消后重新打开。","File changed outside the editor. Cancel and reopen."));
 /* A failed instruction guard refuses both live Apply and persistence. */
 RuntimeActionBindings previous;runtime_action_bindings_get(&previous);
 if(!runtime_action_bindings_set(&draft))return @(TOTK_UI("版本或指令校验不通过，未保存。","Version/instruction checks failed; nothing saved."));
 NSMutableString *text=[NSMutableString string];for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)[text appendFormat:i?@",%u":@"%u",draft.target[i]];[text appendString:@"\n"];
 NSData *data=[text dataUsingEncoding:NSUTF8StringEncoding];
 if(![data writeToFile:self.path options:NSDataWritingAtomic error:&error]){runtime_action_bindings_set(&previous);return error.localizedDescription;}
 self.original=data;self.restartNotice=!runtime_action_bindings_available();return nil;
}
-(void)apply:(id)sender{(void)sender;NSString *problem=[self save];if(problem){self.message.stringValue=problem;return;}[window endSheet:self.panel];}
/* Reads player-actions.txt into the draft: nil, or what to tell the player (an alert). */
-(NSString *)load{
 if(!runtime_action_bindings_supported())return @(TOTK_UI("动作绑定仅支持王国之泪 1.0.0 / 1.4.3 Mac。","Player actions support TOTK 1.0.0 / 1.4.3 Mac only."));
 const char *controls=getenv("TOTK_CONTROLS");if(!controls||!*controls)return @(TOTK_UI("启动器未设置 controls.txt 路径。","The launcher did not set a controls.txt path."));
 self.path=[[@(controls) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"player-actions.txt"];
 NSError *error=nil;self.original=[NSData dataWithContentsOfFile:self.path options:0 error:&error];
 if(!self.original&&error.code!=NSFileReadNoSuchFileError)return error.localizedDescription;
 if(self.original){
  NSString *text=[[NSString alloc] initWithData:self.original encoding:NSUTF8StringEncoding];RuntimeActionBindings saved;
  NSString *trimmed=[text stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
  if(!trimmed||self.original.length>=512||memchr(self.original.bytes,0,self.original.length)||!action_bindings_parse(trimmed.UTF8String,&saved))
   return @(TOTK_UI("player-actions.txt 格式错误，文件未改动。","Invalid player-actions.txt; file left unchanged."));
 }
 runtime_action_bindings_get(&draft);return nil;
}
/* One band per source button in a flipped list (panel_scroll), the first at y0. */
-(void)addRowsTo:(NSView *)view y0:(CGFloat)y0{
 self.targets=[NSMutableArray array];NSMutableArray *names=[NSMutableArray array];for(unsigned j=0;j<action_binding_code_count();j++)[names addObject:@(action_binding_name(action_binding_code_at(j)))];
 CGFloat w=view.frame.size.width,pw=320;
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++){
  NSView *band=panel_band(view,NSMakeRect(0,y0+i*(PANEL_ROW_H+PANEL_GAP),w,PANEL_ROW_H));panel_label(band,@(action_binding_source_name(i)),12,w-pw-26,PANEL_ROW_H);
  NSPopUpButton *p=panel_popup(band,names,w-pw-8,pw,PANEL_ROW_H);[p selectItemAtIndex:action_binding_code_index(draft.target[i])];p.autoresizingMask=NSViewMinXMargin;p.tag=i;p.target=self;p.action=@selector(changed:);[self.targets addObject:p];
 }
}
-(void)noticeRestart{
 if(!self.restartNotice)return;
 self.restartNotice=NO;NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("动作布局已保存，下次启动生效","Actions saved; restart the game to enable them"));alert.informativeText=@(TOTK_UI("首次启用需重启。TOTK_PLAYER_ACTIONS=0 会禁用此功能。","First use needs a restart. TOTK_PLAYER_ACTIONS=0 disables this feature."));[alert beginSheetModalForWindow:window completionHandler:nil];
}
-(void)open:(id)sender{
 (void)sender;if(self.panel||textEntryOpen||window.attachedSheet)return;
 NSString *problem=[self load];
 if(problem){NSAlert *alert=[NSAlert new];alert.messageText=problem;[alert beginSheetModalForWindow:window completionHandler:nil];return;}
 CGFloat h=600,top=h-PANEL_SHEET_TOP;
 self.panel=panel_sheet(@(TOTK_UI("玩家动作（独立于菜单）","Player Actions (independent of menus)")),PANEL_W,h);NSView *view=self.panel.contentView;
 panel_note(view,@(TOTK_UI("来源是游戏看到的按键。只改场景动作，菜单确认、返回和切页保持原样；首次启用需重启。","Sources are game-visible buttons. Only gameplay actions change; menu confirm/back/tabs stay original. First use needs a restart.")),NSMakeRect(PANEL_MARGIN+4,top-30,PANEL_W-2*PANEL_MARGIN-8,30));
 CGFloat list=top-38-86;
 [self addRowsTo:panel_scroll(view,NSMakeRect(PANEL_MARGIN,86,PANEL_W-2*PANEL_MARGIN,list),ACTION_BINDING_COUNT*(PANEL_ROW_H+PANEL_GAP)-PANEL_GAP,NULL) y0:0];
 self.message=panel_note(view,runtime_action_bindings_available()?@(TOTK_UI("应用后立即生效。","Apply updates live gameplay.")):@(TOTK_UI("保存后重新启动游戏，以安装动作转换。","Save, then restart to install action translation.")),NSMakeRect(PANEL_MARGIN+4,48,PANEL_W-2*PANEL_MARGIN-8,30));
 SEL actions[]={@selector(reset:),@selector(cancel:),@selector(apply:)};
 panel_buttons(view,PANEL_W,@[@(TOTK_UI("恢复默认","Reset Defaults")),@(TOTK_UI("取消","Cancel")),@(TOTK_UI("应用","Apply"))],self,actions);
 actionEditorOpen=YES;runtime_input_suspend(1);front_mouse_update();
 [window beginSheet:self.panel completionHandler:^(NSModalResponse response){(void)response;[self.panel orderOut:nil];self.panel=nil;actionEditorOpen=NO;runtime_input_suspend(0);front_mouse_update();[self noticeRestart];}];
}
@end
