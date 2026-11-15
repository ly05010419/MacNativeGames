"""Exercise actual pool creation/address/TLS/raw-table code without Metal.

Foundation buffers model allocation and residency only. Real Metal mapping,
driver lifetime, and gameplay first-use cost remain native acceptance gates.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'

PRELUDE = r'''
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include "nvn_pool_index.h"
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE TEST_IPHONE
#define MTLResourceStorageModeShared 0
#define BOTW_LATE_DROP(lo,hi)
#define MLOG(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define MDIE(...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); exit(2); } while (0)
static unsigned checked, no_copy_creations, no_copy_attempts, commits, drops;
static int fail_once;
static void check(int good,const char *label) {
    checked++;
    if (!good) { fprintf(stderr,"FAIL %s after %u checks\n",label,checked); exit(1); }
}
@protocol MTLBuffer <NSObject>
@property(readonly) NSUInteger length;
@property(readonly) NSUInteger allocatedSize;
@property(readonly) uint64_t gpuAddress;
@property(readonly) void *contents;
@end
@interface TestBuffer : NSObject <MTLBuffer>
@property NSUInteger length;
@property NSUInteger allocatedSize;
@property uint64_t gpuAddress;
@property void *contents;
@property BOOL owns;
@end
@implementation TestBuffer
- (void)dealloc { if (_owns) free(_contents); }
@end
@interface TestDevice : NSObject
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)n options:(NSUInteger)o deallocator:(id)d;
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(NSUInteger)o;
@end
@implementation TestDevice
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)n options:(NSUInteger)o deallocator:(id)d {
    (void)o; (void)d; no_copy_attempts++;
    if (fail_once) { fail_once=0; return nil; }
    TestBuffer *b=[TestBuffer new]; b.contents=p; b.length=n; b.allocatedSize=n;
    b.gpuAddress=UINT64_C(0x100000000)+UINT64_C(0x1000000)*++no_copy_creations;
    return b;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(NSUInteger)o {
    (void)o; TestBuffer *b=[TestBuffer new]; b.contents=calloc(1,n); b.owns=YES;
    b.length=n; b.allocatedSize=n; b.gpuAddress=(uintptr_t)b.contents; return b;
}
@end
static TestDevice *g_dev;
static pthread_mutex_t g_mu=PTHREAD_MUTEX_INITIALIZER;
static NSMutableSet *resident;
static NSMutableArray *deferred;
static uint64_t g_reg_gen;
static _Atomic int g_raw_index=-1,g_raw_range_cache=-1;
static void res_add(id b) { check(b!=nil,"resident buffer exists"); [resident addObject:b]; }
static void res_remove(id b) { [resident removeObject:b]; }
static void res_commit(void) { commits++; }
static void defer_obj_release(id b) { [deferred addObject:b]; }
static int nvn_dirty_tracked(void *p,size_t n) { (void)p; (void)n; return 0; }
static void *nvn_dirty_gpu_write_begin(void *p,size_t n) { (void)p; (void)n; return NULL; }
static void nvn_dirty_gpu_write_end(void *p) { (void)p; }
static void pool_range_check(uintptr_t a,uintptr_t b,const void *mem,uint64_t size) {
    check(a<=(uintptr_t)mem && size && (uintptr_t)mem+size<=b && !(a%getpagesize()) && !(b%getpagesize()),"valid aligned range");
}
'''

INIT = r'''
static void mtl_init(void) {
    if (!g_dev) { g_dev=[TestDevice new]; g_keep=[NSMutableArray new];
        resident=[NSMutableSet new]; deferred=[NSMutableArray new]; }
}
'''

FIXTURE = r'''
static void stor_drop_range(uintptr_t lo,uintptr_t hi) { check(lo<hi,"storage deletion range"); drops++; }
static void wb_drop_range(uintptr_t lo,uintptr_t hi) { check(lo<hi,"writeback deletion range"); }
static void stor_sweep(uint64_t fence) { (void)fence; }
static uint64_t registration(void *p) {
    stor_alias_pool v={0}; check(stor_alias_pool_range((uintptr_t)p,16,&v),"registered identity before materialization");
    check(v.base==(uintptr_t)p && v.size==65536 && v.generation,"registered extent and generation"); return v.generation;
}
int main(int argc,char **argv) { @autoreleasepool {
    int eager=argc>1 && !strcmp(argv[1],"eager");
    if (eager) setenv("TOTK_POOL_RES_ALL","1",1); else unsetenv("TOTK_POOL_RES_ALL");
    enum { N=32, SIZE=65536 };
    uint8_t *memory=mmap(NULL,N*SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    check(memory!=MAP_FAILED,"guest mapping");
    for (unsigned i=0;i<N;i++) { memset(memory+i*SIZE,(int)(i+1),SIZE); mtl_pool_add(memory+i*SIZE,SIZE); }
    int lazy=!eager;
#if !defined(GAME_OOT) || TARGET_OS_IPHONE
    lazy=0;
#endif
    check(g_pools.n==N && g_pool_bytes==N*SIZE,"all guest registrations preserved");
    check(no_copy_creations==(lazy?0:N),"unused pools do not construct wrappers");
    check(g_keep.count==(lazy?0:N),"wrapper ownership matches construction");
    check(g_pool_res_n==(eager?N:0),"explicit resident-all behavior preserved");
    uint64_t first=registration(memory);
    void *buffer=NULL; uint64_t off=0,gpu=0;
    check(mtl_gpu_addr((uintptr_t)memory+37,512,&buffer,&off,&gpu),"first GPU address lookup");
    uint64_t first_gpu=gpu;
    check(buffer && off==37 && gpu==((__bridge id<MTLBuffer>)buffer).gpuAddress+37,"GPU mapping delta");
    check(((uint8_t *)((__bridge id<MTLBuffer>)buffer).contents)[37]==1,"CPU backing preserved");
    check(no_copy_creations==(lazy?1:N) && g_pool_res_n==(eager?N:1),"first use materializes once and resides");
    check(registration(memory)==first,"materialization preserves registration generation");
    for (int i=0;i<50;i++) check(mtl_gpu_addr((uintptr_t)memory+37,512,&buffer,&off,&gpu) && gpu==first_gpu,"TLS/hash repeated GPU lookup");
    check(no_copy_creations==(lazy?1:N),"TLS hits do not allocate");
    uintptr_t lo=0,hi=0;
    check(mtl_addr_range((uintptr_t)memory+SIZE+7,20,&lo,&hi) && lo==(uintptr_t)memory+SIZE && hi==lo+SIZE,"range consumer materializes");
    check(no_copy_creations==(lazy?2:N),"range consumer allocation count");
    mtl_pool_addressed(memory+2*SIZE,memory+2*SIZE,SIZE);
    check(gmem_map_gpu(0)!=0,"issued-address table created");
    check(no_copy_creations==(lazy?3:N),"issued-address consumer materializes only relevant pool");
    uint64_t *table=g_gmem_buf[0].contents;
    int found=0;
    for (unsigned i=1;i<GMEM_MAP_ENTRIES;i++) if (table[i*4]==(uintptr_t)memory+2*SIZE) {
        id<MTLBuffer> b=(__bridge id<MTLBuffer>)pidx_find(&g_pools,(uintptr_t)memory+2*SIZE,1)->buf;
        found=table[i*4+2]==b.gpuAddress-(uintptr_t)b.contents;
    }
    check(found,"raw table contains real materialized GPU delta");
    check(gmem_map_gpu(1)!=0 && no_copy_creations==(lazy?3:N),"both raw table ABIs preserve wrappers");
    mtl_pool_del(memory+3*SIZE);
    check(no_copy_creations==(lazy?3:N) && !mtl_addr_known((uintptr_t)memory+3*SIZE,1),"dormant deletion creates nothing");
    check(mtl_gpu_addr((uintptr_t)memory+37,512,&buffer,&off,&gpu),"prime TLS before deletion");
    mtl_pool_del(memory);
    check(!mtl_addr((uintptr_t)memory+37,512,&buffer,&off),"deletion invalidates TLS before pointer access");
    mtl_pool_add(memory,SIZE);
    check(registration(memory)>first,"same-address registration gets new generation");
    unsigned before=no_copy_attempts;
    if (lazy) fail_once=1;
    check(mtl_gpu_addr((uintptr_t)memory+37,512,&buffer,&off,&gpu) && gpu!=first_gpu,"same-address lookup cannot return stale wrapper");
    if (lazy) check(no_copy_attempts==before+2,"existing first-use retry preserved");
    for (unsigned i=0;i<N*SIZE;i++) if (memory[i]!=(uint8_t)(i/SIZE+1)) check(0,"guest bytes unchanged");
    check(1,"all guest bytes unchanged");
    pool_wrapper_report();
    for (unsigned i=0;i<N;i++) if(i!=3) mtl_pool_del(memory+i*SIZE);
    check(g_pools.n==0 && g_keep.count==0 && g_pool_bytes==0 && g_pool_res_n==0,"all ownership/accounting removed");
    check(drops==N+1,"only real guest finalizations trigger storage cleanup");
    check(commits>0,"resident transitions commit");
    g_gmem_buf[0]=nil; g_gmem_buf[1]=nil; [resident removeAllObjects]; [deferred removeAllObjects];
    munmap(memory,N*SIZE);
    printf("PASS lazy-pool %u checks / %u no-copy creations\n",checked,no_copy_creations);
} return 0; }
'''


def between(text, first, last):
    return text[text.index(first):text.index(last, text.index(first))]


def actual_source(source, negative):
    globals_ = between(source, 'static pidx g_pools;', '#define C1_BANK_SIZE')
    pool = between(source, 'static uint64_t g_pool_bytes', '// ---------- 提交 ----------')
    alias = between(source, 'typedef struct { uint64_t base, size, generation; } stor_alias_pool;', '#include "nvn_stor_alias.h"')
    raw = between(source, '#define GMEM_MAP_ENTRIES', '/* The host never consumes the per-page GPU log.')
    if negative == 'eager':
        pool = pool.replace('eager = pool_res_all();', 'eager = 1;')
    elif negative == 'buffer-generation':
        alias = alias.replace('uint64_t generation = p->registration;', 'uint64_t generation = p->buf ? p->registration : 0;')
    elif negative == 'reused-generation':
        pool = pool.replace('ent.registration = g_pool_adds + 1;', 'ent.registration = 1;')
    elif negative == 'stale-tls':
        at = pool.index('void mtl_pool_del(')
        pool = pool[:at] + pool[at:].replace('atomic_fetch_add_explicit(&g_pool_gen, 1, memory_order_release);', '', 1)
    elif negative == 'raw-unmaterialized':
        raw = raw.replace('        pool_res(p);', '')
    return PRELUDE + globals_ + INIT + pool + alias + raw + FIXTURE


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=NVN / 'nvn_mtl.m')
    parser.add_argument('--sanitize', choices=['address', 'thread'])
    parser.add_argument('--negative', choices=['eager', 'buffer-generation', 'reused-generation', 'stale-tls', 'raw-unmaterialized'])
    parser.add_argument('--platform', choices=['oot-mac', 'other-mac', 'oot-ios'], default='oot-mac')
    parser.add_argument('--resident-all', action='store_true')
    args = parser.parse_args()
    source = args.source.read_text()
    with tempfile.TemporaryDirectory(prefix='nvn-lazy-pool-') as temp:
        path = Path(temp)
        fixture = path / 'probe.m'
        fixture.write_text(actual_source(source, args.negative))
        flags = ['-DTEST_IPHONE=' + ('1' if args.platform == 'oot-ios' else '0')]
        if args.platform != 'other-mac':
            flags.append('-DGAME_OOT=1')
        if args.sanitize:
            flags += ['-fsanitize=' + args.sanitize, '-fno-omit-frame-pointer']
        subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fobjc-arc', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-function', '-Wno-unused-variable', '-pthread',
                        '-framework', 'Foundation', '-I', str(NVN), *flags,
                        str(fixture), '-o', str(path / 'probe')], check=True)
        subprocess.run([str(path / 'probe'), *(['eager'] if args.resident_all else [])], check=True)


if __name__ == '__main__':
    main()
