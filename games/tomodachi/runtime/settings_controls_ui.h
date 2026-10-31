/* Upstream PR #1210's drawing/menu interaction, adapted to Tomodachi's maps.
   Controller navigation follows settings_panel.h (PR #977 / #1210). */
- (void)deviceChanged:(NSSegmentedControl *)sender {
 self.keyboardDiagram.hidden=sender.selectedSegment!=0;self.padDiagram.hidden=sender.selectedSegment!=1;
 [self.keyboardDiagram setNeedsDisplay:YES];[self.padDiagram setNeedsDisplay:YES];
}
- (void)controllerType:(NSPopUpButton *)sender {
 atomic_store(&titlePadType,(int)sender.indexOfSelectedItem);preferences[@"controllerType"]=@(sender.indexOfSelectedItem);save_preferences();[self.padDiagram setNeedsDisplay:YES];
}
- (void)assignInput:(int)input action:(int)action {
 if(input>=KEYMAP_MOUSE_FIRST)return; // Mouse is the game's touch input, not TOTK camera controls.
 if(keymap_assign(&titleKeys,input,action)){title_keys_apply();title_controls_save();[self.keyboardDiagram setNeedsDisplay:YES];}
}
- (void)assignPad:(int)physical action:(int)action {
 if(physical<0||physical>=16||action>=KEYMAP_SWITCH_BUTTONS)return;
 uint16_t map[CONTROLLER_BUTTONS];uint8_t modes[CONTROLLER_BUTTONS];runtime_controller_map_get(map,modes);
 controller_map_assign(map,modes,physical,action);atomic_store(&titlePadMap[physical],map[physical]);title_controls_save();[self.padDiagram setNeedsDisplay:YES];
}
- (void)bindingMenu:(NSMenuItem *)sender {
 NSDictionary *item=sender.representedObject;
 if([item[@"pad"] boolValue])[self assignPad:[item[@"input"] intValue] action:[item[@"action"] intValue]];
 else [self assignInput:[item[@"input"] intValue] action:[item[@"action"] intValue]];
}
- (void)popInput:(int)input pad:(BOOL)pad at:(NSRect)at {
 NSMenu *menu=[[NSMenu alloc] initWithTitle:@""];menu.autoenablesItems=NO;
 NSMenuItem *heading=[[NSMenuItem alloc] initWithTitle:pad?settings_pad_button(input,settings_pad_family()):settings_input_text(input) action:nil keyEquivalent:@""];heading.enabled=NO;[menu addItem:heading];[menu addItem:NSMenuItem.separatorItem];
 BOOL supported=pad?input<16:keymap_bindable(input)&&input<KEYMAP_MOUSE_FIRST;
 int current=pad?(input<CONTROLLER_BUTTONS?controller_map_target(atomic_load(&titlePadMap[input])):-1):keymap_action_of(&titleKeys,input,NULL);
 if(supported){
  for(int action=0;action<KEYMAP_ACTIONS;action++){
   if(pad&&action>=KEYMAP_SWITCH_BUTTONS)continue;
   NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:@(titleActionNames[action]) action:@selector(bindingMenu:) keyEquivalent:@""];item.target=self;item.representedObject=@{@"input":@(input),@"action":@(action),@"pad":@(pad)};item.state=current==action;[menu addItem:item];
  }
  [menu addItem:NSMenuItem.separatorItem];NSMenuItem *none=[[NSMenuItem alloc] initWithTitle:@"None" action:@selector(bindingMenu:) keyEquivalent:@""];none.target=self;none.representedObject=@{@"input":@(input),@"action":@(-1),@"pad":@(pad)};[menu addItem:none];
 }else{NSMenuItem *info=[[NSMenuItem alloc] initWithTitle:input==18?@"Key 1 always opens Settings":pad?@"Not reported by this runtime":@"Mouse buttons remain touch input" action:nil keyEquivalent:@""];info.enabled=NO;[menu addItem:info];}
 NSView *view=pad?(NSView *)self.padDiagram:self.keyboardDiagram;
 if(pad)self.padDiagram.selected=input;else self.keyboardDiagram.selected=input;[view setNeedsDisplay:YES];
 [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(NSMinX(at),NSMaxY(at)) inView:view];
 if(pad)self.padDiagram.selected=-1;else self.keyboardDiagram.selected=-1;[view setNeedsDisplay:YES];
}
- (void)resetKeyboard:(id)sender {
 (void)sender;[preferences removeObjectForKey:@"keys"];[preferences removeObjectForKey:@"keyboardMap"];load_keymap();
 // Rebuild keyboard defaults without disturbing the controller map.
 KeyMap saved=titleKeys;memset(&titleKeys,KEYMAP_NONE,sizeof titleKeys);
 for(int input=0;input<128;input++)for(int a=0;a<KEYMAP_ACTIONS;a++)if(atomic_load(&keyMap[input])==canonicalCodes[a]){int slot=titleKeys.slot[a][0]==KEYMAP_NONE?0:1;titleKeys.slot[a][slot]=(uint8_t)input;}
 (void)saved;title_keys_apply();title_controls_save();[self.keyboardDiagram setNeedsDisplay:YES];
}
- (void)resetController:(id)sender {
 (void)sender;for(int i=0;i<CONTROLLER_BUTTONS;i++)atomic_store(&titlePadMap[i],i<16?1u<<i:0);atomic_store(&swapButtons,0);preferences[@"swapButtons"]=@NO;title_controls_save();[self.padDiagram setNeedsDisplay:YES];
}
- (void)controlsTick {
 uint64_t buttons=atomic_load(&titlePadButtons),edges=buttons&~self.previousPad;self.previousPad=buttons;
 if(!atomic_load(&titlePadConnected))return;
 const uint64_t chord=(1ull<<4)|(1ull<<5);
 if((buttons&chord)==chord&&(edges&chord)){runtime_title_settings_key(18);return;}
 if(!atomic_load(&panelOpen))return;
 if(edges&(1ull<<1)){[self close:nil];return;}
 NSInteger page=self.pageTabs.selectedSegment;
 if(edges&((1ull<<6)|(1ull<<7))){page=(page+((edges&(1ull<<7))?1:2))%3;self.pageTabs.selectedSegment=page;[self tab:self.pageTabs];return;}
 int lx=atomic_load(&titlePadLX),ly=atomic_load(&titlePadLY);
 int direction=(buttons&(1ull<<13)||ly>16000)?1:(buttons&(1ull<<15)||ly < -16000)?2:(buttons&(1ull<<12)||lx < -16000)?3:(buttons&(1ull<<14)||lx>16000)?4:0;
 double now=NSProcessInfo.processInfo.systemUptime;
 BOOL step=direction&&(direction!=self.previousDirection||now>=self.repeatAt);
 if(step)self.repeatAt=now+(direction==self.previousDirection?.12:.35);self.previousDirection=direction;
 if(page==2){
  if(edges&(1ull<<0)){self.deviceTabs.selectedSegment=!self.deviceTabs.selectedSegment;[self deviceChanged:self.deviceTabs];return;}
  if(self.deviceTabs.selectedSegment!=1||!step)return;
  NSArray *order=self.padDiagram.order;if(!order.count)return;
  if(direction<=2)self.padIndex=(self.padIndex+(direction==1?order.count-1:1))%order.count;
  else{int physical=[order[self.padIndex] intValue];if(physical<16){int current=controller_map_target(atomic_load(&titlePadMap[physical]));[self assignPad:physical action:(current+1+(direction==3?16:1))%17-1];}}
  self.padDiagram.selected=[order[self.padIndex] intValue];[self.padDiagram setNeedsDisplay:YES];return;
 }
 NSArray *rows=self.tabs.selectedTabViewItem.view.subviews;NSMutableArray<NSControl *> *targets=[NSMutableArray array];
 for(NSView *row in rows)for(NSView *view in row.subviews)if([view isKindOfClass:NSPopUpButton.class]||[view isKindOfClass:NSSwitch.class])[targets addObject:(id)view];
 if(!targets.count)return;
 if(step&&direction<=2)self.padIndex=(self.padIndex+(direction==1?targets.count-1:1))%targets.count;
 self.padIndex%=targets.count;
 for(NSUInteger i=0;i<targets.count;i++){NSView *row=targets[i].superview;row.layer.borderWidth=i==(NSUInteger)self.padIndex?2:0;row.layer.borderColor=[NSColor colorWithRed:.7 green:.95 blue:.8 alpha:1].CGColor;}
 if((step&&direction>=3)||(edges&1)){
  NSControl *control=targets[self.padIndex];if([control isKindOfClass:NSPopUpButton.class]){NSPopUpButton *popup=(id)control;[popup selectItemAtIndex:(popup.indexOfSelectedItem+(direction==3?popup.numberOfItems-1:1))%popup.numberOfItems];}
  else{NSSwitch *toggle=(id)control;toggle.state=!toggle.state;}[control sendAction:control.action to:control.target];
 }
}

