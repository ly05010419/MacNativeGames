/* Mac software keyboard. The NSTextField remains first responder for typing, selection, paste and IMEs.
   Controller focus is separate: south selects / east cancels (reversed for Switch), west deletes, north inserts space,
   L3 changes case, R3 changes QWERTY/AZERTY, L1/R1 move the caret, L2 changes symbols and R2/menu confirm. No game data or renderer dependency. */
#pragma once
#include "totk_ui_lang.h"   /* totk_ui_zh: the host text's language */

/* A shortcut badge belongs to its key, not to a separate legend the player has to memorise. */
@interface TOTKKeyboardKey:NSButton
@property(copy) NSString *shortcut;
@end
@implementation TOTKKeyboardKey
-(void)drawRect:(NSRect)dirty{
 (void)dirty;NSRect bounds=self.bounds;
 NSColor *ink=[NSColor colorWithWhite:1 alpha:self.enabled?0.96:0.28];
 NSMutableParagraphStyle *center=[NSMutableParagraphStyle new];center.alignment=NSTextAlignmentCenter;
 CGFloat titleHeight=self.font.pointSize+5;
 CGFloat y=self.shortcut.length?5:(bounds.size.height-titleHeight)/2;
 [self.title drawInRect:NSMakeRect(2,y,bounds.size.width-4,titleHeight)
  withAttributes:@{NSFontAttributeName:self.font,NSForegroundColorAttributeName:ink,NSParagraphStyleAttributeName:center}];
 if(self.shortcut.length){
  NSDictionary *attributes=@{NSFontAttributeName:[NSFont systemFontOfSize:9 weight:NSFontWeightSemibold],
   NSForegroundColorAttributeName:[NSColor colorWithWhite:1 alpha:self.enabled?0.8:0.25],NSParagraphStyleAttributeName:center};
  CGFloat width=fmin(bounds.size.width-12,ceil([self.shortcut sizeWithAttributes:attributes].width)+14);
  NSRect badge=NSMakeRect((bounds.size.width-width)/2,bounds.size.height-19,width,14);
  [[NSColor colorWithWhite:1 alpha:0.10] setFill];[[NSBezierPath bezierPathWithRoundedRect:badge xRadius:4 yRadius:4] fill];
  [self.shortcut drawInRect:NSInsetRect(badge,2,1) withAttributes:attributes];
 }
}
@end

@interface TOTKTextBar:NSView<NSTextFieldDelegate>
@property(strong) NSTextField *field,*counter,*hint;
@property(strong) NSView *card;
@property(strong) TOTKKeyboardKey *cursorLeft,*cursorRight;
@property(strong) NSMutableArray<NSButton *> *keys;
@property(strong) NSArray<NSArray<NSString *> *> *rows;
@property(strong) NSTimer *controllerTimer;
@property int maxUnits,minUnits,digitsOnly,row,column,previousDirection,lastPadType;
@property uint32_t disabled;
@property uint64_t previousButtons;
@property double repeatAt;
@property BOOL azerty,upper,symbols,armed,finished;
@property(copy) void(^done)(NSString *text); /* nil: cancelled */
-(void)refilter;
-(void)buildWithPrompt:(NSString *)prompt initial:(NSString *)initial secure:(BOOL)secure;
-(void)startController;
@end

/* Chinese exactly when the rest of the host's text is (totk_ui_zh: TOTK_UI_LANG, else the first preferred language);
   French where that is English but the language is French, since the layout and accents follow it too. */
