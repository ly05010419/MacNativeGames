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
@end
static TOTKActionEditor *actionEditor;
@implementation TOTKActionEditor
-(void)changed:(NSPopUpButton *)sender{draft.target[sender.tag]=action_binding_code_at((unsigned)sender.indexOfSelectedItem);}
-(void)reset:(id)sender{(void)sender;action_bindings_default(&draft);for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)[self.targets[i] selectItemAtIndex:action_binding_code_index(draft.target[i])];}
-(void)cancel:(id)sender{(void)sender;[window endSheet:self.panel];}
-(void)apply:(id)sender{
 (void)sender;if(!action_bindings_valid(&draft))return;
 NSError *error=nil;NSData *current=[NSData dataWithContentsOfFile:self.path options:0 error:&error];
 if(!current&&error.code!=NSFileReadNoSuchFileError){self.message.stringValue=error.localizedDescription;return;}
 if((current||self.original)&&![current isEqualToData:self.original]){self.message.stringValue=@(TOTK_UI("文件已被其他程序修改，请取消后重新打开。","File changed outside the editor. Cancel and reopen."));return;}
 /* A failed instruction guard refuses both live Apply and persistence. */
 RuntimeActionBindings previous;runtime_action_bindings_get(&previous);
 if(!runtime_action_bindings_set(&draft)){self.message.stringValue=@(TOTK_UI("版本或指令校验不通过，未保存。","Version/instruction checks failed; nothing saved."));return;}
 NSMutableString *text=[NSMutableString string];for(unsigned i=0;i<ACTION_BINDING_COUNT;i++)[text appendFormat:i?@",%u":@"%u",draft.target[i]];[text appendString:@"\n"];
 if(![[text dataUsingEncoding:NSUTF8StringEncoding] writeToFile:self.path options:NSDataWritingAtomic error:&error]){
  runtime_action_bindings_set(&previous);self.message.stringValue=error.localizedDescription;return;
 }
 self.restartNotice=!runtime_action_bindings_available();[window endSheet:self.panel];
}
-(void)open:(id)sender{
 (void)sender;if(self.panel||textEntryOpen||window.attachedSheet)return;
 if(!runtime_action_bindings_supported()){
  NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("动作绑定仅支持王国之泪 1.0.0 / 1.4.3 Mac。","Player actions support TOTK 1.0.0 / 1.4.3 Mac only."));[alert beginSheetModalForWindow:window completionHandler:nil];return;
 }
 const char *controls=getenv("TOTK_CONTROLS");if(!controls||!*controls)return;
 self.path=[[@(controls) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"player-actions.txt"];
 NSError *error=nil;self.original=[NSData dataWithContentsOfFile:self.path options:0 error:&error];
 if(!self.original&&error.code!=NSFileReadNoSuchFileError){NSAlert *alert=[NSAlert new];alert.messageText=error.localizedDescription;[alert beginSheetModalForWindow:window completionHandler:nil];return;}
 if(self.original){
  NSString *text=[[NSString alloc] initWithData:self.original encoding:NSUTF8StringEncoding];RuntimeActionBindings saved;
  NSString *trimmed=[text stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
  if(!trimmed||self.original.length>=512||memchr(self.original.bytes,0,self.original.length)||!action_bindings_parse(trimmed.UTF8String,&saved)){
   NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("player-actions.txt 格式错误，文件未改动。","Invalid player-actions.txt; file left unchanged."));[alert beginSheetModalForWindow:window completionHandler:nil];return;
  }
 }
 runtime_action_bindings_get(&draft);
 self.panel=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,540,642) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];self.panel.title=@(TOTK_UI("玩家动作（独立于菜单）","Player Actions (independent of menus)"));NSView *view=self.panel.contentView;
 NSTextField *intro=[NSTextField wrappingLabelWithString:@(TOTK_UI("来源是游戏看到的按键。只改场景动作，菜单确认、返回和切页保持原样；首次启用需重启。","Sources are game-visible buttons. Only gameplay actions change; menu confirm/back/tabs stay original. First use needs a restart."))];intro.frame=NSMakeRect(16,572,508,56);[view addSubview:intro];
 self.targets=[NSMutableArray array];NSMutableArray *names=[NSMutableArray array];for(unsigned j=0;j<action_binding_code_count();j++)[names addObject:@(action_binding_name(action_binding_code_at(j)))];
 for(unsigned i=0;i<ACTION_BINDING_COUNT;i++){
  CGFloat y=542-i*30;NSTextField *label=[NSTextField labelWithString:@(action_binding_source_name(i))];label.frame=NSMakeRect(18,y+2,150,24);[view addSubview:label];
  NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSMakeRect(174,y,346,28) pullsDown:NO];[p addItemsWithTitles:names];[p selectItemAtIndex:action_binding_code_index(draft.target[i])];p.tag=i;p.target=self;p.action=@selector(changed:);[view addSubview:p];[self.targets addObject:p];
 }
 self.message=[NSTextField wrappingLabelWithString:runtime_action_bindings_available()?@(TOTK_UI("应用后立即生效。","Apply updates live gameplay.")):@(TOTK_UI("保存后重新启动游戏，以安装动作转换。","Save, then restart to install action translation."))];self.message.frame=NSMakeRect(16,40,508,42);[view addSubview:self.message];
 NSArray *titles=@[@(TOTK_UI("恢复默认","Reset Defaults")),@(TOTK_UI("取消","Cancel")),@(TOTK_UI("应用","Apply"))];SEL actions[]={@selector(reset:),@selector(cancel:),@selector(apply:)};
 for(unsigned i=0;i<3;i++){NSButton *b=[NSButton buttonWithTitle:titles[i] target:self action:actions[i]];b.frame=NSMakeRect(i?330+(i-1)*98:16,8,i?92:150,30);if(i==1)b.keyEquivalent=@"\e";if(i==2)b.keyEquivalent=@"\r";[view addSubview:b];}
 actionEditorOpen=YES;runtime_input_suspend(1);front_mouse_update();
 [window beginSheet:self.panel completionHandler:^(NSModalResponse response){(void)response;[self.panel orderOut:nil];self.panel=nil;actionEditorOpen=NO;runtime_input_suspend(0);front_mouse_update();
  if(self.restartNotice){self.restartNotice=NO;NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("动作布局已保存，下次启动生效","Actions saved; restart the game to enable them"));alert.informativeText=@(TOTK_UI("首次启用需重启。TOTK_PLAYER_ACTIONS=0 会禁用此功能。","First use needs a restart. TOTK_PLAYER_ACTIONS=0 disables this feature."));[alert beginSheetModalForWindow:window completionHandler:nil];}
 }];
}
@end
