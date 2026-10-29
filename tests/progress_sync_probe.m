/* Real coordinator/store/transport, synthetic profiles and a local cloud fixture only. */
#import "../games/totk/runtime/progress_sync.h"
#import "../games/totk/runtime/progress_store.h"
#import "../games/totk/runtime/progress_errors.h"
#include <unistd.h>
static NSUInteger checks, starts, waits, managers;
static uint64_t generation;
static BOOL frozen;
static BOOL pendingSave;
static NSUInteger enableCalls;
@interface DelayedTransport:NSObject
@property(copy) void (^completion)(NSArray *,NSArray *);
- (void)exchangeWithFolder:(NSURL *)folder completion:(void (^)(NSArray *,NSArray *))completion;
@end
@implementation DelayedTransport
- (void)exchangeWithFolder:(NSURL *)folder completion:(void (^)(NSArray *,NSArray *))completion {(void)folder;self.completion=completion;}
@end
#define CHECK(c,n) do { checks++; if(!(c)){fprintf(stderr,"FAIL: %s\n",n);exit(1);} } while(0)
void runtime_fs_progress_enable(void) {enableCalls++;}
int runtime_fs_progress_snapshot_begin(uint64_t *epoch) { if(frozen||!generation)return 0;frozen=YES;*epoch=generation;return 1; }
void runtime_fs_progress_snapshot_end(void) {frozen=NO;}
int runtime_fs_progress_save_pending(void) {return pendingSave;}
void totk_progress_show_manager(TOTKProgressSync *sync,id host) {(void)sync;(void)host;managers++;}
void totk_progress_choose_folder(TOTKProgressSync *sync,id host) {(void)sync;(void)host;}
void totk_progress_offer_wait(TOTKProgressSync *sync,id host) {(void)sync;(void)host;waits++;}
static NSURL *at(NSURL *root,NSString *path) {return [root URLByAppendingPathComponent:path];}
static NSString *const Save=@"save/00000000000000020000000000000001/slot_00/progress.sav";
static void payload(NSURL *profile,NSString *text) {
    [NSFileManager.defaultManager createDirectoryAtURL:at(profile,Save).URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    uint64_t words[]={1,2};CHECK([[NSData dataWithBytes:words length:16] writeToURL:at(profile,@"account.uid") atomically:YES],"identity fixture");
    CHECK([text writeToURL:at(profile,Save) atomically:YES encoding:NSUTF8StringEncoding error:nil],"save fixture");
}
static NSString *contents(NSURL *profile) {return [NSString stringWithContentsOfURL:at(profile,Save) encoding:NSUTF8StringEncoding error:nil];}
static void until(BOOL (^done)(void)) {
    NSDate *limit=[NSDate dateWithTimeIntervalSinceNow:8];
    while(!done()&&limit.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    CHECK(done(),"asynchronous coordinator reached expected state");
}
int main(int argc,const char **argv) { @autoreleasepool {
    CHECK(argc==3,"scenario and fixture path");NSString *scenario=@(argv[1]);NSURL *root=[NSURL fileURLWithPath:@(argv[2])];
    if([scenario isEqual:@"errors"]) {
        NSError *early=nil;[TOTKProgressSync.shared useFolder:root error:&early];
        CHECK([early.localizedDescription containsString:@(TOTK_UI("仍在准备","still preparing"))],"folder selection before game preparation has a recoverable error");
        CHECK(!enableCalls&&!starts,"early folder choice cannot enable sync or launch an unprepared game");
        CHECK([TOTKProgressErrorMessage([NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteOutOfSpaceError userInfo:nil]) containsString:@(TOTK_UI("释放空间","Free space"))],"local space error offers recovery");
        CHECK([TOTKProgressErrorMessage([NSError errorWithDomain:@"NSFileProviderErrorDomain" code:NSFileProviderErrorInsufficientQuota userInfo:nil]) containsString:@(TOTK_UI("iCloud 空间不足","iCloud storage is full"))],"provider quota error distinguished");
        CHECK([TOTKProgressErrorMessage([NSError errorWithDomain:NSCocoaErrorDomain code:NSFileReadNoPermissionError userInfo:nil]) containsString:@(TOTK_UI("重新选择","choose a writable sync folder again"))],"permission error has reauthorization exit");
        CHECK([TOTKProgressErrorMessage([NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorNotConnectedToInternet userInfo:nil]) containsString:@(TOTK_UI("网络恢复","network returns"))],"offline error does not claim upload success");
        CHECK([TOTKProgressErrorMessage([NSError errorWithDomain:@"wrapper" code:0 userInfo:@{NSUnderlyingErrorKey:[NSError errorWithDomain:NSPOSIXErrorDomain code:ENOSPC userInfo:nil]}]) containsString:@(TOTK_UI("空间不足","Storage is full"))],"underlying ENOSPC retained through wrappers");
        CHECK(TOTKProgressErrorMessage(nil)==nil,"nil error remains nil for caller fallback");
        printf("PASS: %lu error recovery message checks\n",(unsigned long)checks);return 0;
    }
    NSURL *profile=at(root,@"original"),*localRoot=at(root,@"local"),*cloud=at(root,@"cloud");
    payload(profile,@"seed");
    NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
    [defaults setVolatileDomain:@{@"TOTK_PROGRESS_DEVICE_ID":@"local-test",@"TOTK_PROGRESS_ENABLED":@YES} forName:NSArgumentDomain];
    [defaults removeObjectForKey:@"TOTK_PROGRESS_PENDING_ACTION"];
    setenv("TOTK_PROGRESS_ROOT",localRoot.fileSystemRepresentation,1);
    __block NSString *base,*remoteID,*branchID;
    @autoreleasepool {
        NSError *error=nil;
        TOTKProgressStore *local=[[TOTKProgressStore alloc] initWithRoot:localRoot device:@"local-test" gameVersion:@"1.4.3" error:&error];
        CHECK(local!=nil,"local store created");base=[local captureProfile:profile error:&error];CHECK(base!=nil,"initial version");
        CHECK([local exchangeWithFolder:cloud error:&error],"initial cloud fixture");
        TOTKProgressStore *remote=[[TOTKProgressStore alloc] initWithRoot:at(root,@"remote") device:@"remote-test" gameVersion:@"1.4.3" error:&error];
        CHECK([remote exchangeWithFolder:cloud error:&error]&&[remote checkout:base resolving:NO error:&error],"remote gets base");
        payload(remote.workingProfile,@"remote-next");remoteID=[remote captureProfile:remote.workingProfile error:&error];CHECK(remoteID!=nil,"remote next version");
        CHECK([remote exchangeWithFolder:cloud error:&error],"publish remote fixture");
        if([scenario isEqual:@"fork"]) {
            CHECK([local checkout:base resolving:NO error:&error],"local fork setup");payload(local.workingProfile,@"local-branch");branchID=[local captureProfile:local.workingProfile error:&error];CHECK(branchID!=nil,"local branch");
        }
        if([scenario isEqual:@"restore"]) {
            CHECK([local exchangeWithFolder:cloud error:&error]&&[local checkout:remoteID resolving:NO error:&error],"restore has prior and current");
            [defaults setObject:@{@"version":base,@"restore":@YES} forKey:@"TOTK_PROGRESS_PENDING_ACTION"];
        }
    }
    BOOL offline=[@[@"running",@"restore",@"commit",@"retry",@"recovery",@"backup",@"pause",@"invalid",@"pending-error",@"enable-during-game",@"management-new"] containsObject:scenario];
    [defaults setVolatileDomain:@{@"TOTK_PROGRESS_DEVICE_ID":@"local-test",@"TOTK_PROGRESS_ENABLED":@(!offline)} forName:NSArgumentDomain];
    NSData *goodState=[NSData dataWithContentsOfURL:at(localRoot,@"state.json")];
    if([scenario isEqual:@"enable-during-game"]||[scenario isEqual:@"management-new"])CHECK([NSFileManager.defaultManager removeItemAtURL:localRoot error:nil],"fresh disabled feature fixture");
    if([scenario isEqual:@"recovery"]) {
        @autoreleasepool {
            TOTKProgressStore *local=[[TOTKProgressStore alloc] initWithRoot:localRoot device:@"local-test" gameVersion:@"1.4.3" error:nil];
            CHECK([local checkout:base resolving:NO error:nil],"prepare incomplete working generation");payload(local.workingProfile,@"uncommitted partial work");
        }
    }
    if([scenario isEqual:@"retry"])[@"{}" writeToURL:at(localRoot,@"state.json") atomically:YES encoding:NSUTF8StringEncoding error:nil];
    if([scenario isEqual:@"pending-error"])[defaults setObject:@{@"version":@"00000000-0000-0000-0000-000000000001",@"restore":@YES} forKey:@"TOTK_PROGRESS_PENDING_ACTION"];
    NSString *badID=@"00000000-0000-0000-0000-000000000002";
    if([scenario isEqual:@"pending-inbox"])[@"[]" writeToURL:at(cloud,[NSString stringWithFormat:@"totk-progress-v1/%@.json",badID]) atomically:YES encoding:NSUTF8StringEncoding error:nil];
    TOTKProgressSync *sync=TOTKProgressSync.shared;[sync setValue:cloud forKey:@"folder"];
    if([scenario hasPrefix:@"management"]) {
        unsetenv("TOTK_SAVE_PROFILE_DIR");
        [sync prepareProfile:profile gameVersion:@"1.4.3" host:nil completion:nil];
        until(^BOOL{return [sync.viewState[@"localPrepared"] boolValue];});
        CHECK(!starts&&!sync.gameRunning&&!getenv("TOTK_SAVE_PROFILE_DIR"),"management launch never starts or mounts the game");
        CHECK(![sync.viewState[@"canContinueLocal"] boolValue],"management does not offer a start action without a game callback");
        NSString *expected=@"seed";
        if(!offline) {
            until(^BOOL{return [sync.viewState[@"versions"] count]==2;});
            CHECK([sync.viewState[@"base"] isEqual:base],"management receives remote records without starting a game or silently applying them");
            [sync selectVersion:remoteID restore:NO];
            until(^BOOL{
                for(NSDictionary *record in sync.viewState[@"versions"])
                    if([record[@"id"] isEqual:sync.viewState[@"base"]]&&[record[@"selected"] isEqual:remoteID])return YES;
                return NO;
            });
            expected=@"remote-next";
        }
        CHECK(!sync.gameRunning,"management selection stays in the manager");
        [sync nameBackup:@"管理页备份" version:nil replacing:nil];
        until(^BOOL{return [sync.viewState[@"backups"] count]==1;});
        CHECK(!sync.gameRunning,"naming a backup never starts the game");
        [sync prepareProfile:profile gameVersion:@"1.4.3" host:nil completion:^{starts++;}];
        until(^BOOL{return starts==1;});
        CHECK([contents([NSURL fileURLWithPath:@(getenv("TOTK_SAVE_PROFILE_DIR"))]) isEqual:expected],"explicit play after management starts the prepared complete version");
        CHECK([contents(profile) isEqual:@"seed"],"management and subsequent play preparation preserve the original profile");
        printf("PASS: %lu coordinator checks (%s, local fixtures only)\n",(unsigned long)checks,argv[1]);return 0;
    }
    [sync prepareProfile:profile gameVersion:@"1.4.3" host:nil completion:^{starts++;}];
    if([scenario isEqual:@"retry"]) {
        until(^BOOL{return [sync.viewState[@"status"] containsString:@(TOTK_UI("索引损坏","index is damaged"))];});CHECK(!starts,"corrupt state never launches empty game");
        CHECK([goodState writeToURL:at(localRoot,@"state.json") atomically:YES],"repair fixture index");[sync refresh];
    }
    if([scenario isEqual:@"pending-error"]) {
        until(^BOOL{return [sync.viewState[@"status"] containsString:@(TOTK_UI("安排的进度暂不能载入","selected progress cannot be loaded"))];});
        CHECK(!starts&&[sync.viewState[@"canContinueLocal"] boolValue],"failed pending load stays visible with local recovery available");
        [sync cancelPendingSelection];[sync continueLocal];
    }
    if([scenario isEqual:@"pending-inbox"]) {
        until(^BOOL{return [sync.viewState[@"status"] containsString:@(TOTK_UI("格式不匹配","format does not match"))];});
        CHECK(!starts,"malformed incoming record cannot silently start a different progress");
        CHECK([NSFileManager.defaultManager fileExistsAtPath:at(localRoot,[NSString stringWithFormat:@"incoming/%@.json",badID]).path],"unaccepted staging file retained for retry");
        [sync continueLocal];
    }
    if([scenario isEqual:@"recovery"]) {
        until(^BOOL{return [sync.viewState[@"localRecoveryRequired"] boolValue];});CHECK(!starts,"unarchived working changes never silently rolled back or launched");
        [sync continueLocal];until(^BOOL{return starts==1;});CHECK([contents([NSURL fileURLWithPath:@(getenv("TOTK_SAVE_PROFILE_DIR"))]) isEqual:@"uncommitted partial work"],"explicit local recovery preserves working generation");
    } else if([scenario isEqual:@"fork"]) {
        until(^BOOL{return [sync.viewState[@"heads"] count]==2;});CHECK(!starts&&!sync.gameRunning,"fork waits before game reads");
        [sync selectVersion:branchID restore:NO];until(^BOOL{return starts==1;});
        CHECK([contents([NSURL fileURLWithPath:@(getenv("TOTK_SAVE_PROFILE_DIR"))]) isEqual:@"local-branch"],"selected fork supplies runtime profile");
        until(^BOOL{return [sync.viewState[@"heads"] count]==1;});
        CHECK([sync.viewState[@"versions"] count]==4,"unselected branch and choice retained");
    } else {
        until(^BOOL{return starts==1;});NSURL *active=[NSURL fileURLWithPath:@(getenv("TOTK_SAVE_PROFILE_DIR"))];
        if([scenario isEqual:@"startup"]) {
            CHECK([contents(active) isEqual:@"remote-next"],"startup automatically applies verified descendant");
            CHECK(![NSFileManager.defaultManager fileExistsAtPath:at(localRoot,[NSString stringWithFormat:@"incoming/%@.json",remoteID]).path]&&[NSFileManager.defaultManager fileExistsAtPath:at(localRoot,[NSString stringWithFormat:@"records/%@.json",remoteID]).path],"verified durable record replaces temporary inbox copy");
        }
        if([scenario isEqual:@"restore"]) {
            CHECK([contents(active) isEqual:@"seed"],"pending restore applies offline before game starts");
            CHECK(![defaults objectForKey:@"TOTK_PROGRESS_PENDING_ACTION"],"successful pending action clears");
        }
        if([scenario isEqual:@"running"]) {
            CHECK(![sync.viewState[@"restartToContinue"] boolValue],"no restart prompt before receiving a successor");
            CHECK([contents(active) isEqual:@"seed"],"offline starts local");[sync setSynchronizationEnabled:YES];
            until(^BOOL{return [sync.viewState[@"versions"] count]>=2;});
            CHECK([contents(active) isEqual:@"seed"],"running game never hot replaced by remote");
            CHECK([sync.viewState[@"base"] isEqual:base],"remote receive does not advance running base");
            CHECK([sync.viewState[@"restartToContinue"] boolValue],"received compatible successor offers restart without changing running data");
            [sync refresh];until(^BOOL{return ![[sync valueForKey:@"transferRunning"] boolValue];});
            CHECK(!waits&&!managers,"foreground-style refresh only stages progress; no automatic prompt or manager while running");
            CHECK([sync.viewState[@"historyRecordBytes"] unsignedLongLongValue]>[sync.viewState[@"historyBytes"] unsignedLongLongValue],"history storage reports encoded record files rather than decoded payload size");
        }
        if([scenario isEqual:@"commit"]) {
            payload(active,@"saved-immediately-before-exit");generation=1;[sync sealCommittedGeneration:generation];
            NSDictionary *state=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:at(localRoot,@"state.json")] options:0 error:nil];
            CHECK(![state[@"base"] isEqual:base],"successful save boundary updates durable state synchronously");
            NSDictionary *record=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:at(at(localRoot,@"records"),[state[@"base"] stringByAppendingString:@".json"])] options:0 error:nil];
            NSData *bytes=[[NSData alloc] initWithBase64EncodedString:record[@"files"][Save][@"data"] options:0];
            CHECK([[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] isEqual:@"saved-immediately-before-exit"],"sealed payload available without draining async callbacks");
        }
        if([scenario isEqual:@"backup"]) {
            pendingSave=YES;[sync nameBackup:@"during save" version:nil replacing:nil];
            until(^BOOL{return [sync.viewState[@"status"] containsString:@(TOTK_UI("完成后","saving finishes"))];});CHECK([sync.viewState[@"backups"] count]==0,"backup waits instead of naming previous save");
            payload(active,@"save being named");pendingSave=NO;[sync sealCommittedGeneration:1];
            until(^BOOL{return [sync.viewState[@"backups"] count]==1;});
            CHECK([sync.viewState[@"backups"][0][@"target"] isEqual:sync.viewState[@"base"]],"deferred backup names completed new version");
        }
        if([scenario isEqual:@"enable-during-game"]) {
            CHECK(!enableCalls,"disabled new game has no managed save gate yet");
            [sync showFrom:nil];until(^BOOL{return enableCalls>0;});
            CHECK(![sync.viewState[@"versions"] count],"opening manager never captures potentially live uncommitted files");
            payload(active,@"first save after opening manager");[sync sealCommittedGeneration:1];
            until(^BOOL{return [sync.viewState[@"versions"] count]==1;});
            CHECK(![sync.viewState[@"enabled"] boolValue],"local history works without turning on cloud sync");
        }
        if([scenario isEqual:@"invalid"]) {
            [sync selectVersion:@"00000000-0000-0000-0000-000000000001" restore:YES];
            until(^BOOL{return [sync.viewState[@"status"] containsString:@(TOTK_UI("尚未到达","has not arrived yet"))];});
            CHECK(![defaults objectForKey:@"TOTK_PROGRESS_PENDING_ACTION"],"unknown version cannot be queued for next boot");
            [sync selectVersion:base restore:YES];until(^BOOL{return [sync.viewState[@"pendingAction"] count]>0;});
            CHECK([sync.viewState[@"base"] isEqual:base],"pending choice never changes active base");
            [sync cancelPendingSelection];until(^BOOL{return [sync.viewState[@"pendingAction"] count]==0;});
        }
        if([scenario isEqual:@"pause"]) {
            DelayedTransport *transport=[DelayedTransport new];[sync setValue:transport forKey:@"transport"];
            [sync setSynchronizationEnabled:YES];CHECK(transport.completion!=nil,"transfer starts on enabling");
            [sync setSynchronizationEnabled:NO];
            transport.completion(@[at(at(root,@"remote/records"),[remoteID stringByAppendingString:@".json"])],@[]);
            until(^BOOL{return ![[sync valueForKey:@"transferRunning"] boolValue];});
            CHECK([sync.viewState[@"versions"] count]==1,"late response after pause is not accepted");
            CHECK(![sync.viewState[@"enabled"] boolValue],"pause state remains visible");
            [sync setSynchronizationEnabled:YES];transport.completion(@[at(at(root,@"remote/records"),[remoteID stringByAppendingString:@".json"])],@[]);
            until(^BOOL{return [sync.viewState[@"versions"] count]==2;});
            CHECK([contents(active) isEqual:@"seed"]&&[sync.viewState[@"enabled"] boolValue],"resume accepts remote without replacing running data");
            CHECK([NSFileManager.defaultManager fileExistsAtPath:at(root,[NSString stringWithFormat:@"remote/records/%@.json",remoteID]).path],"cleanup never removes a source outside owned staging directory");
        }
    }
    CHECK(starts==1,"runtime launched exactly once");CHECK([contents(profile) isEqual:[scenario isEqual:@"enable-during-game"]?@"first save after opening manager":@"seed"],"original fixture only changes through an explicit game write");
    [defaults removeObjectForKey:@"TOTK_PROGRESS_PENDING_ACTION"];
    printf("PASS: %lu coordinator checks (%s, local fixtures only)\n",(unsigned long)checks,argv[1]);
    return 0;
}}
