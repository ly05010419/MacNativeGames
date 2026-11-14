"""Execute production capture dequeue/early-return code and source-region checks."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'

@unittest.skipUnless(sys.platform == 'darwin', 'Objective-C blocks')
class Capture(unittest.TestCase):
    def test_failure_callbacks_and_source_regions(self):
        source = (NVN/'nvn_mtl.m').read_text()
        start = source.index('    void (^grab)(CGImageRef) = nil; void (^grab_game)')
        body = source[start:source.index('    int alloc_i =', start)]
        probe = r'''
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
#include <assert.h>
#include <limits.h>
#include "nvn_capture_region.h"
static void (^g_grab_cb)(CGImageRef), (^g_grab_game_cb)(CGImageRef);
static char g_grab_src_path[1024];
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER, g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_pq_done, g_drawable_ns;
static int mode;
#define MLOG(...) ((void)0)
static int nogpu(void) { return mode == 1; }
static uint64_t now_ns(void) { return 0; }
static void grab_present(void (^cb)(CGImageRef), id src, const int *crop, CGSize size) {
    (void)src; (void)crop; (void)size; cb(NULL);
}
@interface Layer : NSObject
@property CGSize drawableSize;
- (id<CAMetalDrawable>)nextDrawable;
@end
@implementation Layer
- (id<CAMetalDrawable>)nextDrawable { return mode == 3 ? nil : (id<CAMetalDrawable>)[NSObject new]; }
@end
static int present(Layer *layer, int interp) {
    id<CAMetalDrawable> supplied = nil;
    id src = nil; const int *crop = NULL; uint64_t ticket = 9;
    BODY
    return 2;
}
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 2); mode = atoi(argv[1]);
    __block int a = 0, b = 0;
    g_grab_cb = ^(CGImageRef image) { assert(!image); a++; };
    g_grab_game_cb = ^(CGImageRef image) { assert(!image); b++; };
    Layer *layer = [Layer new];
    int rc = present(layer, mode == 4 ? 0 : -1);
    if (mode == 4) { assert(rc == 2 && a == 0 && b == 0 && g_grab_cb && g_grab_game_cb); }
    else {
        assert(rc == (mode == 3 ? 0 : 1));
        assert(a == 1 && b == 1 && !g_grab_cb && !g_grab_game_cb);
        present(layer, -1); assert(a == 1 && b == 1);
    }
    nvn_capture_region r;
    assert(nvn_capture_source_region(2560, 1440, NULL, &r) && r.w == 2560 && r.h == 1440);
    int crop[] = {128, 80, 1920, 1080};
    assert(nvn_capture_source_region(2560, 1440, crop, &r) && r.x == 128 && r.y == 80 && r.w == 1920 && r.h == 1080);
    int bad[][4] = {{-1,0,10,10},{0,-1,10,10},{0,0,0,10},{0,0,10,0},{0,0,-1,10},
        {0,0,2561,1440},{0,0,2560,1441},{2500,0,100,10},{0,1400,10,100},{INT_MAX,0,INT_MAX,10}};
    for (unsigned i=0;i<sizeof bad/sizeof bad[0];i++) assert(!nvn_capture_source_region(2560,1440,bad[i],&r));
    assert(!nvn_capture_source_region(SIZE_MAX,4,NULL,&r));
    assert(!nvn_capture_source_region(0,4,NULL,&r));
    puts("PASS capture callbacks and original source regions");
}}
'''.replace('    BODY', body)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp); (path/'probe.m').write_text(probe)
            subprocess.run(['xcrun','clang','-fobjc-arc','-fsanitize=address,undefined','-g',
                '-I'+str(NVN),'-framework','Foundation','-framework','QuartzCore',str(path/'probe.m'),'-o',str(path/'probe')],check=True)
            for mode in (1,2,3,4):
                env = dict(os.environ); env.pop('TOTK_PRESENT_SKIP',None)
                if mode == 2: env['TOTK_PRESENT_SKIP']='1'
                out = subprocess.run([str(path/'probe'),str(mode)],env=env,capture_output=True,text=True,timeout=15)
                self.assertEqual(out.returncode,0,out.stdout+out.stderr)
                self.assertIn('PASS capture',out.stdout)

if __name__ == '__main__': unittest.main()
