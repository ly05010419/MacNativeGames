// tests/test_archive_map.py builds this: nvn_archive_map.h on this Mac. Prints "ok <check>" / "FAIL <check>: <detail>",
// "note ..." lines and "done <failures>". Arguments: real .metalarchive files to open (on an external drive for the
// footprint check), may be none.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include "nvn_archive_map.h"

static int fails;
static void expect(const char *name, int ok, NSString *detail) {
    printf("%s %s%s%s\n", ok ? "ok" : "FAIL", name, ok ? "" : ": ", ok ? "" : detail.UTF8String);
    if (!ok) fails++;
}
static uint64_t footprint(void) {
    task_vm_info_data_t i; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&i, &n) == KERN_SUCCESS ? i.phys_footprint : 0;
}
static double open_all(id<MTLDevice> dev, NSArray<NSString *> *paths, int mapped, NSMutableArray *keep) {
    uint64_t f0 = footprint();
    for (NSString *p in paths) {
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new]; d.url = [NSURL fileURLWithPath:p];
        NSError *e = nil;
        if (mapped) arch_map_begin();
        id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:d error:&e];
        if (mapped) arch_map_end();
        if (a) [keep addObject:a];
    }
    return ((double)footprint() - (double)f0) / 1048576.0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSUUID UUID].UUIDString];
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *arch = [dir stringByAppendingPathComponent:@"a.metalarchive"], *other = [dir stringByAppendingPathComponent:@"b.bin"];
        [[NSData dataWithBytes:"0123456789abcdef" length:16] writeToFile:arch atomically:YES];
        [[NSData dataWithBytes:"0123456789abcdef" length:16] writeToFile:other atomically:YES];

        uint64_t c0 = arch_map_count();
        (void)[[NSData alloc] initWithContentsOfURL:[NSURL fileURLWithPath:arch] options:NSDataReadingMappedIfSafe error:nil];
        expect("outside an open the read keeps its options", arch_map_count() == c0, @"");
        arch_map_begin();
        NSData *d = [[NSData alloc] initWithContentsOfURL:[NSURL fileURLWithPath:arch] options:NSDataReadingMappedIfSafe error:nil];
        uint64_t c1 = arch_map_count();
        (void)[[NSData alloc] initWithContentsOfURL:[NSURL fileURLWithPath:other] options:NSDataReadingMappedIfSafe error:nil];
        arch_map_end();
        expect("an archive read inside an open is mapped", c1 == c0 + 1 && d.length == 16, [NSString stringWithFormat:@"%llu", (unsigned long long)(c1 - c0)]);
        expect("another file inside an open keeps its options", arch_map_count() == c1, @"");
        arch_map_end();   // unbalanced: never below zero
        (void)[[NSData alloc] initWithContentsOfURL:[NSURL fileURLWithPath:arch] options:0 error:nil];
        expect("an extra end leaves reads alone", arch_map_count() == c1, @"");
        __block uint64_t seen = 0;
        arch_map_begin();
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSThread *t = [[NSThread alloc] initWithBlock:^{   // another thread (dispatch_sync may run on this one), not in an open
            uint64_t before = arch_map_count();
            (void)[[NSData alloc] initWithContentsOfURL:[NSURL fileURLWithPath:arch] options:NSDataReadingMappedIfSafe error:nil];
            seen = arch_map_count() - before;
            dispatch_semaphore_signal(done);
        }];
        [t start]; dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        arch_map_end();
        expect("an open on one thread does not change another thread's reads", seen == 0, @"");
        [NSFileManager.defaultManager removeItemAtPath:dir error:nil];

        NSMutableArray<NSString *> *files = [NSMutableArray array];
        for (int i = 1; i < argc; i++) [files addObject:@(argv[i])];
        if (files.count >= 4) {
            id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSMutableArray *keep = [NSMutableArray array];
            NSUInteger half = files.count / 2;
            double read = open_all(dev, [files subarrayWithRange:NSMakeRange(0, half)], 0, keep);
            double mapped = open_all(dev, [files subarrayWithRange:NSMakeRange(half, files.count - half)], 1, keep);
            printf("note %lu archives each way: footprint +%.1f MB read, +%.1f MB mapped\n", (unsigned long)half, read, mapped);
            expect("archives opened mapped add far less to the footprint", mapped < read * 0.5, [NSString stringWithFormat:@"read %.1f MB, mapped %.1f MB", read, mapped]);
        }
        printf("done %d\n", fails);
        return fails != 0;
    }
}
