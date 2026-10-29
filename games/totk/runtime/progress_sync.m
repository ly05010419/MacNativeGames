#import "progress_sync.h"
#import "progress_store.h"
#import "progress_transport.h"
#import "progress_errors.h"
#include "progress_save_gate.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

NSString *const TOTKProgressDidChange = @"TOTKProgressDidChange";
NSString *const TOTKProgressDidCheck = @"TOTKProgressDidCheck";
static NSString *const EnabledKey = @"TOTK_PROGRESS_ENABLED";
static NSString *const BookmarkKey = @"TOTK_PROGRESS_FOLDER";
static NSString *const PendingKey = @"TOTK_PROGRESS_PENDING_ACTION";
static NSString *const WaitingBackupsKey = @"TOTK_PROGRESS_WAITING_BACKUPS";

@interface TOTKProgressDirectoryPresenter : NSObject<NSFilePresenter>
@property(copy) NSURL *presentedItemURL;
@property(strong) NSOperationQueue *presentedItemOperationQueue;
@property(copy) void (^changed)(void);
@end
@implementation TOTKProgressDirectoryPresenter
- (void)presentedItemDidChange { if(self.changed)self.changed(); }
- (void)presentedSubitemDidAppearAtURL:(NSURL *)url { (void)url;if(self.changed)self.changed(); }
- (void)presentedSubitemDidChangeAtURL:(NSURL *)url { (void)url;if(self.changed)self.changed(); }
@end

