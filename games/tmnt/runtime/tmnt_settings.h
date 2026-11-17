/* Native host settings. Changes to the sync folder take effect on the next launch. */
void tmnt_frame_rate_set(int rate);
int tmnt_frame_rate_get(void);
int runtime_gamepad_type(int *detected);
int runtime_gamepad_type_cycle(void);
int runtime_mouse_enabled(void);
int runtime_mouse_toggle(void);
static NSPanel *tmntSettingsPanel;
static NSMutableDictionary *tmntPreferences;
static NSTextField *tmntSyncStatus;

static NSString *tmnt_text(NSString *cn, NSString *tw, NSString *en) {
    const char *override = getenv("TOTK_UI_LANG");
    NSString *lang = override ? @(override) : NSLocale.preferredLanguages.firstObject;
    if (![lang hasPrefix:@"zh"]) return en;
    return [lang containsString:@"Hant"] || [lang containsString:@"TW"] || [lang containsString:@"HK"] ? tw : cn;
}
#define TMNT_TEXT(cn,tw,en) tmnt_text(@cn,@tw,@en)
static NSString *tmnt_preferences_path(void) {
    const char *profile = getenv("TOTK_PROFILE_DIR");
    return profile && *profile ? [@(profile) stringByAppendingPathComponent:@"host-settings.plist"] : nil;
}
static void tmnt_preferences_save(void) {
    NSString *path = tmnt_preferences_path();
    if (path && ![tmntPreferences writeToFile:path atomically:YES])
        fprintf(stderr,"[tmnt-settings] could not save host preferences\n");
}
static void tmnt_sync_status(void) {
    NSString *folder = tmntPreferences[@"syncFolder"];
    tmntSyncStatus.stringValue = folder.length ? [folder stringByAbbreviatingWithTildeInPath] :
        TMNT_TEXT("未选择同步文件夹", "未選擇同步資料夾", "No sync folder selected");
    tmntSyncStatus.toolTip = folder;
}
void runtime_front_notice(const char *title, const char *detail) {
    NSString *t = @(title), *d = @(detail);
    dispatch_async(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new]; a.messageText=t; a.informativeText=d;
        [a beginSheetModalForWindow:window completionHandler:nil];
    });
}
@interface TMNTSettingsController : NSObject
- (void)change:(NSPopUpButton *)sender;
- (void)chooseSync:(id)sender;
- (void)stopSync:(id)sender;
- (void)fullscreen:(id)sender;
- (void)openSaves:(id)sender;
@end
@implementation TMNTSettingsController
- (void)change:(NSPopUpButton *)sender {
    NSInteger value = sender.selectedItem.tag;
    NSString *key = sender.identifier;
    tmntPreferences[key] = @(value);
    if ([key isEqualToString:@"frameRate"]) tmnt_frame_rate_set((int)value);
    if ([key isEqualToString:@"controller"]) {
        int detected;
        for (int i = 0; i < 4 && runtime_gamepad_type(&detected) != value; i++)
            runtime_gamepad_type_cycle();
    }
    if ([key isEqualToString:@"mouse"] && runtime_mouse_enabled() != value) runtime_mouse_toggle();
    if ([key isEqualToString:@"windowSize"] && !(window.styleMask & NSWindowStyleMaskFullScreen))
        [window setContentSize:NSMakeSize(value, value * 9.0 / 16.0)];
    tmnt_preferences_save();
}
- (void)chooseSync:(id)sender {
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseFiles=NO; p.canChooseDirectories=YES; p.canCreateDirectories=YES; p.allowsMultipleSelection=NO;
    p.message=TMNT_TEXT("选择 iCloud 云盘中的同步文件夹。保留本机存档，下次启动时同步。", "選擇 iCloud 雲碟中的同步資料夾。保留本機存檔，下次啟動時同步。", "Choose a sync folder in iCloud Drive. Local saves are retained. Sync starts next launch.");
    NSString *folder=tmntPreferences[@"syncFolder"];
    if (!folder.length) folder=[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Mobile Documents/com~apple~CloudDocs"];
    p.directoryURL=[NSURL fileURLWithPath:folder isDirectory:YES];
    [p beginSheetModalForWindow:tmntSettingsPanel completionHandler:^(NSModalResponse result) {
        if(result!=NSModalResponseOK || !p.URL)return;
        tmntPreferences[@"syncFolder"]=p.URL.path;
        tmnt_preferences_save(); tmnt_sync_status();
    }];
}
- (void)stopSync:(id)sender { (void)sender; [tmntPreferences removeObjectForKey:@"syncFolder"]; tmnt_preferences_save(); tmnt_sync_status(); }
- (void)fullscreen:(id)sender { (void)sender; [tmntSettingsPanel orderOut:nil]; [window makeKeyAndOrderFront:nil]; [window toggleFullScreen:nil]; }
- (void)openSaves:(id)sender { (void)sender; const char *p=getenv("TOTK_PROFILE_DIR"); if(p) [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:[@(p) stringByAppendingPathComponent:@"save"] isDirectory:YES]]; }
@end
static TMNTSettingsController *tmntSettingsController;
static NSButton *tmnt_button(NSString *title, SEL action) {
    return [NSButton buttonWithTitle:title target:tmntSettingsController action:action];
}
static void tmnt_settings_row(NSStackView *stack, NSString *label, NSString *key,
                              NSArray<NSString *> *titles, NSArray<NSNumber *> *values, NSInteger selected) {
    NSTextField *text = [NSTextField labelWithString:label];
    [text.widthAnchor constraintEqualToConstant:140].active = YES;
    NSPopUpButton *choice = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    [choice addItemsWithTitles:titles];
    for (NSUInteger i=0;i<values.count;i++) choice.itemArray[i].tag=values[i].integerValue;
    [choice selectItemWithTag:selected]; choice.identifier=key;
    choice.target=tmntSettingsController; choice.action=@selector(change:);
    [choice.widthAnchor constraintEqualToConstant:300].active=YES;
    NSStackView *row=[NSStackView stackViewWithViews:@[text,choice]]; row.spacing=16;
    [stack addArrangedSubview:row];
}
static void tmnt_settings_load(void) {
    NSString *path=tmnt_preferences_path();
    tmntPreferences=path ? [[NSDictionary dictionaryWithContentsOfFile:path] mutableCopy] : nil;
    if(!tmntPreferences)tmntPreferences=[NSMutableDictionary dictionary];
    NSDictionary *allowed=@{@"frameRate":@[@0,@30,@60,@120],@"controller":@[@0,@1,@2,@3],@"mouse":@[@0,@1],@"windowSize":@[@1280,@1600,@1920]};
    for(NSString *key in allowed) if(tmntPreferences[key] && ![allowed[key] containsObject:tmntPreferences[key]]) [tmntPreferences removeObjectForKey:key];
    id folder=tmntPreferences[@"syncFolder"];
    if(![folder isKindOfClass:NSString.class] || ![folder isAbsolutePath]) [tmntPreferences removeObjectForKey:@"syncFolder"];
    tmnt_frame_rate_set([tmntPreferences[@"frameRate"] intValue]);
    /* Explicit environment controls are used by isolated tests and override preferences. */
    folder=tmntPreferences[@"syncFolder"];
    if(folder && !getenv("TOTK_CLOUD_SAVE_FOLDER")) setenv("TOTK_CLOUD_SAVE_FOLDER",[folder fileSystemRepresentation],0);
}
static void tmnt_settings_restore_window(void) {
    NSInteger width=[tmntPreferences[@"windowSize"] integerValue];
    if(width) [window setContentSize:NSMakeSize(width,width*9.0/16.0)];
    if(tmntPreferences[@"mouse"] && runtime_mouse_enabled()!=[tmntPreferences[@"mouse"] intValue]) runtime_mouse_toggle();
}
static void tmnt_settings_toggle(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if(tmntSettingsPanel.isVisible) { [tmntSettingsPanel orderOut:nil]; [window makeKeyAndOrderFront:nil]; return; }
        if(!tmntSettingsController)tmntSettingsController=[TMNTSettingsController new];
        tmntSettingsPanel=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,520,410)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
        tmntSettingsPanel.title=TMNT_TEXT("忍者神龟 · 设置","忍者龜 · 設定","TMNT · Settings");
        tmntSettingsPanel.releasedWhenClosed=NO;
        tmntSettingsPanel.collectionBehavior=NSWindowCollectionBehaviorFullScreenAuxiliary;
        NSStackView *stack=[NSStackView new]; stack.orientation=NSUserInterfaceLayoutOrientationVertical;
        stack.alignment=NSLayoutAttributeLeading; stack.spacing=18; stack.translatesAutoresizingMaskIntoConstraints=NO;
        [tmntSettingsPanel.contentView addSubview:stack];
        [stack.leadingAnchor constraintEqualToAnchor:tmntSettingsPanel.contentView.leadingAnchor constant:24].active=YES;
        [stack.topAnchor constraintEqualToAnchor:tmntSettingsPanel.contentView.topAnchor constant:24].active=YES;
        tmnt_settings_row(stack,TMNT_TEXT("目标帧率","目標幀率","Frame rate"),@"frameRate",
            @[TMNT_TEXT("跟随游戏","跟隨遊戲","Follow game"),@"30 FPS",@"60 FPS",TMNT_TEXT("120 FPS（实验）","120 FPS（實驗）","120 FPS (experimental)")],@[@0,@30,@60,@120],tmnt_frame_rate_get());
        int detected;
        tmnt_settings_row(stack,TMNT_TEXT("手柄布局","控制器配置","Controller layout"),@"controller",
            @[TMNT_TEXT("自动","自動","Automatic"),@"Xbox",@"PlayStation",@"Switch"],@[@0,@1,@2,@3],runtime_gamepad_type(&detected));
        tmnt_settings_row(stack,TMNT_TEXT("鼠标控制","滑鼠控制","Mouse control"),@"mouse",
            @[TMNT_TEXT("关闭","關閉","Off"),TMNT_TEXT("开启","開啟","On")],@[@0,@1],runtime_mouse_enabled());
        tmnt_settings_row(stack,TMNT_TEXT("窗口大小","視窗大小","Window size"),@"windowSize",
            @[@"1280 × 720",@"1600 × 900",@"1920 × 1080"],@[@1280,@1600,@1920],[tmntPreferences[@"windowSize"] integerValue]?:1280);
        [stack addArrangedSubview:tmnt_button(TMNT_TEXT("切换全屏","切換全螢幕","Toggle full screen"),@selector(fullscreen:))];
        tmntSyncStatus=[NSTextField labelWithString:@""]; tmntSyncStatus.lineBreakMode=NSLineBreakByTruncatingMiddle;
        [tmntSyncStatus.widthAnchor constraintEqualToConstant:460].active=YES; tmnt_sync_status();
        [stack addArrangedSubview:tmntSyncStatus];
        NSStackView *buttons=[NSStackView stackViewWithViews:@[
            tmnt_button(TMNT_TEXT("选择 iCloud 文件夹…","選擇 iCloud 資料夾…","Choose iCloud folder…"),@selector(chooseSync:)),
            tmnt_button(TMNT_TEXT("停止同步","停止同步","Stop sync"),@selector(stopSync:)),
            tmnt_button(TMNT_TEXT("本机存档","本機存檔","Local saves"),@selector(openSaves:))]];
        buttons.spacing=8; [stack addArrangedSubview:buttons];
        NSTextField *note=[NSTextField wrappingLabelWithString:TMNT_TEXT("同步设置下次启动生效。数字 1 或 Esc 关闭面板。","同步設定下次啟動生效。數字 1 或 Esc 關閉面板。","Sync changes apply next launch. Press 1 or Esc to close.")];
        [note.widthAnchor constraintEqualToConstant:460].active=YES; note.textColor=NSColor.secondaryLabelColor;
        [stack addArrangedSubview:note];
        [tmntSettingsPanel center]; [tmntSettingsPanel makeKeyAndOrderFront:nil]; runtime_input_clear();
    });
}
