"""Compile the production cache routines with a CPU sampler registry; no GPU/game data.

The existing probe-gpu-sampler-border supplies the end-to-end Metal pixel checks.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn"


@unittest.skipUnless(sys.platform == "darwin", "Foundation cache ownership")
class SamplerCaches(unittest.TestCase):
    def test_invalidation_lazy_allocation_and_function_identity(self):
        source = (NVN / "nvn_mtl.m").read_text()
        classes = source[source.index("@interface NvBorderKeyCache"):source.index("@interface NvShader")]
        border = source[source.index("static int border_filtered("):source.index("static NSString *border_src(")]
        functions = source[source.index("static id<MTLFunction> function_cache_get("):source.index("static int function_cache_on(")]
        probe = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nvn_obj.h"
#include "nvn_refl.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#define MDIE(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)
CLASSES
@interface NvShader : NSObject { @public nvn_refl rf; NvBorderKeyCache *border_cache; int border_seen; } @end
@implementation NvShader @end
static struct { uint64_t tex[6][128], samp[6][128]; } S;
static _Atomic int g_border_custom_any = 1;
static _Atomic uint64_t g_reg_gen = 1;
static samp_par g_sreg[4]; static int g_sreg_ok[4], g_nsreg=4;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int registry_lock(void) { return pthread_mutex_lock(&mutex); }
static void registry_unlock(void) { pthread_mutex_unlock(&mutex); }
static int samp_border_custom(const samp_par *p) { return p->border[0]!=0 || p->border[1]!=0 || p->border[2]!=0; }
BORDER
FUNCTIONS
static NSString *key_for(NvShader *sh, int stage) {
    float c[8][4]; int a[8],n; unsigned x[8]; samp_par p[8];
    return border_key(sh,stage,c,a,x,p,&n);
}
static void registration(int id, samp_par p, int valid) {
    registry_lock(); g_sreg[id]=p; g_sreg_ok[id]=valid; g_reg_gen++; registry_unlock();
}
static void *replace_sampler(void *unused) {
    (void)unused;
    for (int i=0;i<10000;i++) registration(1,(samp_par){.ws=4,.wt=4,.border={i&1?.25f:.75f,0,0,1}},1);
    return NULL;
}
int main(int argc,char **argv) { @autoreleasepool {
    assert(argc==2); int enabled=atoi(argv[1]);
    setenv("TOTK_BORDER_KEY_CACHE",argv[1],1); setenv("TOTK_BORDER_KEY_CHECK","1",1);
    NvShader *plain=[NvShader new]; assert(!key_for(plain,1) && !plain->border_cache);
    NvShader *sh=[NvShader new]; sh->rf.nb=1; sh->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SAMPT,.reg=0,.aid=7};
    samp_par a={.ws=4,.wt=4,.border={.25,.5,.75,1},.aniso=1,.lod_max=4}; registration(1,a,1);
    S.tex[1][0]=UINT64_C(1)<<20;
    NSString *first=key_for(sh,1); assert(first && [first hasPrefix:@"7:3:"]);
    assert((sh->border_cache!=nil)==enabled);
    NSString *same=key_for(sh,1); assert([same isEqualToString:first]); if(enabled)assert(same==first && g_bk_hits==1);
    a.border[0]=.125; registration(1,a,1); NSString *changed=key_for(sh,1); assert(![changed isEqualToString:first]);
    a.magf=1; registration(1,a,1); NSString *filtered=key_for(sh,1); assert([filtered containsString:@":f"] && ![filtered isEqualToString:changed]);
    a.wt=1; registration(1,a,1); assert(![key_for(sh,1) isEqualToString:filtered]);
    registration(1,a,0); assert(!key_for(sh,1));
    registration(1,a,1); assert(key_for(sh,1));
    a.border[1]=.875; registration(2,a,1); S.tex[1][0]=UINT64_C(2)<<20;
    NSString *second=key_for(sh,1); assert(second && ![second isEqualToString:first]);
    sh->rf.b[0].kind=NVN_RB_SAMP; S.samp[0][0]=UINT64_C(1)<<20;
    assert(![key_for(sh,0) isEqualToString:second]);
    // Concurrent descriptor publication must never produce a stale cache hit (CHECK recomputes under the same lock).
    pthread_t writer; assert(!pthread_create(&writer,NULL,replace_sampler,NULL));
    for(int i=0;i<10000;i++) key_for(sh,0);
    assert(!pthread_join(writer,NULL));
    if(!enabled)assert(!sh->border_cache);

    NvFunctionCache *cache=nil;
    function_cache_put(&cache,nil,nil,0,nil); assert(!cache);  // pending functions retry
    id peer=[NSObject new], token=[NSObject new]; __weak id weak_peer=peer, weak_token=token;
    NSString *key=[NSString stringWithFormat:@"key-%d",argc], *equal=[[NSMutableString alloc] initWithString:key];
    function_cache_put(&cache,peer,key,0,(id<MTLFunction>)token);
    assert(function_cache_get(cache,peer,key,0)==token);
    assert(!function_cache_get(cache,peer,equal,0) && !function_cache_get(cache,peer,key,1));
    assert(!function_cache_get(cache,[NSObject new],key,0));
    peer=nil;token=nil;assert(weak_peer && weak_token);  // identities remain owned until eviction
    for(int i=0;i<4;i++)function_cache_put(&cache,nil,nil,i,(id<MTLFunction>)[NSObject new]);
    assert(!weak_peer && !weak_token);
    puts("sampler invalidation, concurrent publication, lazy ownership and pending-function retry passed");
    return 0;
} }
'''.replace("\nCLASSES\n", "\n" + classes + "\n").replace("\nBORDER\n", "\n" + border + "\n").replace("\nFUNCTIONS\n", "\n" + functions + "\n")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "probe.m"
            binary = Path(folder) / "probe"
            path.write_text(probe)
            build = subprocess.run(["xcrun", "clang", "-std=gnu11", "-O1", "-g", "-fobjc-arc", "-Wall", "-Wextra", "-Werror",
                                    "-I" + str(NVN), "-I" + str(NVN / "include"), str(path),
                                    "-framework", "Foundation", "-framework", "Metal", "-o", str(binary)],
                                   capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for enabled in ("0", "1"):
                result = subprocess.run([str(binary), enabled], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