@interface TOTKProgressSync ()
@property(atomic, readwrite) NSDictionary *viewState;
@property(atomic, readwrite) BOOL gameRunning;
@end
@implementation TOTKProgressSync {
    dispatch_queue_t _storeQueue;
    TOTKProgressStore *_store;
    TOTKProgressTransport *_transport;
    NSURL *_sourceProfile, *_localRoot, *_folder, *_readyProfile;
    NSString *_gameVersion;
    NSString *_startupProblem;
    __weak id _host;
    void (^_start)(void);
    BOOL _transferRunning, _needsTransfer, _hasStore, _starting;
    _Atomic BOOL _localPrepared, _enabled, _folderAccessible, _localRecoveryRequired;
    _Atomic uint64_t _configuration;
    NSTimer *_timer;
    id<NSFilePresenter> _presenter;
    uint64_t _lastCapturedGeneration;
}
+ (instancetype)shared { static id value; static dispatch_once_t once; dispatch_once(&once,^{ value=[self new]; });return value; }
+ (void)showMenu:(id)sender {
    (void)sender;
#if TARGET_OS_IPHONE
    [[self shared] showFrom:nil];
#else
    [[self shared] showFrom:NSApp.keyWindow];
#endif
}
- (void)showButton:(id)sender { (void)sender;[self showFrom:_host]; }
- (instancetype)init {
    if ((self=[super init])) {
        _storeQueue=dispatch_queue_create("totk.progress.local-store",DISPATCH_QUEUE_SERIAL);
        _viewState=@{@"status":@(TOTK_UI("同步尚未开启", "Sync is not enabled")),@"versions":@[],@"backups":@[],@"heads":@[]};
        _lastCapturedGeneration=UINT64_MAX;
    }
    return self;
}
- (void)publish:(NSString *)status {
    NSMutableDictionary *view=[NSMutableDictionary new]; view[@"status"]=status;
    view[@"enabled"]=@(_enabled);view[@"folderAuthorized"]=@(_folderAccessible);
    view[@"localPrepared"]=@(_localPrepared);
    view[@"canContinueLocal"]=@(_localPrepared&&!self.gameRunning&&_start!=nil);
    view[@"pendingAction"]=[NSUserDefaults.standardUserDefaults dictionaryForKey:PendingKey]?:@{};
    view[@"waitingBackups"]=@([[NSUserDefaults.standardUserDefaults arrayForKey:WaitingBackupsKey] count]);
    if (_store) {
        NSArray *versions=[_store.versions.allValues sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b) { return [b[@"created"] compare:a[@"created"]]; }];
        view[@"versions"]=versions;view[@"backups"]=_store.backups;view[@"heads"]=_store.heads;
        view[@"receipts"]=_store.receipts;view[@"base"]=_store.baseVersion?:@"";
        NSString *head=_store.heads.count==1?_store.heads.firstObject:nil;
        view[@"restartToContinue"]=@(self.gameRunning&&head&&_store.baseVersion&&![head isEqual:_store.baseVersion]&&[_store isDescendant:head of:_store.baseVersion]&&[_store.versions[head][@"version"] isEqual:_gameVersion]);
        NSMutableDictionary *receivers=[NSMutableDictionary new];
        for(NSDictionary *receipt in _store.receipts)if([receipt[@"target"] isEqual:_store.baseVersion]&&![receipt[@"device"] isEqual:_store.deviceID]) {
            NSDictionary *prior=receivers[receipt[@"device"]];
            if(!prior||[receipt[@"created"] doubleValue]>[prior[@"created"] doubleValue])receivers[receipt[@"device"]]=receipt;
        }
        view[@"currentReceivers"]=[receivers.allValues sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b){return [a[@"device"] compare:b[@"device"]];}];
        view[@"problems"]=_store.pendingProblems;
        view[@"localRecoveryRequired"]=@(_localRecoveryRequired);
        unsigned long long bytes=0,recordBytes=0;
        for (NSDictionary *r in versions) {
            bytes += [r[@"bytes"] unsignedLongLongValue];
            NSURL *file=[[_store.localRoot URLByAppendingPathComponent:@"records"] URLByAppendingPathComponent:[r[@"id"] stringByAppendingString:@".json"]];
            recordBytes += [NSFileManager.defaultManager attributesOfItemAtPath:file.path error:nil].fileSize;
        }
        view[@"historyBytes"]=@(bytes);view[@"historyRecordBytes"]=@(recordBytes);
    }
    view[@"running"]=@(self.gameRunning);
    dispatch_async(dispatch_get_main_queue(),^{ self.viewState=view;[NSNotificationCenter.defaultCenter postNotificationName:TOTKProgressDidChange object:self]; });
}
- (void)prepareProfile:(NSURL *)profile gameVersion:(NSString *)version host:(id)host completion:(void (^)(void))completion {
    NSAssert(NSThread.isMainThread,@"prepare on main");
    _sourceProfile=profile;_readyProfile=profile;_gameVersion=version;_host=host;_start=[completion copy];
    const char *override=getenv("TOTK_PROGRESS_ROOT");
    _localRoot=override?[NSURL fileURLWithPath:@(override) isDirectory:YES]:[profile URLByAppendingPathComponent:@".progress" isDirectory:YES];
    _enabled=[NSUserDefaults.standardUserDefaults boolForKey:EnabledKey];
    BOOL existing=[NSFileManager.defaultManager fileExistsAtPath:[[_localRoot URLByAppendingPathComponent:@"state.json"] path]];
    if (completion&&!_enabled&&!existing) { _localPrepared=YES;[self continueLocal];return; }
    [self initializeStore];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        if (self->_start&&self->_localPrepared) totk_progress_offer_wait(self,self->_host);
    });
}
- (void)initializeStore {
    dispatch_async(_storeQueue,^{ @autoreleasepool {
        if (!self->_store) {
            NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
            NSString *device=[defaults stringForKey:@"TOTK_PROGRESS_DEVICE_ID"];
            if (!device) { device=NSUUID.UUID.UUIDString.lowercaseString;[defaults setObject:device forKey:@"TOTK_PROGRESS_DEVICE_ID"]; }
            NSError *error=nil;
            self->_store=[[TOTKProgressStore alloc] initWithRoot:self->_localRoot device:device gameVersion:self->_gameVersion error:&error];
            if (!self->_store) { [self publish:TOTKProgressErrorMessage(error)?:@(TOTK_UI("本机进度区暂不可用，请重试；现有存档保留。", "The local progress store is unavailable. Try again; existing saves are retained."))];return; }
#if TARGET_OS_IPHONE
            self->_store.deviceName=UIDevice.currentDevice.name;
#else
            self->_store.deviceName=@"Mac";
#endif
            self->_transport=[[TOTKProgressTransport alloc] initWithLocalRoot:self->_localRoot];
        }
        NSError *error=nil;
        if (!self.gameRunning) {
            if (!self->_store.baseVersion) {
                [self->_store captureProfile:self->_sourceProfile error:&error];
                if(error&&[NSFileManager.defaultManager fileExistsAtPath:[[self->_sourceProfile URLByAppendingPathComponent:@"account.uid"] path]])self->_startupProblem=TOTKProgressErrorMessage(error);
            }
            self->_localRecoveryRequired=![self->_store workingProfileMatchesBase:&error];
            if (!self->_localRecoveryRequired && self->_store.baseVersion && ![self->_store checkout:self->_store.baseVersion resolving:NO error:&error]) {
                [self publish:TOTKProgressErrorMessage(error)?:@(TOTK_UI("无法准备完整进度；保留原档，请重试。", "Could not prepare complete progress. The original save is retained; try again."))];return;
            }
            NSDictionary *pending=[NSUserDefaults.standardUserDefaults dictionaryForKey:PendingKey];
            if(pending) {
                BOOL applied=[pending[@"restore"] boolValue]?[self->_store restoreVersion:pending[@"version"] error:&error]!=nil:[self->_store checkout:pending[@"version"] resolving:YES error:&error];
                if(applied){[NSUserDefaults.standardUserDefaults removeObjectForKey:PendingKey];self->_localRecoveryRequired=NO;self->_startupProblem=nil;}
                else self->_startupProblem=[@(TOTK_UI("安排的进度暂不能载入：", "The selected progress cannot be loaded yet: ")) stringByAppendingString:TOTKProgressErrorMessage(error)?:@(TOTK_UI("请重试或取消该安排；原进度保留。", "Retry or cancel the selection; the original progress is retained."))];
            }
        }
        NSURL *prepared=self->_store.workingProfile?:self->_sourceProfile;
        self->_localPrepared=YES;
        [self publish:self->_startupProblem?:(self->_localRecoveryRequired?@(TOTK_UI("上次退出留下未归档的本机存档，完整性尚未确认，已保留。可尝试使用本机继续，或从历史选择已验证的进度。", "The previous session left an unarchived local save. It is retained, but its completeness has not been confirmed. Continue locally or select verified progress from history.")):@(TOTK_UI("本机进度已保留，正在检查其他设备的新进度…", "Local progress is retained. Checking for progress from other devices…")))];
        dispatch_async(dispatch_get_main_queue(),^{
            self->_readyProfile=prepared;self->_localPrepared=YES;self->_hasStore=YES;
            if(self.gameRunning)runtime_fs_progress_enable();
            [self restoreFolder];
            if (self->_enabled&&self->_folder) [self refresh];
            else if (self->_start) {
                if(self->_localRecoveryRequired||self->_startupProblem)totk_progress_offer_wait(self,self->_host);
                else [self continueLocal];
            }
        });
    }});
}
- (void)restoreFolder {
    if (_folder) return;
    NSData *bookmark=[NSUserDefaults.standardUserDefaults dataForKey:BookmarkKey]; if (!bookmark) return;
    BOOL stale=NO;NSError *error=nil;
#if TARGET_OS_IPHONE
    NSURLBookmarkResolutionOptions options=NSURLBookmarkResolutionWithoutUI;
#else
    NSURLBookmarkResolutionOptions options=NSURLBookmarkResolutionWithSecurityScope|NSURLBookmarkResolutionWithoutUI;
#endif
    NSURL *url=[NSURL URLByResolvingBookmarkData:bookmark options:options relativeToURL:nil bookmarkDataIsStale:&stale error:&error];
    if (!url||![url startAccessingSecurityScopedResource]) {
        _folderAccessible=NO;dispatch_async(_storeQueue,^{[self publish:@(TOTK_UI("云文件夹授权已失效，请重新选择；本机进度保留。", "Cloud folder access has expired. Choose the folder again; local progress is retained."))];});return;
    }
    _folder=url;_folderAccessible=YES;
    if (stale) [self saveBookmark:url error:nil];
    if(_enabled)[self installWatcher];
}
- (BOOL)saveBookmark:(NSURL *)url error:(NSError **)error {
#if TARGET_OS_IPHONE
    NSURLBookmarkCreationOptions options=NSURLBookmarkCreationMinimalBookmark;
#else
    NSURLBookmarkCreationOptions options=NSURLBookmarkCreationWithSecurityScope;
#endif
    NSData *bookmark=[url bookmarkDataWithOptions:options includingResourceValuesForKeys:nil relativeToURL:nil error:error];
    if (!bookmark) return NO;
    [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:BookmarkKey];return YES;
}
- (void)useFolder:(NSURL *)folder error:(NSError **)error {
    if(!_sourceProfile||!_localRoot||!_gameVersion.length||!_localPrepared) {
        if(error)*error=[NSError errorWithDomain:@"TOTKProgress" code:5 userInfo:@{NSLocalizedDescriptionKey:@(TOTK_UI("游戏仍在准备，请等待启动完成后再选择同步文件夹。", "The game is still preparing. Wait for preparation to finish before choosing a sync folder."))}];return;
    }
    if (![folder startAccessingSecurityScopedResource]) {
        if (error) *error=[NSError errorWithDomain:@"TOTKProgress" code:5 userInfo:@{NSLocalizedDescriptionKey:@(TOTK_UI("未获得文件夹访问权限。", "Folder access was not granted."))}];return;
    }
    if (![self saveBookmark:folder error:error]) { [folder stopAccessingSecurityScopedResource];return; }
    if (_folder) [_folder stopAccessingSecurityScopedResource];
    _configuration++;_folder=folder;_folderAccessible=YES;_enabled=YES;[NSUserDefaults.standardUserDefaults setBool:YES forKey:EnabledKey];
    runtime_fs_progress_enable();
    if (!_hasStore) [self initializeStore];
    [self installWatcher];[self saveCommitted];[self refresh];
}
- (void)installWatcher {
    // Native directory notifications are supplemented by a modest foreground retry timer.
    if (_presenter) [NSFileCoordinator removeFilePresenter:_presenter];
    TOTKProgressDirectoryPresenter *presenter=[TOTKProgressDirectoryPresenter new];
    presenter.presentedItemURL=[_folder URLByAppendingPathComponent:@"totk-progress-v1" isDirectory:YES];
    presenter.presentedItemOperationQueue=[NSOperationQueue new];presenter.presentedItemOperationQueue.maxConcurrentOperationCount=1;
    __weak TOTKProgressSync *weakSelf=self;
    presenter.changed=^{dispatch_async(dispatch_get_main_queue(),^{[weakSelf refresh];});};
    _presenter=presenter;[NSFileCoordinator addFilePresenter:presenter];
    [_timer invalidate];
    _timer=[NSTimer scheduledTimerWithTimeInterval:15 repeats:YES block:^(NSTimer *timer) {
        (void)timer;
#if TARGET_OS_IPHONE
        if (UIApplication.sharedApplication.applicationState!=UIApplicationStateActive) return;
#endif
        [self saveCommitted];[self refresh];
    }];
#if TARGET_OS_IPHONE
    [NSNotificationCenter.defaultCenter removeObserver:self name:UIApplicationDidBecomeActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refresh) name:UIApplicationDidBecomeActiveNotification object:nil];
