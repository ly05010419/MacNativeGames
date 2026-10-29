#import "progress_sync.h"
#import "progress_errors.h"
#include <TargetConditionals.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

static NSString *versionTitle(NSDictionary *record) {
    NSDate *date=[NSDate dateWithTimeIntervalSince1970:[record[@"created"] doubleValue]];
    NSDateFormatter *formatter=[NSDateFormatter new];formatter.dateStyle=NSDateFormatterShortStyle;formatter.timeStyle=NSDateFormatterMediumStyle;
    NSString *kind=[record[@"kind"] isEqual:@"restore"]?@(TOTK_UI("恢复记录", "Restored progress")):[record[@"kind"] isEqual:@"choice"]?@(TOTK_UI("已选择进度", "Selected progress")):@(TOTK_UI("游戏内存档", "In-game save"));
    return [NSString stringWithFormat:@(TOTK_UI("记录于 %@ · %@", "Recorded %@ · %@")),[formatter stringFromDate:date],kind];
}
static NSString *versionDetail(NSDictionary *record) {
    NSString *device=record[@"deviceName"]?:record[@"device"];
    if ([[NSUUID alloc] initWithUUIDString:device]) device=@(TOTK_UI("其他设备", "Other device"));
    return [NSString stringWithFormat:@(TOTK_UI("%@ · 游戏 %@ · %@", "%@ · Game %@ · %@")),device?:@(TOTK_UI("设备", "Device")),record[@"version"]?:@"—",[NSByteCountFormatter stringFromByteCount:[record[@"bytes"] longLongValue] countStyle:NSByteCountFormatterCountStyleFile]];
}
static NSString *statusText(NSDictionary *state) {
    NSMutableArray *lines=[NSMutableArray arrayWithObject:state[@"status"]?:@(TOTK_UI("正在准备进度列表…", "Preparing progress history…"))];
    if(state[@"enabled"]&&![state[@"enabled"] boolValue]&&![state[@"status"] containsString:@(TOTK_UI("云同步已暂停", "Cloud sync paused"))])[lines addObject:@(TOTK_UI("云同步已暂停，本机保存和历史继续保留。", "Cloud sync paused. Local saves and history are retained."))];
    if([state[@"pendingAction"] count])[lines addObject:@(TOTK_UI("已安排载入所选进度；请退出并重新打开游戏，可以取消安排。", "The selected progress will load after you exit and reopen the game. You can cancel this selection."))];
    else if([state[@"restartToContinue"] boolValue]&&![state[@"status"] containsString:@(TOTK_UI("重新打开", "reopen"))])[lines addObject:@(TOTK_UI("有新进度可接续，请退出并重新打开游戏。", "New progress is ready. Exit and reopen the game to continue."))];
    NSMutableArray *names=[NSMutableArray new];
    for(NSDictionary *receipt in state[@"currentReceivers"])[names addObject:receipt[@"deviceName"]?:@(TOTK_UI("其他设备", "Other device"))];
    if(names.count)[lines addObject:[NSString stringWithFormat:@(TOTK_UI("已接收当前版本：%@。接收不代表已经载入游玩。", "Current version received by: %@. Receipt does not mean it has been loaded in the game.")),[names componentsJoinedByString:@"、"]]];
    if([state[@"waitingBackups"] unsignedIntegerValue])[lines addObject:@(TOTK_UI("有命名备份正在等待游戏保存完成。", "A named backup is waiting for the game to finish saving."))];
    if(state[@"historyRecordBytes"])[lines addObject:[NSString stringWithFormat:@(TOTK_UI("本机历史记录文件：%@；首版持续保留，不自动清理。", "Local history records: %@. History is retained without automatic cleanup in this release.")),[NSByteCountFormatter stringFromByteCount:[state[@"historyRecordBytes"] longLongValue] countStyle:NSByteCountFormatterCountStyleFile]]];
    return [lines componentsJoinedByString:@"\n"];
}

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>

