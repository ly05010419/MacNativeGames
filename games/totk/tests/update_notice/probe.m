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
 setenv("MNG_CHECK_UPDATES","0",1);mng_update_notice(@"/unused");
 puts("PASS: revisions, paths, malformed comparisons, fork divergence, renames, truncation, deadline and opt-out");
}}