#endif
}
- (void)setSynchronizationEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread,@"sync preference on main");
    if(enabled&&!_folder){[self restoreFolder];if(!_folder){[self chooseFolderFrom:_host];return;}}
    _configuration++;_enabled=enabled;[NSUserDefaults.standardUserDefaults setBool:enabled forKey:EnabledKey];
    if(enabled){[self installWatcher];[self refresh];}
    else {
        if(_presenter){[NSFileCoordinator removeFilePresenter:_presenter];_presenter=nil;}
        [_timer invalidate];_timer=nil;_needsTransfer=NO;
        if(_start&&_localPrepared&&!_localRecoveryRequired)[self continueLocal];
    }
    dispatch_async(_storeQueue,^{[self publish:enabled?@(TOTK_UI("云同步已恢复，正在检查并补传本机进度。", "Cloud sync resumed. Checking for updates and uploading pending local progress.")):@(TOTK_UI("云同步已暂停；本机保存、历史和备份继续保留。已开始的传输可能仍会收尾。", "Cloud sync paused. Local saves, history and backups are retained. Transfers already started may still finish."))];});
}
- (void)cancelPendingSelection {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:PendingKey];
    dispatch_async(_storeQueue,^{self->_startupProblem=nil;[self publish:@(TOTK_UI("已取消下次载入安排，当前进度不变。", "Next-launch selection cancelled. Current progress is unchanged."))];});
}
- (void)continueLocal {
    NSAssert(NSThread.isMainThread,@"start on main");
    if (!_start||!_localPrepared||_starting) return;
    _starting=YES;
    dispatch_async(_storeQueue,^{
        // Serialize the startup boundary with checkout: no remote application may cross it.
        self.gameRunning=YES;
        NSURL *ready=self->_store.workingProfile?:self->_readyProfile;
        BOOL managed=self->_store!=nil;
        [self publish:@(TOTK_UI("本机进度已准备好，正在进入游戏。", "Local progress is ready. Starting the game."))];
        dispatch_async(dispatch_get_main_queue(),^{
            self->_readyProfile=ready;
            if (ready) setenv("TOTK_SAVE_PROFILE_DIR",ready.fileSystemRepresentation,1);
            if (managed) runtime_fs_progress_enable();
            void (^start)(void)=self->_start;self->_start=nil;
            if(start)start();
        });
    });
}
- (void)refresh {
    if (!NSThread.isMainThread) {dispatch_async(dispatch_get_main_queue(),^{[self refresh];});return;}
    if (!_hasStore) { if (_sourceProfile) [self initializeStore]; return; }
    if (!_folder||!_enabled) return;
    if (_transferRunning) {_needsTransfer=YES;return;}
    _transferRunning=YES;
    uint64_t configuration=_configuration;NSURL *folder=_folder;
    BOOL scoped=[folder startAccessingSecurityScopedResource];
    [_transport exchangeWithFolder:folder completion:^(NSArray<NSURL *> *incoming,NSArray<NSString *> *problems) {
        if(scoped)[folder stopAccessingSecurityScopedResource];
        dispatch_async(self->_storeQueue,^{ @autoreleasepool {
            if(configuration!=self->_configuration){dispatch_async(dispatch_get_main_queue(),^{self->_transferRunning=NO;[self refresh];});return;}
            NSMutableArray *all=[problems mutableCopy];NSError *error=nil;
            for (NSURL *file in incoming) {
                if (![self->_store ingestRecordAtURL:file error:&error]) [all addObject:TOTKProgressErrorMessage(error)?:@(TOTK_UI("某版本暂不可用。", "A version is temporarily unavailable."))];
                else if([file.URLByDeletingLastPathComponent.URLByResolvingSymlinksInPath.path isEqual:[self->_localRoot URLByAppendingPathComponent:@"incoming"].URLByResolvingSymlinksInPath.path])
                    [NSFileManager.defaultManager removeItemAtURL:file error:nil]; // The immutable verified record is already durable.
            }
            if (![self->_store acknowledgeDownloads:&error]) [all addObject:TOTKProgressErrorMessage(error)?:@(TOTK_UI("接收确认尚未保存。", "The receipt has not been saved yet."))];
            [all addObjectsFromArray:self->_store.pendingProblems];
            NSArray *heads=self->_store.heads;
            NSString *status=all.count?all.firstObject:@(TOTK_UI("已检查，暂未发现其他新进度。", "Checked; no other new progress is currently visible."));
            if (!all.count&&heads.count>1) status=@(TOTK_UI("有多个进度待选择，各份都已保留。", "Multiple progress branches are available. All are retained; choose one to continue."));
            else if (!all.count&&heads.count==1&&![heads.firstObject isEqual:self->_store.baseVersion]) status=self.gameRunning?@(TOTK_UI("已收到其他设备的新进度；退出并重新打开游戏后接续，当前游玩保持不变。", "Progress from another device was received. Exit and reopen the game to continue from it; your current game is unchanged.")):@(TOTK_UI("正在准备接续进度…", "Preparing to continue progress…"));
            if(self->_localRecoveryRequired)status=@(TOTK_UI("上次退出留下未归档的本机存档，完整性尚未确认，已保留。可尝试使用本机继续，或从历史选择已验证的进度。", "The previous session left an unarchived local save. It is retained, but its completeness has not been confirmed. Continue locally or select verified progress from history."));
            if(self->_startupProblem)status=self->_startupProblem;
            [self publish:status];
            dispatch_async(dispatch_get_main_queue(),^{
                self->_transferRunning=NO;
                if(configuration!=self->_configuration){[self refresh];return;}
                [NSNotificationCenter.defaultCenter postNotificationName:TOTKProgressDidCheck object:self];
                if (self->_start&&!self->_starting) {
                    if(all.count)totk_progress_offer_wait(self,self->_host);
                    else [self reconcileStartup];
                }
                if (self->_needsTransfer) {self->_needsTransfer=NO;[self refresh];}
            });
        }});
    }];
}
- (void)reconcileStartup {
    dispatch_async(_storeQueue,^{ @autoreleasepool {
        if (self.gameRunning) return;
        if (self->_localRecoveryRequired||self->_startupProblem) {dispatch_async(dispatch_get_main_queue(),^{totk_progress_offer_wait(self,self->_host);});return;}
        NSArray *heads=self->_store.heads;NSError *error=nil;
        NSDictionary *pending=[NSUserDefaults.standardUserDefaults dictionaryForKey:PendingKey];
        BOOL applied=NO;
        if (pending) {
            if ([pending[@"restore"] boolValue]) applied=[self->_store restoreVersion:pending[@"version"] error:&error]!=nil;
            else applied=[self->_store checkout:pending[@"version"] resolving:YES error:&error];
            if (applied) [NSUserDefaults.standardUserDefaults removeObjectForKey:PendingKey];
        } else if (heads.count>1) {
            NSDictionary *first=self->_store.versions[heads.firstObject];BOOL identical=YES;
            for(NSString *head in heads)if(![self->_store.versions[head][@"tree"] isEqual:first[@"tree"]]||![self->_store.versions[head][@"version"] isEqual:first[@"version"]])identical=NO;
            if(identical)applied=[self->_store checkout:heads.firstObject resolving:YES error:&error];
        } else if (heads.count==1 && (!self->_store.baseVersion||[self->_store isDescendant:heads.firstObject of:self->_store.baseVersion])) {
            applied=[self->_store checkout:heads.firstObject resolving:NO error:&error];
        } else if (heads.count<=1) applied=YES;
        NSURL *ready=self->_store.workingProfile?:self->_sourceProfile;
        if (error) [self publish:TOTKProgressErrorMessage(error)];
        dispatch_async(dispatch_get_main_queue(),^{
            if (self.gameRunning) return;
            self->_readyProfile=ready;
            if (applied&&!error) [self continueLocal];
            else { [self showFrom:self->_host];totk_progress_offer_wait(self,self->_host); }
        });
    }});
}
- (void)saveCommitted {
    dispatch_async(_storeQueue,^{ @autoreleasepool {
        if (!self->_store) return;
        uint64_t generation=0;
        if (!self.gameRunning || !runtime_fs_progress_snapshot_begin(&generation)) return;
        if (generation==self->_lastCapturedGeneration) {runtime_fs_progress_snapshot_end();return;}
        NSError *error=nil;
        NSURL *profile=self->_readyProfile?:self->_sourceProfile;
        NSString *saved=[self->_store captureProfile:profile error:&error];
        runtime_fs_progress_snapshot_end();
        if (saved) {self->_lastCapturedGeneration=generation;self->_localRecoveryRequired=NO;[self finishWaitingBackups];[self publish:self->_enabled&&self->_folderAccessible?@(TOTK_UI("本机已保存，正在提交 iCloud；不代表其他设备已收到。", "Saved locally and uploading to iCloud. This does not mean other devices have received it.")):@(TOTK_UI("本机已保存，云同步未开启；本地历史已保留。", "Saved locally. Cloud sync is not enabled; local history is retained."))];}
        else if (error) [self publish:TOTKProgressErrorMessage(error)];
        dispatch_async(dispatch_get_main_queue(),^{if(saved)[self refresh];});
    }});
}
- (void)sealCommittedGeneration:(uint64_t)generation {
    dispatch_sync(_storeQueue,^{ @autoreleasepool {
        if (!self->_store || !self.gameRunning || generation==self->_lastCapturedGeneration) return;
        NSError *error=nil;
        NSString *saved=[self->_store captureProfile:self->_readyProfile?:self->_sourceProfile error:&error];
        if (saved) {
            self->_lastCapturedGeneration=generation;
            self->_localRecoveryRequired=NO;
            [self finishWaitingBackups];
            [self publish:self->_enabled&&self->_folderAccessible?@(TOTK_UI("本机已保存，正在提交 iCloud；不代表其他设备已收到。", "Saved locally and uploading to iCloud. This does not mean other devices have received it.")):@(TOTK_UI("本机已保存，云同步未开启；本地历史已保留。", "Saved locally. Cloud sync is not enabled; local history is retained."))];
            dispatch_async(dispatch_get_main_queue(),^{[self refresh];});
        } else if (error) [self publish:[@(TOTK_UI("游戏存档仍在本机，但未能保留同步版本：", "The game save remains local, but its sync version could not be retained: ")) stringByAppendingString:TOTKProgressErrorMessage(error)]];
    }});
}
- (void)selectVersion:(NSString *)version restore:(BOOL)restore {
    dispatch_async(_storeQueue,^{
        NSError *error=nil;
        if(!self->_store||![self->_store canLoadVersion:version error:&error]){[self publish:TOTKProgressErrorMessage(error)?:@(TOTK_UI("此版本暂不可用，请先完成下载。", "This version is unavailable. Finish downloading it first."))];return;}
        if (self.gameRunning) {
            [NSUserDefaults.standardUserDefaults setObject:@{@"version":version,@"restore":@(restore)} forKey:PendingKey];
            [self publish:@(TOTK_UI("已安排在下次启动时载入；当前游戏继续使用本机进度。", "Selected for the next launch. The current game continues using local progress."))];return;
        }
        BOOL ok=restore?[self->_store restoreVersion:version error:&error]!=nil:[self->_store checkout:version resolving:YES error:&error];
        if (!ok) {[self publish:TOTKProgressErrorMessage(error)];return;}
        self->_localRecoveryRequired=NO;
        self->_startupProblem=nil;
        NSURL *ready=self->_store.workingProfile;[self publish:@(TOTK_UI("所选完整进度已准备好，其他版本仍保留。", "The selected complete progress is ready. Other versions are retained."))];
        dispatch_async(dispatch_get_main_queue(),^{self->_readyProfile=ready;[self continueLocal];[self refresh];});
    });
}
- (void)nameBackup:(NSString *)name version:(NSString *)version replacing:(NSString *)backupID {
    dispatch_async(_storeQueue,^{NSError *error=nil;
        NSString *trimmed=[name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if(!trimmed.length||trimmed.length>120){[self publish:@(TOTK_UI("请输入 1 至 120 个字符的备份名称。", "Enter a backup name of 1 to 120 characters."))];return;}
        if(!version&&self.gameRunning&&runtime_fs_progress_save_pending()) {
            NSMutableArray *waiting=[[NSUserDefaults.standardUserDefaults arrayForKey:WaitingBackupsKey] mutableCopy]?:[NSMutableArray new];
            [waiting addObject:trimmed];[NSUserDefaults.standardUserDefaults setObject:waiting forKey:WaitingBackupsKey];
            [self publish:@(TOTK_UI("正在保存游戏，完成后自动为本次存档保留命名备份。", "The game is saving. A named backup of this save will be kept when saving finishes."))];return;
        }
        NSString *target=version?:self->_store.baseVersion;
        if (target&&[self->_store nameBackup:trimmed forVersion:target replacing:backupID error:&error]) [self publish:self->_enabled?@(TOTK_UI("命名备份已在本机保留，正在同步。", "The named backup is retained locally and is syncing.")):@(TOTK_UI("命名备份已在本机保留，联网同步开启后补传。", "The named backup is retained locally and will upload when online sync is enabled."))];else [self publish:TOTKProgressErrorMessage(error)?:@(TOTK_UI("尚无完整的已保存进度。", "No complete saved progress is available yet."))];
        dispatch_async(dispatch_get_main_queue(),^{[self refresh];});
    });
}
- (void)finishWaitingBackups {
    NSArray *waiting=[NSUserDefaults.standardUserDefaults arrayForKey:WaitingBackupsKey];if(!waiting.count)return;
    NSMutableArray *remaining=[NSMutableArray new];
    for(NSString *name in waiting)if(![self->_store nameBackup:name forVersion:self->_store.baseVersion replacing:nil error:nil])[remaining addObject:name];
    if(remaining.count)[NSUserDefaults.standardUserDefaults setObject:remaining forKey:WaitingBackupsKey];else [NSUserDefaults.standardUserDefaults removeObjectForKey:WaitingBackupsKey];
}
- (void)showFrom:(id)host {
    _host=host?:_host;
    if (!_hasStore&&_sourceProfile) [self initializeStore];
    totk_progress_show_manager(self,host?:_host);
}
- (void)chooseFolderFrom:(id)host {totk_progress_choose_folder(self,host?:_host);}
@end
void totk_progress_save_committed(void) { [[TOTKProgressSync shared] saveCommitted]; }
void totk_progress_save_boundary(uint64_t generation) { [[TOTKProgressSync shared] sealCommittedGeneration:generation]; }
