#import "../third_party/d2r_gfx/gfx/nvn/nvn_deferred_queue.h"
#include <assert.h>
#include <stdio.h>

@implementation NvDeferredRelease @end
static unsigned destroyed;
@interface Token : NSObject { @public unsigned index; } @end
@implementation Token
- (void)dealloc { destroyed++; }
@end

int main(void) { @autoreleasepool {
    const unsigned count = 100000;
    NSMutableArray<NvDeferredRelease *> *queue = [NSMutableArray array];
    for (unsigned i = 0; i < count; i++) { @autoreleasepool {
        NvDeferredRelease *entry = [NvDeferredRelease new];
        entry->fence = i % 3 + 7; entry->keep_resident = i & 1;
        Token *token = [Token new]; token->index = i;
        entry->object = token; [queue addObject:entry];
    }}
    uint64_t minimum = 7;
    NSMutableArray<NvDeferredRelease *> *ready = [NSMutableArray array];
    nvn_deferred_partition(queue, 6, ready, &minimum);
    assert(queue.count == count && !ready.count && minimum == 7 && !destroyed);
    nvn_deferred_partition(queue, 7, ready, &minimum);
    assert(ready.count == (count + 2) / 3 && minimum == 8 && !destroyed);
    unsigned r = 0, p = 0;
    for (unsigned i = 0; i < count; i++) {
        NvDeferredRelease *entry = i % 3 == 0 ? ready[r++] : queue[p++];
        assert(entry->fence == i % 3 + 7 && entry->keep_resident == (int)(i & 1));
        assert(entry->object);
        assert(((Token *)entry->object)->index == i);
    }
    for (NvDeferredRelease *entry in ready.reverseObjectEnumerator) entry->object = nil;
    assert(destroyed == ready.count); [ready removeAllObjects];
    nvn_deferred_partition(queue, 8, ready, &minimum);
    assert(minimum == 9 && destroyed == (count + 2) / 3);
    for (NvDeferredRelease *entry in ready) { assert(entry->fence == 8); entry->object = nil; }
    [ready removeAllObjects];
    nvn_deferred_partition(queue, 9, ready, &minimum);
    assert(!queue.count && minimum == UINT64_MAX);
    for (NvDeferredRelease *entry in ready) { assert(entry->fence == 9); entry->object = nil; }
    assert(destroyed == count);
    puts("deferred queue: 100000 interleaved fences, reference lifetime and stable partition PASS");
} return 0; }
