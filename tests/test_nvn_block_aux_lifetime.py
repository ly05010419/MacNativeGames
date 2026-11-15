"""Actual cache/forget/publish source with Foundation allocations, without Metal/GPU.

The residency and deferred-fence model below is explicit: native blockrt coverage
must still verify real Metal membership and pixels. No renderer module is built.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'

PRELUDE = r'''
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "nvn_hmap.h"
#include "totk_serial_set.h"
@protocol MTLTexture <NSObject>
@property(readonly) NSUInteger allocatedSize;
@end
@interface TestTexture : NSObject <MTLTexture>
@property NSUInteger allocatedSize;
@end
static _Atomic unsigned destroyed;
@implementation TestTexture
- (void)dealloc { atomic_fetch_add(&destroyed, 1); }
@end
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t residence = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_treg_refs = {.klen=8};
static totk_serial_set g_dead_serial, g_gpu_owned;
static uint64_t g_serial_forgotten, g_fence=10;
static int pinned;
static NSMutableArray *deferred;
static NSMutableSet *resident;
static unsigned checked;
#define MLOG(...) do { printf(__VA_ARGS__); puts(""); } while(0)
static void check(int good, const char *label) {
    checked++;
    if (!good) { fprintf(stderr,"FAIL %s after %u checks\n",label,checked); exit(1); }
}
int dm_nvn_pin_has_serial(uint64_t serial) { (void)serial; return pinned; }
static void res_add(id texture) {
    pthread_mutex_lock(&residence); [resident addObject:texture]; pthread_mutex_unlock(&residence);
}
static void __attribute__((unused)) defer_obj_release(id texture) {
    [deferred addObject:@[texture,@(g_fence+1)]];
}
static void drain(uint64_t done) { @autoreleasepool {
    NSMutableArray *keep=[NSMutableArray array];
    for (NSArray *entry in deferred) {
        if ([entry[1] unsignedLongLongValue]<=done) [resident removeObject:entry[0]];
        else [keep addObject:entry];
    }
    deferred=keep;
} }
'''

FIXTURE = r'''
static void publish(uint64_t serial, id<MTLTexture> a) {
    struct { uint64_t serial; } value={serial}, *t=&value;
    NSNumber *key=@(serial);
    res_add(a);
@PUBLISH@
}
static void forget(uint64_t serial) {
    pthread_mutex_lock(&registry); serial_forget_locked_rmu(serial); pthread_mutex_unlock(&registry);
}
static void *worker(void *index) {
    for (unsigned i=0;i<500;i++) @autoreleasepool {
        uint64_t serial=2000+(uintptr_t)index*500+i;
        TestTexture *texture=[TestTexture new]; texture.allocatedSize=4096+(i&3)*256;
        publish(serial,texture);
        pthread_mutex_lock(&g_smu); totk_serial_add(&g_dead_serial,serial); pthread_mutex_unlock(&g_smu);
        forget(serial);
    }
    return NULL;
}
int main(void) { @autoreleasepool {
    deferred=[NSMutableArray array]; resident=[NSMutableSet set];
    __weak TestTexture *old=nil;
    __weak TestTexture *current=nil;
    const uint64_t serial=1001;
    @autoreleasepool {
        TestTexture *a=[TestTexture new];a.allocatedSize=4096;old=a;publish(serial,a);
        TestTexture *b=[TestTexture new];b.allocatedSize=8192;current=b;publish(serial,b);
    }
    check(g_blkaux.count==1 && g_blkaux_bytes==8192 && g_blkaux_created==2 && g_blkaux_retired==1,"replacement accounting");
    int alive;
    @autoreleasepool { alive=old!=nil; } check(alive,"old allocation lost before fence");
    drain(10);
    @autoreleasepool { alive=old!=nil; } check(alive,"old allocation lost at preceding fence");
    drain(11);g_fence=11;
    @autoreleasepool { alive=old!=nil; } check(!alive,"replaced allocation survived its completed fence");
    forget(serial);
    check(g_blkaux.count==1,"live unreferenced serial was retired");
    totk_serial_add(&g_dead_serial,serial);pinned=1;forget(serial);
    check(g_blkaux.count==1 && totk_serial_has(&g_dead_serial,serial),"recording pin did not preserve finalized contents");
    pinned=0;nvn_hm_put(&g_treg_refs,&serial,(void *)1);forget(serial);
    check(g_blkaux.count==1 && totk_serial_has(&g_dead_serial,serial),"descriptor slot did not preserve finalized contents");
    nvn_hm_del(&g_treg_refs,&serial);forget(serial);
    check(!g_blkaux.count && !g_blkaux_bytes && g_blkaux_retired==2 && g_serial_forgotten==1,"proven-unreachable final retirement");
    drain(11);
    @autoreleasepool { alive=current!=nil; }check(alive,"final allocation lost before its new fence");
    drain(12);g_fence=12;
    @autoreleasepool { alive=current!=nil; }check(!alive,"final allocation survived its completed fence");
    forget(serial);check(g_blkaux_retired==2 && !g_blkaux_bytes,"repeated forget changed accounting");
    pthread_t threads[4];
    for(uintptr_t i=0;i<4;i++)check(!pthread_create(&threads[i],NULL,worker,(void *)i),"worker creation");
    for(unsigned i=0;i<4;i++)check(!pthread_join(threads[i],NULL),"worker join");
    check(!g_blkaux.count && !g_blkaux_bytes && g_blkaux_created==2002 && g_blkaux_retired==2002,"concurrent cache balance");
    check(atomic_load(&destroyed)==2,"concurrent retirements released before completion");
    drain(13);
    check(atomic_load(&destroyed)==2002 && !resident.count && !deferred.count,"concurrent retirements failed release");
    blk_aux_report();
    printf("PASS checks=%u textures=%u workers=4\n",checked,atomic_load(&destroyed));
    return 0;
} }
'''


def actual_source(source, omit_defer):
    text = source.read_text()
    start = text.index('static NSMutableDictionary<NSNumber *, id<MTLTexture>> *g_blkaux;')
    end = text.index('typedef struct { __unsafe_unretained id<MTLTexture> aux, dst;', start)
    cache = text[start:end]
    if omit_defer:
        assert cache.count('defer_obj_release(texture);') == 1
        cache = cache.replace('defer_obj_release(texture);', '', 1)
    start = text.index('static void serial_forget_locked_rmu(')
    end = text.index('static inline int registry_lock(', start)
    forget = text[start:end]
    marker = '        pthread_mutex_lock(&g_smu);\n        blk_aux_retire_locked(t->serial);'
    start = text.index(marker)
    end = text.index('        static int once;', start)
    publish = text[start:end]
    return PRELUDE + cache + forget + FIXTURE.replace('@PUBLISH@', publish)


def run(source, sanitize, negative):
    with tempfile.TemporaryDirectory(prefix='block-aux-ownership-') as scratch:
        unit = Path(scratch) / 'fixture.m'
        binary = Path(scratch) / 'fixture'
        unit.write_text(actual_source(source, negative))
        command = ['clang', '-std=gnu11', '-fobjc-arc', '-O1', '-g', '-Wall', '-Wextra',
                   '-Werror', '-pthread', '-framework', 'Foundation', '-I', str(source.parent)]
        if sanitize:
            command += ['-fsanitize=address', '-fno-omit-frame-pointer']
        subprocess.run(command + [str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
        if negative:
            assert result.returncode != 0 and 'FAIL replaced allocation survived its completed fence' in result.stderr, result
        else:
            assert result.returncode == 0 and 'textures=2002 workers=4' in result.stdout, result
        print(('ASan' if sanitize else 'normal') + (' no-defer negative' if negative else ' positive'))
        print(result.stdout + result.stderr, end='')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=SOURCE)
    args = parser.parse_args()
    for sanitizer in (False, True):
        run(args.source, sanitizer, False)
        run(args.source, sanitizer, True)
