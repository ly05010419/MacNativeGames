/* Stable O(n) partition of fence-owned references. The caller holds its queue
 * mutex. Ready entries remain strongly held until the caller handles residency
 * and releases them outside that mutex. A minimum fence skips unchanged queues.
 * Same root issue as seedds/nxnative #1312 (MIT), independently integrated into
 * this runtime's keep_resident and completion-fence ownership model. */
#import <Foundation/Foundation.h>
#include <stdint.h>

@interface NvDeferredRelease : NSObject { @public id object; uint64_t fence; int keep_resident; } @end

static inline void nvn_deferred_partition(NSMutableArray<NvDeferredRelease *> *queue,
                                        uint64_t done, NSMutableArray<NvDeferredRelease *> *ready,
                                        uint64_t *minimum) {
    NSUInteger count = queue.count, kept = 0;
    uint64_t next = UINT64_MAX;
    for (NSUInteger i = 0; i < count; i++) {
        __unsafe_unretained NvDeferredRelease *entry = queue[i];
        if (entry->fence <= done) [ready addObject:entry];
        else {
            if (entry->fence < next) next = entry->fence;
            if (kept != i) queue[kept] = entry;
            kept++;
        }
    }
    if (kept != count) [queue removeObjectsInRange:NSMakeRange(kept, count - kept)];
    *minimum = next;
}