static NSString *text_keyboard_label(NSString *en,NSString *fr,NSString *zh){
 if(totk_ui_zh())return zh;
 const char *override=getenv("TOTK_UI_LANG");
 NSString *language=override&&*override?([NSString stringWithUTF8String:override]?:@"en"):(NSLocale.preferredLanguages.firstObject?:@"en");
 return [language hasPrefix:@"fr"]?fr:en;
}
@implementation TOTKTextBar
-(BOOL)allowed:(unichar)c{
 BOOL digit=c>='0'&&c<='9';uint32_t d=self.disabled;
 if(self.digitsOnly&&!digit)return NO;
 if(c<0x20||c==0x7f)return NO;
 if((d&1u<<1&&c==' ')||(d&1u<<2&&c=='@')||(d&1u<<3&&c=='%')||(d&1u<<4&&c=='/')||(d&1u<<5&&c=='\\')||(d&1u<<6&&digit))return NO;
 return YES;
}
-(void)controlTextDidChange:(NSNotification *)n{(void)n;[self refilter];}
-(void)refilter{
 NSText *ed=self.field.currentEditor;
 if([ed isKindOfClass:NSTextView.class]&&((NSTextView *)ed).hasMarkedText)return;
 NSString *s=self.field.stringValue;NSMutableString *t=[NSMutableString string];
 [s enumerateSubstringsInRange:NSMakeRange(0,s.length) options:NSStringEnumerationByComposedCharacterSequences usingBlock:^(NSString *sub,NSRange r,NSRange er,BOOL *stop){
  (void)r;(void)er;
  for(NSUInteger i=0;i<sub.length;i++)if(![self allowed:[sub characterAtIndex:i]])return;
  if(self.maxUnits>0&&t.length+sub.length>(NSUInteger)self.maxUnits){*stop=YES;return;}
  [t appendString:sub];}];
 if(![t isEqualToString:s]){
  NSRange selection=ed?ed.selectedRange:NSMakeRange(t.length,0);
  self.field.stringValue=t;
  if(ed){ed.string=t;[ed setSelectedRange:NSMakeRange(MIN(selection.location,t.length),0)];}
  NSBeep();
 }
 self.counter.stringValue=self.maxUnits>0?[NSString stringWithFormat:@"%lu / %d",(unsigned long)t.length,self.maxUnits]:@"";
}
-(void)finish:(NSString *)text{
 if(self.finished)return;
 self.finished=YES;[self.controllerTimer invalidate];self.controllerTimer=nil;
 void(^d)(NSString *)=self.done;self.done=nil;if(d)d(text);
}
-(void)confirm{
 NSTextView *editor=(NSTextView *)self.field.currentEditor;
 if([editor isKindOfClass:NSTextView.class]&&editor.hasMarkedText){[editor unmarkText];}
 [self refilter];NSString *s=self.field.stringValue;
 if((NSInteger)s.length<self.minUnits){NSBeep();return;}
 [self finish:s];
}
-(BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel{
 (void)c;(void)tv;
 if(sel==@selector(insertNewline:)){[self confirm];return YES;}
 if(sel==@selector(cancelOperation:)){[self finish:nil];return YES;}
 return NO;
}
-(void)setFrameSize:(NSSize)size{[super setFrameSize:size];self.needsLayout=YES;}
-(void)layout{
 [super layout];if(!self.card)return;
 CGFloat scale=fmin(1,fmin(self.bounds.size.width/800,self.bounds.size.height/480));
 self.card.frame=NSMakeRect((self.bounds.size.width-760*scale)/2,(self.bounds.size.height-440*scale)/2,760*scale,440*scale);
 self.card.bounds=NSMakeRect(0,0,760,440);
}
-(NSTextField *)label:(NSString *)title frame:(NSRect)frame size:(CGFloat)size{
 NSTextField *label=[NSTextField labelWithString:title];label.frame=frame;
 label.font=[NSFont systemFontOfSize:size weight:NSFontWeightMedium];label.textColor=NSColor.whiteColor;
 label.lineBreakMode=NSLineBreakByTruncatingTail;[self.card addSubview:label];return label;
}
-(void)buildWithPrompt:(NSString *)prompt initial:(NSString *)initial secure:(BOOL)secure{
 self.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;self.wantsLayer=YES;
 self.layer.backgroundColor=[NSColor colorWithWhite:0 alpha:0.45].CGColor;
 self.card=[[NSView alloc] initWithFrame:NSMakeRect(0,0,760,440)];self.card.wantsLayer=YES;
 self.card.layer.cornerRadius=20;self.card.layer.borderWidth=1;
 self.card.layer.borderColor=[NSColor colorWithWhite:1 alpha:0.18].CGColor;
 self.card.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:0.98].CGColor;
 [self addSubview:self.card];[self layout];
 [self label:prompt frame:NSMakeRect(24,390,610,28) size:21];
 self.counter=[self label:@"" frame:NSMakeRect(638,393,98,22) size:14];self.counter.alignment=NSTextAlignmentRight;
 self.field=secure?[[NSSecureTextField alloc] initWithFrame:NSMakeRect(24,337,596,40)]:[[NSTextField alloc] initWithFrame:NSMakeRect(24,337,596,40)];
 self.field.font=[NSFont systemFontOfSize:23];self.field.stringValue=initial;self.field.delegate=self;
 self.field.bezelStyle=NSTextFieldRoundedBezel;self.field.accessibilityLabel=prompt;
 [self.card addSubview:self.field];
 self.cursorLeft=[self cursorButton:NO];self.cursorRight=[self cursorButton:YES];
 self.hint=[self label:@"" frame:NSMakeRect(24,12,712,20) size:11];
 self.hint.maximumNumberOfLines=1;
 self.hint.textColor=[NSColor colorWithWhite:0.75 alpha:1];self.hint.alignment=NSTextAlignmentCenter;
 self.lastPadType=-2;[self updateHint];
 char saved[24]={0};runtime_setting_word("keyboard-layout.txt",saved,sizeof saved);
 NSString *layout=[[@(saved) lowercaseString] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
 self.azerty=[layout isEqual:@"azerty"]||(![layout isEqual:@"qwerty"]&&[NSLocale.preferredLanguages.firstObject hasPrefix:@"fr"]);
 self.keys=[NSMutableArray array];[self rebuild];[self refilter];
}
-(NSString *)shortcutFor:(NSString *)key{
 BOOL ps=self.lastPadType==GAMEPAD_TYPE_PLAYSTATION,xbox=self.lastPadType==GAMEPAD_TYPE_XBOX,sw=self.lastPadType==GAMEPAD_TYPE_SWITCH;
 if([key isEqual:@"shift"])return ps?@"L3":xbox?@"LS":text_keyboard_label(@"L stick",@"Stick G",@"左摇杆");
 if([key isEqual:@"layout"])return ps?@"R3":xbox?@"RS":text_keyboard_label(@"R stick",@"Stick D",@"右摇杆");
 if([key isEqual:@"symbols"])return ps?@"L2":xbox?@"LT":sw?@"ZL":@"L2";
 if([key isEqual:@"space"])return ps?@"△":xbox?@"Y":sw?@"X":text_keyboard_label(@"North",@"Haut",@"上键");
 if([key isEqual:@"delete"])return ps?@"□":xbox?@"X":sw?@"Y":text_keyboard_label(@"West",@"Gauche",@"左键");
 if([key isEqual:@"cancel"])return ps?@"○":(xbox||sw)?@"B":text_keyboard_label(@"East",@"Droite",@"右键");
 if([key isEqual:@"done"])return ps?@"R2":xbox?@"RT":sw?@"ZR":@"R2";
 if([key isEqual:@"left"])return ps?@"L1":xbox?@"LB":sw?@"L":@"L1";
 if([key isEqual:@"right"])return ps?@"R1":xbox?@"RB":sw?@"R":@"R1";
 return @"";
}
-(TOTKKeyboardKey *)cursorButton:(BOOL)right{
 TOTKKeyboardKey *b=[TOTKKeyboardKey buttonWithTitle:right?@"→":@"←" target:self action:@selector(cursorClicked:)];
 b.tag=right;b.frame=NSMakeRect(right?684:628,337,52,40);b.bordered=NO;
 [(NSButtonCell *)b.cell setRefusesFirstResponder:YES];b.wantsLayer=YES;b.layer.cornerRadius=8;
 b.layer.backgroundColor=[NSColor colorWithWhite:1 alpha:0.10].CGColor;
 b.font=[NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
 b.accessibilityLabel=right?text_keyboard_label(@"Move cursor right",@"Déplacer le curseur à droite",@"光标右移"):
  text_keyboard_label(@"Move cursor left",@"Déplacer le curseur à gauche",@"光标左移");
 [self.card addSubview:b];return b;
}
-(void)cursorClicked:(TOTKKeyboardKey *)button{[self moveCursor:button.tag!=0];}
-(void)updateHint{
 int detected=-1,type=runtime_gamepad_type(&detected);if(type==GAMEPAD_TYPE_AUTO)type=detected;
 if(type==self.lastPadType)return;self.lastPadType=type;
 NSString *select=type==GAMEPAD_TYPE_PLAYSTATION?@"×":(type==GAMEPAD_TYPE_XBOX||type==GAMEPAD_TYPE_SWITCH)?@"A":text_keyboard_label(@"South",@"Bas",@"下键");
 self.hint.stringValue=[NSString stringWithFormat:text_keyboard_label(
  @"Move: D-pad / stick   ·   Type: %@   ·   Your Mac keyboard works here too",
  @"Déplacer : croix directionnelle / stick   ·   Saisir : %@   ·   Clavier du Mac toujours actif",
  @"移动：方向键 / 摇杆   ·   输入：%@   ·   也可直接使用 Mac 键盘"),select];
 self.cursorLeft.shortcut=[self shortcutFor:@"left"];self.cursorRight.shortcut=[self shortcutFor:@"right"];
 self.cursorLeft.needsDisplay=self.cursorRight.needsDisplay=YES;
 for(TOTKKeyboardKey *b in self.keys){b.shortcut=[self shortcutFor:self.rows[b.tag/100][b.tag%100]];b.needsDisplay=YES;}
}
-(NSString *)titleFor:(NSString *)key{
 if([key isEqual:@"shift"])return self.upper?@"⇧ ABC":@"⇧ abc";
 if([key isEqual:@"layout"])return self.azerty?@"AZERTY":@"QWERTY";
 if([key isEqual:@"symbols"])return self.symbols?@"ABC":@"123 / é";
 if([key isEqual:@"space"])return text_keyboard_label(@"Space",@"Espace",@"空格");
 if([key isEqual:@"delete"])return @"⌫";
 if([key isEqual:@"cancel"])return text_keyboard_label(@"Cancel",@"Annuler",@"取消");
 if([key isEqual:@"done"])return text_keyboard_label(@"Done",@"Valider",@"确定");
 return self.upper?key.uppercaseString:key;
}
-(BOOL)enabledKey:(NSString *)key{
 if([key isEqual:@"space"])return !self.digitsOnly&&[self allowed:' '];
 if(key.length==1)return [self allowed:[key characterAtIndex:0]];
 return YES;
}
-(void)rebuild{
 for(NSButton *b in self.keys)[b removeFromSuperview];[self.keys removeAllObjects];
 if(self.digitsOnly)self.rows=@[@[@"1",@"2",@"3"],@[@"4",@"5",@"6"],@[@"7",@"8",@"9"],@[@"0",@"delete"],@[@"cancel",@"done"]];
 else if(self.symbols)self.rows=@[@[@"1",@"2",@"3",@"4",@"5",@"6",@"7",@"8",@"9",@"0"],
  @[@"é",@"è",@"ê",@"ë",@"à",@"â",@"ä",@"î",@"ï",@"ç"],@[@"ô",@"ö",@"ù",@"û",@"ü",@"œ",@"-",@"'",@".",@"!"],
  @[@"?",@",",@"/",@"@",@"%",@"\\",@"(",@")",@"+",@"="],@[@"layout",@"symbols",@"shift",@"space",@"delete",@"cancel",@"done"]];
 else if(self.azerty)self.rows=@[@[@"a",@"z",@"e",@"r",@"t",@"y",@"u",@"i",@"o",@"p"],
  @[@"q",@"s",@"d",@"f",@"g",@"h",@"j",@"k",@"l",@"m"],@[@"w",@"x",@"c",@"v",@"b",@"n",@"é",@"è",@"à",@"ç"],
  @[@"1",@"2",@"3",@"4",@"5",@"6",@"7",@"8",@"9",@"0"],@[@"layout",@"symbols",@"shift",@"space",@"delete",@"cancel",@"done"]];
 else self.rows=@[@[@"q",@"w",@"e",@"r",@"t",@"y",@"u",@"i",@"o",@"p"],
  @[@"a",@"s",@"d",@"f",@"g",@"h",@"j",@"k",@"l"],@[@"z",@"x",@"c",@"v",@"b",@"n",@"m",@"-",@"'"],
  @[@"1",@"2",@"3",@"4",@"5",@"6",@"7",@"8",@"9",@"0"],@[@"layout",@"symbols",@"shift",@"space",@"delete",@"cancel",@"done"]];
 self.row=MIN(self.row,(int)self.rows.count-1);self.column=MIN(self.column,(int)self.rows[self.row].count-1);
 for(NSUInteger y=0;y<self.rows.count;y++){
  NSArray *row=self.rows[y];CGFloat width=(712-8*(row.count-1))/row.count;
  for(NSUInteger x=0;x<row.count;x++){
   NSString *key=row[x];TOTKKeyboardKey *b=[TOTKKeyboardKey buttonWithTitle:[self titleFor:key] target:self action:@selector(clicked:)];
   b.shortcut=[self shortcutFor:key];
   b.tag=(NSInteger)(y*100+x);b.frame=NSMakeRect(24+x*(width+8),278-y*56,width,46);
   [(NSButtonCell *)b.cell setRefusesFirstResponder:YES];
   b.bezelStyle=NSBezelStyleRegularSquare;b.bordered=NO;b.wantsLayer=YES;b.layer.cornerRadius=9;
   b.font=[NSFont systemFontOfSize:key.length==1?19:13 weight:NSFontWeightSemibold];b.contentTintColor=NSColor.whiteColor;
   b.enabled=[self enabledKey:key];b.accessibilityLabel=[self titleFor:key];b.accessibilityHelp=b.shortcut;
   [self.keys addObject:b];[self.card addSubview:b];
  }
 }
 [self highlight];
}
-(void)highlight{
 for(NSButton *b in self.keys){BOOL selected=b.tag==self.row*100+self.column;
  b.needsDisplay=YES;
  b.layer.backgroundColor=(selected?[NSColor colorWithCalibratedRed:0.15 green:0.55 blue:0.55 alpha:1]:[NSColor colorWithWhite:1 alpha:b.enabled?0.10:0.03]).CGColor;
  b.layer.borderWidth=selected?2:0;b.layer.borderColor=[NSColor colorWithCalibratedRed:0.7 green:0.95 blue:0.8 alpha:1].CGColor;
 }
}
-(void)edit:(NSString *)insertion delete:(BOOL)backspace{
 if(!self.field.currentEditor)[self.window makeFirstResponder:self.field];
 NSTextView *ed=(NSTextView *)self.field.currentEditor;
 if([ed isKindOfClass:NSTextView.class]){
  if(ed.hasMarkedText){NSBeep();return;} /* do not destroy an unfinished input-method composition */
  if(backspace)[ed deleteBackward:nil];else [ed insertText:insertion replacementRange:ed.selectedRange];
 }else{ /* Also used before a window is visible (validation probes). */
  NSString *s=self.field.stringValue;
  if(backspace&&s.length)s=[s substringToIndex:[s rangeOfComposedCharacterSequenceAtIndex:s.length-1].location];
  else if(!backspace)s=[s stringByAppendingString:insertion];
  self.field.stringValue=s;
 }
 [self refilter];
}
-(void)moveCursor:(BOOL)right{
 if(!self.field.currentEditor)[self.window makeFirstResponder:self.field];
 NSTextView *editor=(NSTextView *)self.field.currentEditor;
 if(![editor isKindOfClass:NSTextView.class]||editor.hasMarkedText)return;
 if(right)[editor moveRight:nil];else [editor moveLeft:nil];
}
-(void)activate:(NSString *)key{
 if(self.finished||![self enabledKey:key])return;
 if([key isEqual:@"done"]){[self confirm];return;}
 if([key isEqual:@"cancel"]){[self finish:nil];return;}
 if([key isEqual:@"delete"]){[self edit:@"" delete:YES];return;}
 if([key isEqual:@"shift"])self.upper=!self.upper;
 else if([key isEqual:@"layout"]){self.azerty=!self.azerty;runtime_setting_save("keyboard-layout.txt",self.azerty?"azerty":"qwerty","keyboard layout");}
 else if([key isEqual:@"symbols"])self.symbols=!self.symbols;
 else{[self edit:[key isEqual:@"space"]?@" ":[self titleFor:key] delete:NO];return;}
 [self rebuild];
}
-(void)clicked:(NSButton *)button{
 self.row=(int)button.tag/100;self.column=(int)button.tag%100;[self highlight];[self activate:self.rows[self.row][self.column]];
 if(!self.finished&&!self.field.currentEditor)[self.window makeFirstResponder:self.field];
}
-(void)move:(int)direction{
 if(direction==1||direction==2){int count=(int)self.rows[self.row].count;self.column=(self.column+(direction==1?-1:1)+count)%count;}
 else{
  double fraction=(self.column+0.5)/self.rows[self.row].count;
  self.row=(self.row+(direction==3?-1:1)+(int)self.rows.count)%(int)self.rows.count;
  self.column=MIN((int)(fraction*self.rows[self.row].count),(int)self.rows[self.row].count-1);
 }
 [self highlight];
}
-(void)handlePad:(RuntimeGamepadState)pad time:(double)now active:(BOOL)active{
 if(self.finished)return;
 if(!active||!pad.connected){self.armed=NO;self.previousButtons=pad.buttons;self.previousDirection=0;return;}
 int direction=(pad.buttons&(1ull<<12)||pad.lx < -16000)?1:(pad.buttons&(1ull<<14)||pad.lx>16000)?2:
  (pad.buttons&(1ull<<13)||pad.ly>16000)?3:(pad.buttons&(1ull<<15)||pad.ly < -16000)?4:0;
 if(!self.armed){if(!pad.buttons&&!direction)self.armed=YES;self.previousButtons=pad.buttons;return;}
 uint64_t edges=pad.buttons&~self.previousButtons;self.previousButtons=pad.buttons;
 if(direction){if(direction!=self.previousDirection){[self move:direction];self.repeatAt=now+0.38;}
  else if(now>=self.repeatAt){[self move:direction];self.repeatAt=now+0.09;}}
 self.previousDirection=direction;
 BOOL sw=self.lastPadType==GAMEPAD_TYPE_SWITCH;
 if(edges&(sw?1u:2u)){[self finish:nil];return;}
 if(edges&((1ull<<10)|(1ull<<9))){[self confirm];return;}
 if(edges&4)[self activate:@"delete"];
 if(edges&64)[self moveCursor:NO];if(edges&128)[self moveCursor:YES];
 if(!self.digitsOnly){
  if(edges&8)[self activate:@"space"];
  if(edges&16)[self activate:@"shift"];
  if(edges&32)[self activate:@"layout"];
  if(edges&256)[self activate:@"symbols"];
 }
 if(edges&(sw?2u:1u))[self activate:self.rows[self.row][self.column]];
}
-(void)startController{
 __weak TOTKTextBar *weakSelf=self;
 self.controllerTimer=[NSTimer timerWithTimeInterval:1.0/60 repeats:YES block:^(NSTimer *timer){
  TOTKTextBar *bar=weakSelf;if(!bar){[timer invalidate];return;}
  [bar updateHint];RuntimeGamepadState pad;runtime_gamepad_text_read(&pad);
  [bar handlePad:pad time:NSProcessInfo.processInfo.systemUptime active:bar.window.isKeyWindow];
 }];
 [NSRunLoop.mainRunLoop addTimer:self.controllerTimer forMode:NSRunLoopCommonModes];
}
-(void)dealloc{[_controllerTimer invalidate];}
@end