static UIViewController *topController(id host) {
    UIViewController *vc=[host isKindOfClass:UIViewController.class]?host:nil;
    if (!vc) for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) if ([scene isKindOfClass:UIWindowScene.class])
        for (UIWindow *window in ((UIWindowScene *)scene).windows) if(window.isKeyWindow)vc=window.rootViewController;
    while(vc.presentedViewController)vc=vc.presentedViewController;
    if ([vc isKindOfClass:UINavigationController.class]) vc=((UINavigationController *)vc).visibleViewController;
    return vc;
}
static void showMessage(id host, NSString *message) {
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@(TOTK_UI("游戏进度", "Game Progress")) message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("好", "OK")) style:UIAlertActionStyleDefault handler:nil]];
    [topController(host) presentViewController:alert animated:YES completion:nil];
}
static void promptName(id host, NSString *initial, void (^completion)(NSString *)) {
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@(TOTK_UI("命名备份", "Named Backups")) message:@(TOTK_UI("保留这次已完成的游戏存档。", "Keep a named backup of this completed save.")) preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field){field.text=initial;field.placeholder=@(TOTK_UI("例如：最终战之前", "For example: Before the final battle"));field.accessibilityLabel=@(TOTK_UI("备份名称", "Backup name"));}];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("取消", "Cancel")) style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("保留备份", "Keep Backup")) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;completion(alert.textFields.firstObject.text?:@"");}]];
    [topController(host) presentViewController:alert animated:YES completion:nil];
}
@interface TOTKProgressManager : UITableViewController
@property(strong) TOTKProgressSync *sync;
@property(copy) void (^enterGame)(void);
@end
@implementation TOTKProgressManager
- (void)viewDidLoad {
    [super viewDidLoad];self.title=@(TOTK_UI("游戏进度", "Game Progress"));
    self.tableView.sectionHeaderHeight=UITableViewAutomaticDimension;
    self.tableView.estimatedSectionHeaderHeight=60;
    self.navigationItem.rightBarButtonItem=self.enterGame?[[UIBarButtonItem alloc] initWithTitle:@(TOTK_UI("进入游戏", "Play")) style:UIBarButtonItemStylePlain target:self action:@selector(close)]:[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(close)];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refreshView) name:TOTKProgressDidChange object:self.sync];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refreshView) name:UIContentSizeCategoryDidChangeNotification object:nil];
    [self refreshView];
}
- (void)dealloc {[NSNotificationCenter.defaultCenter removeObserver:self];}
- (void)close {
    if(self.enterGame)self.enterGame();
    else [self dismissViewControllerAnimated:YES completion:nil];
}
- (void)refreshView {
    if(self.enterGame)self.navigationItem.rightBarButtonItem.enabled=[self.sync.viewState[@"localPrepared"] boolValue];
    [self.tableView reloadData];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {(void)tableView;return 4;}
- (NSArray *)actions {
    NSMutableArray *actions=[NSMutableArray arrayWithArray:@[@"folder",@"refresh",@"backup",@"toggle"]];
    if([self.sync.viewState[@"pendingAction"] count])[actions addObject:@"cancel"];
    if([self.sync.viewState[@"canContinueLocal"] boolValue])[actions addObject:@"continue"];
    return actions;
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;NSDictionary *state=self.sync.viewState;
    if(section==0)return self.actions.count;
    if(section==1)return [state[@"heads"] count]>1?[state[@"heads"] count]:0;
    return [state[section==2?@"versions":@"backups"] count];
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView;return @[@(TOTK_UI("同步与当前进度", "Sync and Current Progress")),@(TOTK_UI("待选择进度（各份均保留）", "Choose Progress (All Branches Retained)")),@(TOTK_UI("历史版本", "History")),@(TOTK_UI("命名备份", "Named Backups"))][section];
}
- (UIView *)tableView:(UITableView *)tableView viewForHeaderInSection:(NSInteger)section {
    UITableViewHeaderFooterView *header=[tableView dequeueReusableHeaderFooterViewWithIdentifier:@"progress-section"];
    if(!header)header=[[UITableViewHeaderFooterView alloc] initWithReuseIdentifier:@"progress-section"];
    UIListContentConfiguration *content=[UIListContentConfiguration groupedHeaderConfiguration];
    NSString *title=[self tableView:tableView titleForHeaderInSection:section];
    if(section==0) {
        // One system section header keeps status and title in the same safe-area column.
        NSMutableAttributedString *text=[[NSMutableAttributedString alloc] initWithString:[statusText(self.sync.viewState) stringByAppendingString:@"\n\n"] attributes:@{NSFontAttributeName:[UIFont preferredFontForTextStyle:UIFontTextStyleBody],NSForegroundColorAttributeName:UIColor.labelColor}];
        [text appendAttributedString:[[NSAttributedString alloc] initWithString:title attributes:@{NSFontAttributeName:[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote],NSForegroundColorAttributeName:UIColor.secondaryLabelColor}]];
        content.attributedText=text;
    } else content.text=title;
    content.textProperties.numberOfLines=0;header.contentConfiguration=content;
    return header;
}
- (NSDictionary *)recordForIndex:(NSIndexPath *)index {
    NSDictionary *state=self.sync.viewState;
    if(index.section==1) {
        NSString *key=state[@"heads"][index.row];for(NSDictionary *r in state[@"versions"])if([r[@"id"] isEqual:key])return r;
    }
    return index.section>=2?state[index.section==2?@"versions":@"backups"][index.row]:nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)index {
    UITableViewCell *cell=[tableView dequeueReusableCellWithIdentifier:@"progress"];
    if(!cell)cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"progress"];
    cell.textLabel.numberOfLines=0;cell.detailTextLabel.numberOfLines=0;cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];cell.textLabel.adjustsFontForContentSizeCategory=YES;
    cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];cell.detailTextLabel.adjustsFontForContentSizeCategory=YES;
    cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
    if(index.section==0) {
        NSDictionary *titles=@{@"folder":@(TOTK_UI("选择 iCloud 同步文件夹", "Choose iCloud Sync Folder")),@"refresh":@(TOTK_UI("立即检查同步", "Check Sync Now")),@"backup":@(TOTK_UI("为当前存档命名备份", "Name a Backup of the Current Save")),@"toggle":[self.sync.viewState[@"enabled"] boolValue]?@(TOTK_UI("暂停云同步", "Pause Cloud Sync")):@(TOTK_UI("开启云同步", "Enable Cloud Sync")),@"cancel":@(TOTK_UI("取消下次载入安排", "Cancel Next-Launch Selection")),@"continue":@(TOTK_UI("使用本机进度继续", "Continue with Local Progress"))};
        cell.textLabel.text=titles[self.actions[index.row]];cell.detailTextLabel.text=nil;
    }
    else {
        NSDictionary *r=[self recordForIndex:index];
        cell.textLabel.text=index.section==3?r[@"name"]:versionTitle(r);
        cell.detailTextLabel.text=index.section==3?@(TOTK_UI("固定保留，可恢复或改名", "Kept permanently; restore or rename")):versionDetail(r);
        if([r[@"id"] isEqual:self.sync.viewState[@"base"]])cell.accessoryType=UITableViewCellAccessoryCheckmark;
    }
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)index {
    [tableView deselectRowAtIndexPath:index animated:YES];
    if(index.section==0) {
        NSArray *actions=self.actions;if(index.row<0||(NSUInteger)index.row>=actions.count)return;NSString *action=actions[index.row];
        if([action isEqual:@"folder"])[self.sync chooseFolderFrom:self];
        if([action isEqual:@"refresh"])[self.sync refresh];
        if([action isEqual:@"toggle"])[self.sync setSynchronizationEnabled:![self.sync.viewState[@"enabled"] boolValue]];
        if([action isEqual:@"cancel"])[self.sync cancelPendingSelection];
        if([action isEqual:@"backup"]) {
            NSString *base=self.sync.viewState[@"base"];
            if(!base.length){showMessage(self,@(TOTK_UI("尚无完整的已保存进度，完成一次游戏内保存后再试。", "No complete saved progress is available yet. Save in the game and try again.")));return;}
            promptName(self,@"",^(NSString *name){[self.sync nameBackup:name version:nil replacing:nil];});
        }
        if([action isEqual:@"continue"]){[self close];[self.sync continueLocal];}
        return;
    }
    NSDictionary *r=[self recordForIndex:index];NSString *version=index.section==3?r[@"target"]:r[@"id"];
    NSString *message=self.sync.gameRunning?@(TOTK_UI("当前游玩不会被打断，所选进度将在下次启动游戏时载入；现有历史仍保留。", "Your current game continues unchanged. The selected progress will load on the next launch; existing history is retained.")):@(TOTK_UI("将使用这份完整进度继续，其他版本仍保留。", "Continue with this complete progress. Other versions are retained."));
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:index.section==3?r[@"name"]:versionTitle(r) message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("取消", "Cancel")) style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:self.sync.gameRunning?@(TOTK_UI("安排下次启动载入", "Load on Next Launch")):@(TOTK_UI("使用此进度", "Use This Progress")) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;[self.sync selectVersion:version restore:index.section!=1];}]];
    [alert addAction:[UIAlertAction actionWithTitle:index.section==3?@(TOTK_UI("修改备份名称", "Rename Backup")):@(TOTK_UI("创建命名备份", "Create Named Backup")) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){
        (void)action;promptName(self,index.section==3?r[@"name"]:@"",^(NSString *name){[self.sync nameBackup:name version:version replacing:index.section==3?r[@"id"]:nil];});
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end
@interface TOTKProgressFolderDelegate : NSObject<UIDocumentPickerDelegate>
@property(strong) TOTKProgressSync *sync;
@property(weak) UIViewController *host;
@end
static TOTKProgressFolderDelegate *folderDelegate;
@implementation TOTKProgressFolderDelegate
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {(void)controller;folderDelegate=nil;}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSError *error=nil;if(urls.firstObject)[self.sync useFolder:urls.firstObject error:&error];
    UIViewController *host=self.host;NSString *message=TOTKProgressErrorMessage(error);
    [controller dismissViewControllerAnimated:YES completion:^{if(message)showMessage(host,message);}];folderDelegate=nil;
}
@end
id totk_progress_make_manager(TOTKProgressSync *sync,void (^enterGame)(void)) {
    TOTKProgressManager *manager=[[TOTKProgressManager alloc] initWithStyle:UITableViewStyleInsetGrouped];manager.sync=sync;
    manager.enterGame=enterGame;
    UINavigationController *navigation=[[UINavigationController alloc] initWithRootViewController:manager];navigation.modalPresentationStyle=UIModalPresentationFormSheet;
    return navigation;
}
void totk_progress_show_manager(TOTKProgressSync *sync,id host) {
    UIViewController *top=topController(host);if([top isKindOfClass:TOTKProgressManager.class])return;
    UINavigationController *navigation=totk_progress_make_manager(sync,nil);
    [top presentViewController:navigation animated:YES completion:nil];
}
void totk_progress_choose_folder(TOTKProgressSync *sync,id host) {
    if(folderDelegate)return;
    folderDelegate=[TOTKProgressFolderDelegate new];folderDelegate.sync=sync;folderDelegate.host=topController(host);
    UIDocumentPickerViewController *picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];picker.delegate=folderDelegate;picker.allowsMultipleSelection=NO;
    [folderDelegate.host presentViewController:picker animated:YES completion:nil];
}
void totk_progress_offer_wait(TOTKProgressSync *sync,id host) {
    if(sync.gameRunning)return;
    UIViewController *top=topController(host);if([top isKindOfClass:TOTKProgressManager.class]||[top isKindOfClass:UIAlertController.class])return;
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@(TOTK_UI("正在接续游戏进度", "Preparing Game Progress")) message:sync.viewState[@"status"] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("继续等待", "Keep Waiting")) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;[sync refresh];}]];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("使用本机进度继续", "Continue with Local Progress")) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;[sync continueLocal];}]];
    [alert addAction:[UIAlertAction actionWithTitle:@(TOTK_UI("查看进度", "View Progress")) style:UIAlertActionStyleDefault handler:^(UIAlertAction *action){(void)action;[sync showFrom:host];}]];
    [top presentViewController:alert animated:YES completion:nil];
}

