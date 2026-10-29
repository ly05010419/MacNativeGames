/* Production presentation boundary and localized messages; no windows or real saves. */
#import "../games/totk/runtime/progress_ui.m"
#include <stdio.h>
#include <stdlib.h>
static NSUInteger checks;
#define CHECK(value,note) do {checks++;if(!(value)){fprintf(stderr,"FAIL: %s\n",note);exit(1);}} while(0)
void runtime_fs_progress_enable(void) {}
int runtime_fs_progress_snapshot_begin(uint64_t *generation) {(void)generation;return 0;}
void runtime_fs_progress_snapshot_end(void) {}
int runtime_fs_progress_save_pending(void) {return 0;}
@interface ForbiddenHost:NSObject @end
@implementation ForbiddenHost
- (BOOL)isKindOfClass:(Class)type {(void)type;CHECK(NO,"running game must not even resolve a presentation host");return NO;}
@end
int main(void) {@autoreleasepool {
    TOTKProgressSync *sync=TOTKProgressSync.shared;
    [sync setValue:@YES forKey:@"gameRunning"];
    totk_progress_offer_wait(sync,[ForbiddenHost new]);
    CHECK(sync.gameRunning,"offering startup UI while running is ignored");
    for(NSString *language in @[@"en",@"zh"]) {
        setenv("TOTK_UI_LANG",language.UTF8String,1);
        BOOL zh=[language isEqual:@"zh"];
        NSDictionary *record=@{@"created":@0,@"kind":@"choice",@"device":@"00000000-0000-0000-0000-000000000001",@"version":@"1.4.3",@"bytes":@16};
        CHECK([versionTitle(record) containsString:zh?@"已选择进度":@"Selected progress"],"version title follows UI language");
        CHECK([versionDetail(record) containsString:zh?@"其他设备":@"Other device"],"device fallback follows UI language");
        NSString *status=statusText(@{@"enabled":@NO,@"restartToContinue":@YES,@"pendingAction":@{},@"currentReceivers":@[],@"waitingBackups":@1,@"historyRecordBytes":@16});
        CHECK([status containsString:zh?@"云同步已暂停":@"Cloud sync paused"],"paused status follows UI language");
        CHECK([status containsString:zh?@"重新打开":@"reopen"],"restart message follows UI language");
        CHECK([status containsString:zh?@"正在等待游戏保存":@"waiting for the game"],"pending backup message follows UI language");
        CHECK([status containsString:zh?@"历史记录文件":@"Local history records"],"history size message follows UI language");
        NSString *error=TOTKProgressErrorMessage([NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteOutOfSpaceError userInfo:nil]);
        CHECK([error containsString:zh?@"释放空间":@"Free space"],"recovery alert follows UI language");
    }
    printf("PASS: %lu progress UI checks (bilingual, no windows)\n",(unsigned long)checks);
}}
