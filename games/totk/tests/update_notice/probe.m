#include "../../app/update_notice.h"
#include <assert.h>
int main(void){@autoreleasepool {
 assert(mng_revision(@"0123456789012345678901234567890123456789"));
 for(id value in @[@1, @"bad", @"../heads/main", @"ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890123"])
  assert(!mng_revision(value));
 assert(!mng_revision(nil));
 assert(!mng_relevant_path(@"docs/readme.md"));
 assert(!mng_relevant_path(@"games/totk/ios/Sources/main.m"));
 assert(!mng_relevant_path(@"games/totk/tests/probe.c"));
 assert(!mng_relevant_path(@"games/odyssey/runtime/input.c"));
 assert(mng_relevant_path(@"games/totk/runtime/frontend.m"));
 assert(mng_relevant_path(@"build_app.py"));
 assert(!mng_comparison_has_update(nil));
 assert(!mng_comparison_has_update(@[]));
 NSMutableDictionary *d=[@{@"status":@"ahead", @"ahead_by":@1, @"files":@[@{@"filename":@"docs/a.md"}]} mutableCopy];
 assert(!mng_comparison_has_update(d));
 d[@"files"]=@[@{@"filename":@"games/totk/runtime/input.c"}];assert(mng_comparison_has_update(d));
 d[@"status"]=@"diverged";assert(mng_comparison_has_update(d));
 d[@"ahead_by"]=@0;assert(!mng_comparison_has_update(d));
 d[@"ahead_by"]=@1;d[@"status"]=@"behind";assert(!mng_comparison_has_update(d));
 d[@"status"]=@"ahead";d[@"files"]=@[@{@"filename":@"archived.c",@"previous_filename":@"games/totk/runtime/old.c"}];assert(mng_comparison_has_update(d));
 d[@"files"]=@[@1, @"bad", @{}];assert(!mng_comparison_has_update(d));
 NSMutableArray *files=[NSMutableArray new];for(int i=0;i<300;i++)[files addObject:@{@"filename":@"docs/a.md"}];
 d[@"files"]=files;assert(mng_comparison_has_update(d));
 assert(!mng_public_json(@"git/ref/heads/main",NSProcessInfo.processInfo.systemUptime-1));
 NSString *base=@"0123456789012345678901234567890123456789", *head=@"abcdefabcdefabcdefabcdefabcdefabcdefabcd";
 NSString *folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 NSFileManager *fm=NSFileManager.defaultManager;
 assert([fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]);
 NSString *path=mng_update_state_path(folder);
 __block int requests=0;
 MNGUpdateFetch fetch=^NSDictionary *(NSString *query,double deadline){
  (void)deadline;requests++;
  if([query hasPrefix:@"git/"])return @{@"object":@{@"sha":head}};
  return @{@"status":@"ahead",@"ahead_by":@1,@"files":@[@{@"filename":@"games/totk/runtime/input.c"}]};
 };
 assert(mng_update_due(nil,100));assert(mng_update_due(@"bad",100));
 assert(!mng_update_due(@100,99));assert(!mng_update_due(@100,86499));assert(mng_update_due(@100,86500));
 mng_update_refresh(folder,base,100,fetch);assert(requests==2);
 NSDictionary *state=[NSDictionary dictionaryWithContentsOfFile:path];
 assert([mng_cached_update(state,base,nil) isEqual:head]);
 assert(!mng_cached_update(state,head,nil));assert(!mng_cached_update(state,base,head));
 assert(!mng_cached_update(@{@"base":base,@"head":@1},base,nil));
 mng_update_refresh(folder,base,86499,fetch);assert(requests==2);
 // Failed/offline attempts throttle too and preserve a previously useful notice.
 MNGUpdateFetch offline=^NSDictionary *(NSString *q,double t){(void)q;(void)t;requests++;return nil;};
 mng_update_refresh(folder,base,86500,offline);assert(requests==3);
 mng_update_refresh(folder,base,86501,offline);assert(requests==3);
 state=[NSDictionary dictionaryWithContentsOfFile:path];assert([mng_cached_update(state,base,nil) isEqual:head]);
 // A concurrent checker neither waits for the lock nor sends another request.
 int lock=open([folder stringByAppendingPathComponent:@"port-update.lock"].fileSystemRepresentation,O_RDWR);
 assert(lock>=0&&!flock(lock,LOCK_EX|LOCK_NB));
 mng_update_refresh(folder,base,172900,fetch);assert(requests==3);close(lock);
 // A skipped revision cannot be overwritten by a later worker/cache refresh.
 assert([head writeToFile:mng_update_skip_path(folder) atomically:YES encoding:NSUTF8StringEncoding error:nil]);
 mng_update_refresh(folder,base,172900,fetch);assert(requests==5);
 state=[NSDictionary dictionaryWithContentsOfFile:path];assert(!mng_cached_update(state,base,mng_update_skipped(folder)));
 // A successful check against an already installed head clears the stale notice.
 mng_update_refresh(folder,head,259300,fetch);assert(requests==6);
 state=[NSDictionary dictionaryWithContentsOfFile:path];assert(!state[@"head"]);
 // Hold the fake network indefinitely: scheduling must return before it completes.
 assert([fm removeItemAtPath:path error:nil]);
 dispatch_semaphore_t entered=dispatch_semaphore_create(0), release=dispatch_semaphore_create(0);
 dispatch_group_t completed=dispatch_group_create();dispatch_group_enter(completed);
 mng_update_schedule(folder,base,^NSDictionary *(NSString *q,double t){
  (void)q;(void)t;dispatch_semaphore_signal(entered);
  dispatch_semaphore_wait(release,DISPATCH_TIME_FOREVER);dispatch_group_leave(completed);return nil;
 });
 assert(!dispatch_semaphore_wait(entered,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC)));
 dispatch_semaphore_signal(release);
 assert(!dispatch_group_wait(completed,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC)));
 // Synchronize with the worker's final lock release before deleting the fixture.
 lock=open([folder stringByAppendingPathComponent:@"port-update.lock"].fileSystemRepresentation,O_RDWR);
 assert(lock>=0&&!flock(lock,LOCK_EX));close(lock);
 setenv("MNG_CHECK_UPDATES","0",1);mng_update_notice(@"/unused");
 mng_update_refresh(folder,base,345700,fetch);assert(requests==6);
 assert([fm removeItemAtPath:folder error:nil]);
 puts("PASS: metadata, cached notice, 24-hour attempt throttle, offline retry, concurrent lock, skip, rebuild, background dispatch and opt-out");
}}
