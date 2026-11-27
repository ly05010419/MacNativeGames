#include "input_diagram_macos.h"
/* Native per-player settings. Included by frontend.m; all UI/config runs on main. */
@interface D3InputSettings : NSWindowController <NSWindowDelegate>
@property int port;
@property int page;
@property(strong) NSTextField *status;
- (void)open:(id)sender;
- (void)populateKey:(NSPopUpButton *)value action:(int)action;
@end
@implementation D3InputSettings
- (instancetype)init {
    NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,1100,830)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
    if ((self = [super initWithWindow:panel])) {
        panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
        panel.hidesOnDeactivate = NO;
        panel.title = @"玩家输入设置"; panel.releasedWhenClosed = NO; panel.delegate = self;
    }
    return self;
}
- (void)open:(id)sender {
    (void)sender;
    runtime_input_settings_suspend(1); front_mouse_update();
    [self rebuild]; [self.window center]; [self.window makeKeyAndOrderFront:nil];
}
- (void)windowWillClose:(NSNotification *)n {
    (void)n; runtime_input_settings_suspend(0);
    [window makeKeyAndOrderFront:nil];
}
- (void)label:(NSString *)text frame:(NSRect)frame view:(NSView *)view {
    NSTextField *label = [NSTextField wrappingLabelWithString:text]; label.frame = frame;
    [view addSubview:label];
}
- (NSPopUpButton *)popup:(NSArray<NSString *> *)items frame:(NSRect)frame action:(SEL)action view:(NSView *)view {
    NSPopUpButton *popup = [[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];
    [popup addItemsWithTitles:items]; popup.target = self; popup.action = action;
    [view addSubview:popup]; return popup;
}
- (void)result:(int)ok {
    if (ok) [self rebuild];
    else { self.status.stringValue = @"未更改：保存失败、设备离线/已绑定其他玩家，或后端不支持。"; self.status.textColor = NSColor.systemRedColor; }
}
/* Change the source first: if the later device save fails, unrelated input is
   still blocked. Report partial success honestly; these are separate files. */
- (BOOL)selectSource:(int)source slot:(int)slot {
    if (runtime_input_source_set(slot, source)) return YES;
    self.status.stringValue = @"输入来源未更改：无法保存 input-sources.plist，请检查配置目录权限。";
    self.status.textColor = NSColor.systemRedColor; return NO;
}
- (void)joinHint {
    if (self.page && !runtime_input_source_allows(self.port, INPUT_DEVICE_KEYBOARD)) return;
    if (self.port > 0 && runtime_input_source(self.port) != 5)
        self.status.stringValue = @"该玩家输入已启用。关闭设置，回到游戏的本地合作/角色选择界面，使用该玩家设备按游戏提示加入；此设置不会自动创建角色。";
}
- (void)sourceChanged:(NSPopUpButton *)sender {
    if (![self selectSource:(int)sender.indexOfSelectedItem slot:self.port]) return;
    int source = (int)sender.indexOfSelectedItem;
    if ((source == 2 || source == 3) && !runtime_keyboard_physical_enabled() &&
        !runtime_keyboard_bind(self.port)) {
        self.status.stringValue = @"输入来源已保存，但系统键盘归属保存失败，原归属未更改。请在系统键盘页重新绑定。";
        self.status.textColor = NSColor.systemRedColor; return;
    }
    [self rebuild]; [self joinHint];
    if ((source == 2 || source == 3) && runtime_keyboard_physical_enabled())
        self.status.stringValue = @"独立键盘已启用：请在系统键盘页为此玩家选择物理键盘。";
    front_mouse_update();
}
- (void)deviceChanged:(NSPopUpButton *)sender {
    NSString *key = sender.selectedItem.representedObject;
    if (runtime_gamepad_slot_type(0) == 4) {
        self.status.stringValue = @"当前是旧版全局第三方 Pro 后端。请先在玩家 1 将手柄类型改为自动识别，再选择实体手柄。"; return;
    }
    if (![self selectSource:[key isEqual:@"none"] ? 5 : 1 slot:self.port]) return;
    if (!runtime_gamepad_device_bind(self.port, key)) {
        self.status.stringValue = @"输入来源已切换，但设备绑定未保存：设备可能离线、已被其他玩家预留，或配置文件不可写。旧绑定仍保留，请刷新设备列表。";
        self.status.textColor = NSColor.systemRedColor; return;
    }
    [self rebuild]; [self joinHint];
}
- (void)physicalEnable:(NSButton *)sender {
    (void)sender;
    int ok = runtime_keyboard_physical_enable(!runtime_keyboard_physical_enabled());
    [self rebuild];
    self.status.stringValue = runtime_keyboard_physical_status();
    if (!ok) self.status.textColor = NSColor.systemRedColor;
}
- (void)physicalChanged:(NSPopUpButton *)sender {
    NSString *key = sender.selectedItem.representedObject;
    if (![self selectSource:[key isEqual:@"none"] ? 5 : 2 slot:self.port]) return;
    if (!runtime_keyboard_physical_bind(self.port, key)) {
        self.status.stringValue = @"输入来源已切换，但独立键盘绑定未保存：请检查设备在线状态、重复绑定和配置目录权限。"; return;
    }
    [self rebuild]; [self joinHint];
}
- (void)refreshDevices:(id)sender { (void)sender; [self rebuild]; }
- (void)player:(NSPopUpButton *)sender { self.port = (int)sender.indexOfSelectedItem; [self rebuild]; }
- (void)pageChanged:(NSPopUpButton *)sender { self.page = (int)sender.indexOfSelectedItem; [self rebuild]; }
- (void)typeChanged:(NSPopUpButton *)sender {
    int type = (int)sender.indexOfSelectedItem;
    if (type == 4) {
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"启用第三方 Pro 全局兼容模式？";
        alert.informativeText = @"此旧后端会全局替代 GameController，不是当前玩家的独立类型。Xbox / PlayStation 等手柄可能不可用；启用手柄输入的玩家按原始设备顺序分配。原来的独立设备绑定会保留但暂不使用（不使用实体手柄仍有效）。要退出，请在玩家 1 选择自动识别或其他类型。";
        [alert addButtonWithTitle:@"启用全局兼容模式"]; [alert addButtonWithTitle:@"取消"];
        [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
            if (response != NSAlertFirstButtonReturn) { [self rebuild]; return; }
            if (runtime_gamepad_slot_type_set(0,4)) {
                self.port = 0; [self rebuild];
                self.status.stringValue = @"第三方 Pro 全局兼容模式已启用；在玩家 1 更改类型即可退出。原独立设备绑定保留。";
            } else [self result:0];
        }];
        return;
    }
    if (runtime_gamepad_slot_type_set(self.port,type)) [self rebuild];
    else { self.status.stringValue = @"手柄类型未保存：controller-slots.plist 无法写入。"; self.status.textColor = NSColor.systemRedColor; }
}
- (void)buttonChanged:(NSPopUpButton *)sender {
    [self result:runtime_input_binding_set(self.port, self.page==2, (int)sender.tag, [sender.selectedItem.representedObject intValue])];
}
- (void)captureKey:(MNGKeyBindingButton *)sender {
    sender.waiting=YES; sender.title=@"请按键… Esc 取消";
    [self.window makeFirstResponder:sender];
}
- (void)stickChanged:(NSPopUpButton *)sender {
    [self result:runtime_input_stick_set(self.port,(int)sender.tag,(int)sender.indexOfSelectedItem)];
}
- (void)mouseStickChanged:(NSPopUpButton *)sender {
    [self result:runtime_input_mouse_stick_set((int)sender.indexOfSelectedItem)];
}
- (void)gyroChanged:(NSPopUpButton *)sender {
    [self result:runtime_input_gyro_set(self.port,(int)sender.indexOfSelectedItem)];
}
- (void)keyChanged:(NSPopUpButton *)sender {
    [self result:runtime_keyboard_mapping_set(self.port, (int)sender.tag, [sender.selectedItem.representedObject intValue])];
}
- (void)mouseChanged:(NSButton *)sender { (void)sender; runtime_mouse_toggle(); [self rebuild]; }
- (void)bindChanged:(NSPopUpButton *)sender {
    int slot = (int)sender.indexOfSelectedItem - 1;
    if (slot >= 0 && ![self selectSource:2 slot:slot]) return;
    if (!runtime_keyboard_bind(slot)) {
        self.status.stringValue = slot >= 0 ? @"已切换为仅键盘，但键盘归属保存失败。请检查 keyboard-slots.plist 权限；原归属未更改。" : @"键盘归属未更改：keyboard-slots.plist 保存失败。"; return;
    }
    if (slot >= 0) self.port = slot;
    [self rebuild]; [self joinHint];
}
- (void)reset:(id)sender {
    (void)sender;
    NSAlert *alert = [NSAlert new]; alert.messageText = @"恢复当前玩家的默认映射？";
    alert.informativeText = self.page == 1 ? @"只重置此玩家的键盘映射，不改变键盘绑定或其他玩家。" :
        @"只重置当前输入页的按键绑定；摇杆、陀螺仪、其他设备和玩家保持不变。";
    [alert addButtonWithTitle:@"恢复默认"]; [alert addButtonWithTitle:@"取消"];
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        if (response == NSAlertFirstButtonReturn)
            [self result:self.page == 1 ? runtime_keyboard_mapping_reset(self.port) : runtime_input_binding_reset(self.port,self.page==2)];
    }];
}
- (void)rebuild {
    NSView *root = [[MNGInputBackground alloc] initWithFrame:NSMakeRect(0,0,1100,830)]; self.window.contentView = root;
    NSMutableArray *players = [NSMutableArray array];
    for (int s = 0; s < runtime_gamepad_slot_count(); ++s) [players addObject:[NSString stringWithFormat:@"玩家 %d",s+1]];
    if (self.port >= (int)players.count) self.port = 0;
    NSPopUpButton *port = [self popup:players frame:NSMakeRect(20,782,160,30) action:@selector(player:) view:root];
    [port selectItemAtIndex:self.port];
    NSPopUpButton *page = [self popup:@[@"手柄绑定",@"键盘绑定",@"鼠标绑定"] frame:NSMakeRect(194,782,250,30) action:@selector(pageChanged:) view:root];
    [page selectItemAtIndex:self.page];
    NSPopUpButton *source = [self popup:@[@"兼容：混合输入",@"仅手柄",@"仅键盘",@"键盘＋鼠标（1P）",@"仅触屏（iOS）",@"关闭此玩家输入"]
        frame:NSMakeRect(460,782,300,30) action:@selector(sourceChanged:) view:root];
    [source selectItemAtIndex:runtime_input_source(self.port)];
    source.autoenablesItems = NO; [source itemAtIndex:4].enabled = NO;
    [source itemAtIndex:3].enabled = self.port == 0;
    [self label:@"以 NS Pro 功能为目标，选择实际输入。编辑映射不会切换输入来源；选择绑定设备会切换来源。不同输入要同时生效，请选择混合输入或键盘＋鼠标。"
        frame:NSMakeRect(20,730,1060,44) view:root];
    self.status = [NSTextField wrappingLabelWithString:@"更改会立即保存。多人游戏启用额外玩家后，请回到游戏按提示加入；不会自动创建角色。"];
    self.status.frame = NSMakeRect(20,8,1060,42); [root addSubview:self.status];
    NSArray *buttons = @[@"不绑定",@"A",@"B",@"X",@"Y",@"L3",@"R3",@"L",@"R",@"ZL",@"ZR",@"+",@"−",@"方向左",@"方向上",@"方向右",@"方向下"];
    if (self.page == 0) {
        NSPopUpButton *type = [self popup:@[@"自动识别",@"Xbox",@"PlayStation",@"Nintendo Switch",@"第三方 Pro（全局兼容模式…）"]
            frame:NSMakeRect(20,690,330,30) action:@selector(typeChanged:) view:root];
        [type selectItemAtIndex:runtime_gamepad_slot_type(self.port)];
        type.autoenablesItems = NO;
    } else if (self.page == 1) {
        NSMutableArray *bindings = [NSMutableArray arrayWithObject:@"使用旧键盘布局（玩家 1）"];
        for (NSUInteger slot = 0; slot < players.count; ++slot)
            [bindings addObject:[NSString stringWithFormat:@"系统键盘 → %@%@", players[slot],
                runtime_input_source_allows((int)slot, INPUT_DEVICE_KEYBOARD) ? @"" : @"（来源未启用）"]];
        NSPopUpButton *bind = [self popup:bindings frame:NSMakeRect(20,690,330,30) action:@selector(bindChanged:) view:root];
        [bind selectItemAtIndex:runtime_keyboard_slot()+1];
        bind.enabled = !runtime_keyboard_physical_enabled();
    }
    NSButton *reset = [NSButton buttonWithTitle:@"恢复默认…" target:self action:@selector(reset:)];
    NSButton *mouse = [NSButton checkboxWithTitle:@"鼠标模式" target:self action:@selector(mouseChanged:)];
    mouse.frame = NSMakeRect(355,690,145,30); mouse.state = runtime_mouse_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    [root addSubview:mouse];
    reset.frame = NSMakeRect(910,690,170,30); [root addSubview:reset];
    NSPopUpButton *device = [self popup:@[@"自动分配手柄",@"不使用实体手柄"]
        frame:NSMakeRect(20,650,510,30) action:@selector(deviceChanged:) view:root];
    [device itemAtIndex:0].representedObject = @"auto"; [device itemAtIndex:1].representedObject = @"none";
    NSString *currentDevice = runtime_gamepad_device_binding(self.port);
    BOOL selected = NO;
    for (NSDictionary *entry in runtime_gamepad_devices()) {
        [device addItemWithTitle:entry[@"name"]]; device.lastItem.representedObject = entry[@"id"];
    }
    for (NSMenuItem *item in device.itemArray)
        if ([item.representedObject isEqual:currentDevice]) { [device selectItem:item]; selected = YES; break; }
    if (!selected) {
        [device addItemWithTitle:runtime_gamepad_device_label(self.port)];
        device.lastItem.representedObject = currentDevice; [device selectItem:device.lastItem];
        device.lastItem.enabled = NO;
    }
    device.hidden = self.page != 0;
    device.autoenablesItems = NO;
    if (self.page == 0 && runtime_gamepad_slot_type(0) == 4) {
        device.enabled = NO;
        self.status.stringValue = @"当前：第三方 Pro 全局兼容模式。独立手柄绑定暂不使用；在玩家 1 更改手柄类型即可退出。";
    }
    NSButton *refresh = [NSButton buttonWithTitle:@"刷新设备" target:self action:@selector(refreshDevices:)];
    refresh.frame = NSMakeRect(540,650,140,30);refresh.hidden = self.page != 0; [root addSubview:refresh];
    if (self.page == 1) {
        NSButton *enable = [NSButton buttonWithTitle:runtime_keyboard_physical_enabled() ? @"关闭独立键盘" : @"启用独立键盘…"
            target:self action:@selector(physicalEnable:)];
        enable.frame = NSMakeRect(20,650,170,30); [root addSubview:enable];
        NSPopUpButton *physical = [self popup:@[@"不绑定物理键盘"] frame:NSMakeRect(194,650,570,30)
            action:@selector(physicalChanged:) view:root];
        [physical itemAtIndex:0].representedObject = @"none";
        for (NSDictionary *entry in runtime_keyboard_physical_devices()) {
            [physical addItemWithTitle:entry[@"name"]]; physical.lastItem.representedObject = entry[@"id"];
        }
        NSString *selectedKey = runtime_keyboard_physical_binding(self.port); BOOL found = NO;
        for (NSMenuItem *item in physical.itemArray)
            if ([item.representedObject isEqual:selectedKey]) { [physical selectItem:item]; found = YES; break; }
        if (!found) {
            [physical addItemWithTitle:runtime_keyboard_physical_label(self.port)];
            physical.lastItem.representedObject = selectedKey; physical.lastItem.enabled = NO;
            [physical selectItem:physical.lastItem];
        }
        physical.autoenablesItems = NO; physical.enabled = runtime_keyboard_physical_enabled();
        self.status.stringValue = runtime_input_source_allows(self.port, INPUT_DEVICE_KEYBOARD)
            ? runtime_keyboard_physical_status()
            : @"当前玩家未启用键盘来源：键盘绑定和键位仅保留，不参与输入。切回键盘来源后恢复；窗口快捷键不受影响。";
    }
    if (self.page == 2 && self.port != 0)
        self.status.stringValue = @"鼠标仅支持玩家 1；当前玩家的鼠标绑定不可编辑。";
    MNGInputDiagram *diagram=[[MNGInputDiagram alloc] initWithFrame:NSMakeRect(350,228,400,310)];
    diagram.highlighted=-1; [root addSubview:diagram];
    __weak D3InputSettings *weakOwner=self;
    diagram.selected=^(int target){
        D3InputSettings *owner=weakOwner;
        for(NSView *view in owner.window.contentView.subviews)if([view isKindOfClass:NSPopUpButton.class]) {
            NSPopUpButton *binding=(id)view;
            if(binding.tag==target&&(binding.action==@selector(keyChanged:)||binding.action==@selector(buttonChanged:))) {
                [owner.window makeFirstResponder:binding];break;
            }
        }
    };
    NSArray *physical = @[@"下方键（Xbox A / NS B）",@"右方键（Xbox B / NS A）",@"左方键（Xbox X / NS Y）",@"上方键（Xbox Y / NS X）",
        @"左摇杆按下",@"右摇杆按下",@"L / LB",@"R / RB",@"ZL / LT",@"ZR / RT",@"+ / Menu",@"− / View",
        @"方向左",@"方向上",@"方向右",@"方向下",@"触摸板按下",@"鼠标左键",@"鼠标右键",@"鼠标中键",@"鼠标侧键 1",@"鼠标侧键 2"];
    const int left[]={8,6,11,4,13,12,15,14},right[]={9,7,10,2,0,1,3,5};
    for(int side=0;side<2;side++)for(int position=0;position<8;position++) {
        int row=side?right[position]:left[position]; CGFloat x=side?770:20,y=568-position*51;
        [self label:buttons[row+1] frame:NSMakeRect(x,y+2,45,28) view:root];
        NSPopUpButton *value=[self popup:@[] frame:NSMakeRect(x+48,y,252,30)
            action:self.page==1?@selector(keyChanged:):@selector(buttonChanged:) view:root];
        value.tag=row;
        if(self.page==1) [self populateKey:value action:row];
        else {
            [value addItemWithTitle:@"不绑定"];value.lastItem.representedObject=@(-1);
            int current=runtime_input_binding_get(self.port,self.page==2,row);
            int first=self.page==2?17:0,end=self.page==2?22:17;
            for(int i=first;i<end;i++) {
                [value addItemWithTitle:physical[i]];value.lastItem.representedObject=@(i);
                if(i==current)[value selectItem:value.lastItem];
            }
            if(current==-2) { [value addItemWithTitle:@"多个输入（保留；选择后替换）"];value.lastItem.representedObject=@(-2);[value selectItem:value.lastItem];value.lastItem.enabled=NO; }
            value.autoenablesItems=NO;value.enabled=self.page!=2||self.port==0;
            value.toolTip=@"选择已使用的实体键会将它改绑到此 NS 功能；其他设备页不受影响。";
        }
    }
    [self label:@"左摇杆" frame:NSMakeRect(20,133,100,24) view:root];
    [self label:@"右摇杆" frame:NSMakeRect(550,133,100,24) view:root];
    if(self.page==1) {
        NSArray *directions=@[@"←",@"→",@"↑",@"↓"];
        for(int stick=0;stick<2;stick++)for(int dir=0;dir<4;dir++) {
            CGFloat x=20+stick*530+dir*130;
            [self label:directions[dir] frame:NSMakeRect(x,105,20,24) view:root];
            NSPopUpButton *key=[self popup:@[] frame:NSMakeRect(x+20,100,108,30) action:@selector(keyChanged:) view:root];
            key.tag=16+stick*4+dir;[self populateKey:key action:(int)key.tag];
        }
    } else {
        if(self.page==0)for(int stick=0;stick<2;stick++) {
            NSPopUpButton *value=[self popup:@[@"不绑定",@"实体左摇杆",@"实体右摇杆"]
                frame:NSMakeRect(20+stick*530,100,350,30) action:@selector(stickChanged:) view:root];
            value.tag=stick;[value selectItemAtIndex:runtime_input_stick_get(self.port,stick)];
        } else {
            [self label:@"鼠标移动映射到：" frame:NSMakeRect(20,100,170,28) view:root];
            NSPopUpButton *value=[self popup:@[@"不控制摇杆",@"NS 左摇杆",@"NS 右摇杆",@"NS 左右摇杆"]
                frame:NSMakeRect(190,100,330,30) action:@selector(mouseStickChanged:) view:root];
            [value selectItemAtIndex:runtime_input_mouse_stick_get()];value.enabled=self.port==0;
            [self label:@"需开启鼠标模式；与键盘绑定独立保存。" frame:NSMakeRect(550,100,530,28) view:root];
        }
    }
    [self label:@"陀螺仪来源" frame:NSMakeRect(350,228,110,25) view:root];
    NSPopUpButton *gyro=[self popup:@[@"自动：鼠标模式优先",@"已绑定手柄的六轴",@"鼠标模拟六轴（1P）",@"关闭六轴"]
        frame:NSMakeRect(450,224,300,30) action:@selector(gyroChanged:) view:root];
    gyro.enabled = self.port < INPUT_MOTION_PLAYERS;
    [gyro selectItemAtIndex:runtime_input_gyro_get(self.port)];gyro.autoenablesItems=NO;[gyro itemAtIndex:2].enabled=self.port==0;
    [self label:@"六轴仍受输入来源和游戏支持限制；灰色表示该玩家不支持六轴；无传感器的手柄不能提供体感。数字窗口快捷键优先，Escape 可从下拉列表绑定。"
        frame:NSMakeRect(20,51,1060,40) view:root];
}
- (void)populateKey:(NSPopUpButton *)value action:(int)action {
    NSDictionary *names=@{@0:@"不绑定",@40:@"Enter",@41:@"Escape",@42:@"Backspace",@43:@"Tab",@44:@"Space",
        @79:@"→",@80:@"←",@81:@"↓",@82:@"↑",@224:@"左 Ctrl",@225:@"左 Shift",@226:@"左 Alt",@227:@"左 Command",
        @228:@"右 Ctrl",@229:@"右 Shift",@230:@"右 Alt",@231:@"右 Command"};
    int current=runtime_keyboard_mapping_get(self.port,action);
    for(int key=0;key<256;key++) {
        NSString *name=names[@(key)];
        if(key>=4&&key<=29)name=[NSString stringWithFormat:@"%c",'A'+key-4];
        if(key>=30&&key<=39)name=[NSString stringWithFormat:@"%d（快捷键优先）",(key-29)%10];
        if(key>=58&&key<=69)name=[NSString stringWithFormat:@"F%d",key-57];
        if(!name&&key!=current)continue;
        [value addItemWithTitle:name?:[NSString stringWithFormat:@"USB HID %d",key]];
        value.lastItem.representedObject=@(key);if(key==current)[value selectItem:value.lastItem];
    }
    if(action>=16)return;
    NSRect frame=value.frame;frame.size.width-=76;value.frame=frame;
    MNGKeyBindingButton *capture=[[MNGKeyBindingButton alloc] initWithFrame:NSMakeRect(NSMaxX(frame)+2,frame.origin.y,74,30)];
    capture.bezelStyle=NSBezelStyleRounded;capture.title=@"录入…";capture.idleTitle=capture.title;
    capture.target=self;capture.action=@selector(captureKey:);capture.tag=action;
    int slot=self.port;__weak D3InputSettings *weakSelf=self;
    capture.captured=^(unsigned code){
        D3InputSettings *owner=weakSelf;if(!owner)return;
        int usage=keyboard_macos_usage(code);if(usage<=0)return;
        [owner result:runtime_keyboard_mapping_set(slot,action,usage)];
    };
    [value.superview addSubview:capture];
}
@end
static D3InputSettings *inputSettingsWindow;