#else
#import <AppKit/AppKit.h>
static NSWindow *hostWindow(id host) {return [host isKindOfClass:NSWindow.class]?host:NSApp.keyWindow;}
static void showMessage(id host,NSString *message) {
    NSWindow *window=hostWindow(host);if(!window||window.attachedSheet)return;
    NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("游戏进度", "Game Progress"));alert.informativeText=message;[alert addButtonWithTitle:@(TOTK_UI("好", "OK"))];[alert beginSheetModalForWindow:window completionHandler:nil];
}
static void promptName(id host,NSString *initial,void (^completion)(NSString *)) {
    NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("命名备份", "Named Backups"));alert.informativeText=@(TOTK_UI("保留这次已完成的游戏存档。", "Keep a named backup of this completed save."));
    NSTextField *field=[[NSTextField alloc] initWithFrame:NSMakeRect(0,0,300,28)];field.stringValue=initial?:@"";field.placeholderString=@(TOTK_UI("例如：最终战之前", "For example: Before the final battle"));field.accessibilityLabel=@(TOTK_UI("备份名称", "Backup name"));alert.accessoryView=field;
    [alert addButtonWithTitle:@(TOTK_UI("保留备份", "Keep Backup"))];[alert addButtonWithTitle:@(TOTK_UI("取消", "Cancel"))];
    NSWindow *window=hostWindow(host);if(!window)return;
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response){if(response==NSAlertFirstButtonReturn)completion(field.stringValue);}];
}
@interface TOTKProgressManager : NSWindowController<NSTableViewDataSource,NSTableViewDelegate>
@property(strong) TOTKProgressSync *sync;
@property(strong) NSTextField *status;
@property(strong) NSTableView *table;
@property(strong) NSArray<NSDictionary *> *rows;
@property(strong) NSButton *continueButton,*toggleButton,*cancelButton;
@end
@implementation TOTKProgressManager
- (instancetype)initWithSync:(TOTKProgressSync *)sync {
    NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,720,600) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    if(!(self=[super initWithWindow:window]))return nil;
    self.sync=sync;window.title=@(TOTK_UI("游戏进度", "Game Progress"));window.releasedWhenClosed=NO;
    NSStackView *stack=[NSStackView new];stack.orientation=NSUserInterfaceLayoutOrientationVertical;stack.spacing=12;stack.alignment=NSLayoutAttributeLeading;stack.translatesAutoresizingMaskIntoConstraints=NO;
    [window.contentView addSubview:stack];[NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:window.contentView.leadingAnchor constant:20],[stack.trailingAnchor constraintEqualToAnchor:window.contentView.trailingAnchor constant:-20],[stack.topAnchor constraintEqualToAnchor:window.contentView.topAnchor constant:20],[stack.bottomAnchor constraintEqualToAnchor:window.contentView.bottomAnchor constant:-20]]];
    self.status=[NSTextField wrappingLabelWithString:@(TOTK_UI("正在准备进度列表…", "Preparing progress history…"))];self.status.font=[NSFont systemFontOfSize:14];[stack addArrangedSubview:self.status];
    self.toggleButton=[NSButton buttonWithTitle:@(TOTK_UI("暂停云同步", "Pause Cloud Sync")) target:self action:@selector(toggleSync)];
    self.continueButton=[NSButton buttonWithTitle:@(TOTK_UI("使用本机继续", "Continue Locally")) target:self action:@selector(continueLocal)];
    self.cancelButton=[NSButton buttonWithTitle:@(TOTK_UI("取消下次载入安排", "Cancel Next-Launch Selection")) target:self action:@selector(cancelPending)];
    NSStackView *tools=[NSStackView stackViewWithViews:@[[NSButton buttonWithTitle:@(TOTK_UI("选择 iCloud 文件夹", "Choose iCloud Folder")) target:self action:@selector(choose)],[NSButton buttonWithTitle:@(TOTK_UI("检查同步", "Check Sync")) target:self action:@selector(check)],self.toggleButton]];[stack addArrangedSubview:tools];
    NSStackView *local=[NSStackView stackViewWithViews:@[[NSButton buttonWithTitle:@(TOTK_UI("命名当前存档", "Name Current Save")) target:self action:@selector(nameCurrent)],self.continueButton,self.cancelButton]];local.detachesHiddenViews=YES;[stack addArrangedSubview:local];
    NSScrollView *scroll=[NSScrollView new];scroll.hasVerticalScroller=YES;scroll.borderType=NSBezelBorder;
    self.table=[NSTableView new];self.table.rowHeight=46;self.table.delegate=self;self.table.dataSource=self;self.table.allowsMultipleSelection=NO;
    NSTableColumn *title=[[NSTableColumn alloc] initWithIdentifier:@"title"];title.title=@(TOTK_UI("进度与备份", "Progress and Backups"));title.width=410;[self.table addTableColumn:title];
    NSTableColumn *detail=[[NSTableColumn alloc] initWithIdentifier:@"detail"];detail.title=@(TOTK_UI("来源与版本", "Source and Version"));detail.width=240;[self.table addTableColumn:detail];
    scroll.documentView=self.table;[stack addArrangedSubview:scroll];[scroll.widthAnchor constraintEqualToAnchor:stack.widthAnchor].active=YES;[scroll.heightAnchor constraintGreaterThanOrEqualToConstant:260].active=YES;
    NSStackView *actions=[NSStackView stackViewWithViews:@[[NSButton buttonWithTitle:@(TOTK_UI("选择为主进度", "Select Main Progress")) target:self action:@selector(select)],[NSButton buttonWithTitle:@(TOTK_UI("恢复所选历史", "Restore Selected History")) target:self action:@selector(restore)],[NSButton buttonWithTitle:@(TOTK_UI("修改备份名称", "Rename Backup")) target:self action:@selector(rename)]]];[stack addArrangedSubview:actions];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refreshView) name:TOTKProgressDidChange object:sync];[self refreshView];return self;
}
- (void)dealloc {[NSNotificationCenter.defaultCenter removeObserver:self];}
- (void)refreshView {
    NSDictionary *state=self.sync.viewState;self.status.stringValue=statusText(state);
    self.toggleButton.title=[state[@"enabled"] boolValue]?@(TOTK_UI("暂停云同步", "Pause Cloud Sync")):@(TOTK_UI("开启云同步", "Enable Cloud Sync"));
    self.continueButton.hidden=![state[@"canContinueLocal"] boolValue];self.cancelButton.hidden=![state[@"pendingAction"] count];
    NSString *selectedID=self.selected[@"id"];
    NSMutableArray *rows=[NSMutableArray new];
    for(NSDictionary *r in state[@"versions"]) {
        NSMutableDictionary *row=[r mutableCopy];row[@"role"]=[state[@"heads"] count]>1&&[state[@"heads"] containsObject:r[@"id"]]?@(TOTK_UI("待选择", "Choose Progress")):@(TOTK_UI("历史", "History"));
        if([r[@"id"] isEqual:state[@"base"]])row[@"role"]=@(TOTK_UI("当前", "Current"));[rows addObject:row];
    }
    for(NSDictionary *r in state[@"backups"]) {NSMutableDictionary *row=[r mutableCopy];row[@"role"]=@(TOTK_UI("备份", "Backup"));[rows addObject:row];}
    self.rows=rows;[self.table reloadData];
    if(selectedID)for(NSUInteger i=0;i<rows.count;i++)if([rows[i][@"id"] isEqual:selectedID]){[self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:i] byExtendingSelection:NO];break;}
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView {(void)tableView;return self.rows.count;}
- (NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(NSTableColumn *)column row:(NSInteger)index {
    (void)tableView;NSDictionary *r=self.rows[index];BOOL backup=[r[@"role"] isEqual:@(TOTK_UI("备份", "Backup"))];
    NSString *text=[column.identifier isEqual:@"title"]?[NSString stringWithFormat:@"%@ · %@",r[@"role"],backup?r[@"name"]:versionTitle(r)]:backup?@(TOTK_UI("固定保留", "Kept permanently")):versionDetail(r);
    NSTextField *field=[NSTextField wrappingLabelWithString:text];field.font=[NSFont systemFontOfSize:13];return field;
}
- (NSDictionary *)selected {NSInteger row=self.table.selectedRow;return row>=0&&row<(NSInteger)self.rows.count?self.rows[row]:nil;}
- (void)choose {[self.sync chooseFolderFrom:self.window];}
- (void)check {[self.sync refresh];}
- (void)toggleSync {[self.sync setSynchronizationEnabled:![self.sync.viewState[@"enabled"] boolValue]];}
- (void)cancelPending {[self.sync cancelPendingSelection];}
- (void)continueLocal {[self close];[self.sync continueLocal];}
- (void)nameCurrent {
    NSString *base=self.sync.viewState[@"base"];if(!base.length){showMessage(self.window,@(TOTK_UI("尚无完整的已保存进度，完成一次游戏内保存后再试。", "No complete saved progress is available yet. Save in the game and try again.")));return;}
    promptName(self.window,@"",^(NSString *name){[self.sync nameBackup:name version:nil replacing:nil];});
}
- (void)rename {
    NSDictionary *r=self.selected;if(![r[@"role"] isEqual:@(TOTK_UI("备份", "Backup"))]){showMessage(self.window,@(TOTK_UI("请先选择一份命名备份。", "Select a named backup first.")));return;}
    promptName(self.window,r[@"name"],^(NSString *name){[self.sync nameBackup:name version:r[@"target"] replacing:r[@"id"]];});
}
- (void)performRestore:(BOOL)restore {
    NSDictionary *r=self.selected;if(!r){showMessage(self.window,@(TOTK_UI("请先选择一份进度或备份。", "Select progress or a backup first.")));return;}NSString *version=r[@"target"]?:r[@"id"];
    NSAlert *alert=[NSAlert new];alert.messageText=restore?@(TOTK_UI("恢复所选进度？", "Restore this progress?")):@(TOTK_UI("选择这份主进度？", "Select this as the main progress?"));
    alert.informativeText=self.sync.gameRunning?@(TOTK_UI("当前游玩不会被打断，将在下次启动时载入。其他版本仍保留。", "Your current game continues unchanged. This progress will load on the next launch. Other versions are retained.")):@(TOTK_UI("将使用这份完整进度继续；其他版本仍保留。", "Continue with this complete progress; other versions are retained."));
    [alert addButtonWithTitle:self.sync.gameRunning?@(TOTK_UI("安排下次载入", "Load on Next Launch")):@(TOTK_UI("使用此进度", "Use This Progress"))];[alert addButtonWithTitle:@(TOTK_UI("取消", "Cancel"))];
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response){if(response==NSAlertFirstButtonReturn)[self.sync selectVersion:version restore:restore];}];
}
- (void)select {[self performRestore:NO];}
- (void)restore {[self performRestore:YES];}
@end
static TOTKProgressManager *manager;
void totk_progress_show_manager(TOTKProgressSync *sync,id host) {
    (void)host;if(!manager)manager=[[TOTKProgressManager alloc] initWithSync:sync];[manager refreshView];[manager showWindow:nil];[manager.window makeKeyAndOrderFront:nil];
}
void totk_progress_choose_folder(TOTKProgressSync *sync,id host) {
    NSOpenPanel *panel=[NSOpenPanel openPanel];panel.canChooseFiles=NO;panel.canChooseDirectories=YES;panel.allowsMultipleSelection=NO;panel.prompt=@(TOTK_UI("选择同步文件夹", "Choose Sync Folder"));
    void (^finish)(NSModalResponse)=^(NSModalResponse response){if(response==NSModalResponseOK){NSError *error=nil;[sync useFolder:panel.URL error:&error];if(error){NSAlert *alert=[NSAlert new];alert.messageText=TOTKProgressErrorMessage(error);[alert beginSheetModalForWindow:hostWindow(host) completionHandler:nil];}}};
    NSWindow *window=hostWindow(host);if(window)[panel beginSheetModalForWindow:window completionHandler:finish];else [panel beginWithCompletionHandler:finish];
}
void totk_progress_offer_wait(TOTKProgressSync *sync,id host) {
    if(sync.gameRunning)return;
    NSWindow *window=hostWindow(host);if(!window||window.attachedSheet||manager.window.isVisible)return;
    NSAlert *alert=[NSAlert new];alert.messageText=@(TOTK_UI("正在接续游戏进度", "Preparing Game Progress"));alert.informativeText=sync.viewState[@"status"]?:@(TOTK_UI("正在检查…", "Checking…"));
    [alert addButtonWithTitle:@(TOTK_UI("继续等待", "Keep Waiting"))];[alert addButtonWithTitle:@(TOTK_UI("使用本机进度继续", "Continue with Local Progress"))];[alert addButtonWithTitle:@(TOTK_UI("查看进度", "View Progress"))];
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response){if(response==NSAlertSecondButtonReturn)[sync continueLocal];else if(response==NSAlertThirdButtonReturn)[sync showFrom:host];else [sync refresh];}];
}
#endif