- (void)layoutPanel {
 CGFloat width=self.pageTabs.selectedSegment==2?960:600,height=530;
 NSRect area=gameWindow.contentView.bounds;CGFloat scale=MIN(1,MIN((area.size.width-16)/width,(area.size.height-16)/height));
 self.panel.frame=NSMakeRect((area.size.width-width*scale)/2,(area.size.height-height*scale)/2,width*scale,height*scale);self.panel.bounds=NSMakeRect(0,0,width,height);
 self.pageTabs.frame=NSMakeRect(14,450,width-28,26);for(NSInteger i=0;i<3;i++)[self.pageTabs setWidth:(width-28)/3 forSegment:i];
 self.tabs.frame=NSMakeRect(14,54,width-28,380);
 for(NSView *view in self.panel.subviews)if([view isKindOfClass:NSButton.class])view.frame=NSMakeRect(width-42,480,28,28);
 for(NSUInteger page=0;page<2;page++)for(NSView *row in self.tabs.tabViewItems[page].view.subviews)if(row.wantsLayer){
  NSRect frame=row.frame;frame.size.width=width-28;row.frame=frame;
  for(NSView *control in row.subviews){NSRect r=control.frame;if([control isKindOfClass:NSPopUpButton.class])r.origin.x=width-296;else if([control isKindOfClass:NSSwitch.class])r.origin.x=width-78;else r.size.width=width-320;control.frame=r;}
 }
}
