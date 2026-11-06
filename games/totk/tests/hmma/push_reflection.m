#import <Foundation/Foundation.h>
#import "nvn_refl.h"
#include <stdio.h>
int main(void) { @autoreleasepool {
    NSArray *bindings = @[
        @{@"kind":@"drvcb", @"bank":@24, @"arg_id":@0},
        @{@"kind":@"cbuf", @"reg":@24, @"arg_id":@1},
        @{@"kind":@"drvcb", @"bank":@0, @"arg_id":@2},
        @{@"kind":@"drvcb", @"bank":@2, @"arg_id":@3}];
    nvn_refl refl; char error[256];
    if(nvn_refl_from_json(@{@"route":@"B", @"bindings":bindings}, &refl, error, sizeof error)) { fprintf(stderr,"%s\n",error); return 1; }
    if(refl.nb != 4 || refl.b[0].kind != NVN_RB_CBUF || refl.b[0].reg != 24 || refl.b[1].kind != NVN_RB_CBUF || refl.b[1].reg != 24 || refl.b[2].kind != NVN_RB_DRV0 || refl.b[3].kind != NVN_RB_DRV2) return 2;
    puts("PASS: cached and new push bindings; driver banks 0 and 2 preserved");
} }
