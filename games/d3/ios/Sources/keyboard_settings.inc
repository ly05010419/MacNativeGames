/* Settings for the one logical keyboard exposed by iOS. */
- (void)openKeyboardMapping {
    keyboardPage = YES;
    [mappingCard removeFromSuperview];
    mappingCard = [[UIScrollView alloc] initWithFrame:settingsCard.frame];
    mappingCard.backgroundColor = [UIColor colorWithRed:.11 green:.12 blue:.15 alpha:1];
    mappingCard.layer.cornerRadius = 16; mappingCard.clipsToBounds = YES;
    mappingCard.alwaysBounceVertical = YES; [settingsOverlay addSubview:mappingCard];
    CGFloat w = mappingCard.bounds.size.width;
    UIButton *back = [UIButton buttonWithType:UIButtonTypeSystem]; back.frame = CGRectMake(12,8,70,32);
    [back setTitle:@"‹ 返回" forState:UIControlStateNormal];
    [back addTarget:self action:@selector(closeMapping) forControlEvents:UIControlEventTouchUpInside]; [mappingCard addSubview:back];
    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(92,8,w-104,32)];
    title.text = @"系统键盘（蓝牙 / USB）"; title.textColor = UIColor.whiteColor; title.font = [UIFont boldSystemFontOfSize:16]; [mappingCard addSubview:title];
    UILabel *help = [[UILabel alloc] initWithFrame:CGRectMake(16,44,w-32,64)];
    help.text = @"iOS 将所有实体键盘合并成一个设备。\n选择玩家后，按该玩家的映射输入。\n绑定后该玩家仅接收键盘；设置期间暂停输入。";
    help.numberOfLines = 3; help.textColor = UIColor.lightGrayColor; help.font = [UIFont systemFontOfSize:12]; [mappingCard addSubview:help];
    UILabel *status = [[UILabel alloc] initWithFrame:CGRectMake(16,110,w-32,28)];
    status.tag = 2100; status.textColor = UIColor.lightGrayColor; status.font = [UIFont systemFontOfSize:12];
    status.text = GCKeyboard.coalescedKeyboard ? @"键盘已连接" : @"未连接键盘；可以先配置"; [mappingCard addSubview:status];
    int slot = runtime_keyboard_slot(), count = runtime_keyboard_slot_count();
    if (keyboardEditSlot < 0 || keyboardEditSlot >= count) keyboardEditSlot = 0;
    __weak TOTKTouchPad *weakSelf = self;
    UIButton *binding = [UIButton buttonWithType:UIButtonTypeSystem]; binding.frame = CGRectMake(16,140,w-32,36);
    [binding setTitle:slot < 0 ? @"绑定到：不启用 ▾" : [NSString stringWithFormat:@"绑定到：%d号玩家 ▾",slot+1] forState:UIControlStateNormal];
    NSMutableArray *choices = [NSMutableArray array];
    for (int s = -1; s < count; s++) {
        UIAction *a = [UIAction actionWithTitle:s < 0 ? @"不启用" : [NSString stringWithFormat:@"%d号玩家",s+1] image:nil identifier:nil handler:^(UIAction *action) {
            TOTKTouchPad *pad = weakSelf; if (!pad) return;
            if (s >= 0 && !runtime_input_source_set(s, INPUT_SOURCE_KEYBOARD)) { [pad mappingSaveFailed]; return; }
            if (!runtime_keyboard_bind(s)) { [pad mappingSaveFailed]; return; }
            if (s >= 0) pad->keyboardEditSlot = s; [pad openKeyboardMapping];
        }]; a.state = s == slot ? UIMenuElementStateOn : UIMenuElementStateOff; [choices addObject:a];
    }
    binding.menu = [UIMenu menuWithTitle:@"系统键盘只绑定一个玩家" children:choices]; binding.showsMenuAsPrimaryAction = YES; [mappingCard addSubview:binding];
    NSMutableArray *playerNames=[NSMutableArray array];
    for(int i=0;i<count;i++)[playerNames addObject:[NSString stringWithFormat:@"玩家 %d",i+1]];
    UISegmentedControl *players = [[UISegmentedControl alloc] initWithItems:playerNames];
    players.frame = CGRectMake(16,182,w-32,32); players.selectedSegmentIndex = keyboardEditSlot;
    [players addTarget:self action:@selector(keyboardPlayerChanged:) forControlEvents:UIControlEventValueChanged]; [mappingCard addSubview:players];
    NSArray *actions = @[@"A",@"B",@"X",@"Y",@"L3",@"R3",@"L",@"R",@"ZL",@"ZR",@"+",@"−",@"方向左",@"方向上",@"方向右",@"方向下",@"左摇杆 ←",@"左摇杆 →",@"左摇杆 ↑",@"左摇杆 ↓",@"右摇杆 ←",@"右摇杆 →",@"右摇杆 ↑",@"右摇杆 ↓"];
    NSMutableArray<NSString *> *names = [NSMutableArray arrayWithObject:@"不绑定"];
    NSMutableArray<NSNumber *> *codes = [NSMutableArray arrayWithObject:@0];
    for (int i=0;i<26;i++) { [names addObject:[NSString stringWithFormat:@"%c",'A'+i]]; [codes addObject:@(4+i)]; }
    for (int i=0;i<10;i++) { [names addObject:[NSString stringWithFormat:@"%d",(i+1)%10]]; [codes addObject:@(30+i)]; }
    [names addObjectsFromArray:@[@"Enter",@"Esc",@"Backspace",@"Tab",@"Space",@"←",@"→",@"↑",@"↓",@"左 Shift",@"右 Shift",@"左 Ctrl",@"右 Ctrl",@"左 Alt",@"右 Alt"]];
    [codes addObjectsFromArray:@[@40,@41,@42,@43,@44,@80,@79,@82,@81,@225,@229,@224,@228,@226,@230]];
    int editSlot = keyboardEditSlot;
    for (int a = 0; a < 24; a++) {
        CGFloat y = 224 + a*42;
        UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(16,y,w*.5-16,36)];
        label.text = actions[a]; label.textColor = UIColor.whiteColor; label.font = [UIFont systemFontOfSize:14]; [mappingCard addSubview:label];
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem]; b.frame = CGRectMake(w*.5,y,w*.5-16,36);
        int current = runtime_keyboard_mapping_get(editSlot,a); NSUInteger idx = [codes indexOfObject:@(current)];
        [b setTitle:idx == NSNotFound ? [NSString stringWithFormat:@"HID %d ▾",current] : [names[idx] stringByAppendingString:@" ▾"] forState:UIControlStateNormal];
        NSMutableArray *keys = [NSMutableArray array];
        for (NSUInteger k=0;k<codes.count;k++) {
            int code = codes[k].intValue;
            UIAction *key = [UIAction actionWithTitle:names[k] image:nil identifier:nil handler:^(UIAction *action) {
                TOTKTouchPad *pad = weakSelf; if (!pad) return;
                if (!runtime_keyboard_mapping_set(editSlot,a,code)) { [pad mappingSaveFailed]; return; }
                CGPoint offset = pad->mappingCard.contentOffset; [pad openKeyboardMapping]; pad->mappingCard.contentOffset = offset;
            }]; key.state = code == current ? UIMenuElementStateOn : UIMenuElementStateOff; [keys addObject:key];
        }
        b.menu = [UIMenu menuWithTitle:@"选择键盘按键" children:keys]; b.showsMenuAsPrimaryAction = YES; [mappingCard addSubview:b];
    }
    UIButton *reset = [UIButton buttonWithType:UIButtonTypeSystem]; reset.frame = CGRectMake(16,1240,w-32,40);
    [reset setTitle:@"恢复此玩家的键盘默认映射…" forState:UIControlStateNormal];
    reset.menu = [UIMenu menuWithTitle:@"不会修改手柄或其他玩家" children:@[[UIAction actionWithTitle:@"确认恢复默认" image:nil identifier:nil handler:^(UIAction *a) {
        TOTKTouchPad *pad = weakSelf; if (!pad) return;
        if (runtime_keyboard_mapping_reset(editSlot)) [pad openKeyboardMapping]; else [pad mappingSaveFailed];
    }]]]; reset.showsMenuAsPrimaryAction = YES; [mappingCard addSubview:reset]; mappingCard.contentSize = CGSizeMake(w,1300);
}
- (void)keyboardPlayerChanged:(UISegmentedControl *)sender {
    keyboardEditSlot = (int)sender.selectedSegmentIndex; [self openKeyboardMapping];
}
