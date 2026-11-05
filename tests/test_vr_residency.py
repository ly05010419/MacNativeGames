"""Exercise production VR startup/residency transactions with concurrent producers."""
from pathlib import Path
import os
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == 'Darwin', 'Mac VR runtime')
class VRResidency(unittest.TestCase):
    def test_startup_publication_and_shared_set_transaction(self):
        source = (ROOT / 'zeldaVR/gfx/totk_vr.m').read_text()
        begin = source.index('static void vr_source_start(')
        helpers = source[begin:source.index('static int source_encode(', begin)]
        # Negative controls remove one synchronization boundary in the scratch
        # build, so both the initialization and transaction checks must fail.
        helpers = helpers.replace('dispatch_once(&once, ^{ start_thread(device); });',
                                  'if (getenv("VR_TEST_NO_ONCE")) start_thread(device); else dispatch_once(&once, ^{ start_thread(device); });')
        helpers = helpers.replace('pthread_mutex_lock(&mutex);',
                                  'if (!getenv("VR_TEST_NO_LOCK")) pthread_mutex_lock(&mutex);')
        helpers = helpers.replace('pthread_mutex_unlock(&mutex);',
                                  'if (!getenv("VR_TEST_NO_LOCK")) pthread_mutex_unlock(&mutex);')
        probe = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
static _Atomic uintptr_t owner;
static _Atomic unsigned initialized,committed;
static struct { id<MTLDevice> dev; id<MTLResidencySet> rs; } g;
static void transaction(void) {
    uintptr_t me=(uintptr_t)pthread_self(),expected=0;
    if (!atomic_compare_exchange_strong(&owner,&expected,me) && expected!=me) {
        fprintf(stderr,"VR residency transaction interleaved\n");exit(2);
    }
    usleep(30);
}
@interface ProbeSet:NSObject
-(void)removeAllocation:(id<MTLAllocation>)resource;
-(void)addAllocation:(id<MTLAllocation>)resource;
-(void)commit;
@end
@implementation ProbeSet
-(void)removeAllocation:(id<MTLAllocation>)resource {(void)resource;transaction();}
-(void)addAllocation:(id<MTLAllocation>)resource {(void)resource;transaction();}
-(void)commit {transaction();atomic_fetch_add(&committed,1);atomic_store(&owner,0);}
@end
static void start_thread(id<MTLDevice> device) {
    if(atomic_fetch_add(&initialized,1)) {fprintf(stderr,"VR source initialized multiple times\n");exit(2);}
    usleep(5000);g.rs=(id<MTLResidencySet>)[ProbeSet new];g.dev=device;
}
HELPERS
int main(void) {@autoreleasepool {
    id<MTLDevice> device=(id<MTLDevice>)[NSObject new];
    dispatch_apply(4,dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^(size_t worker){
        (void)worker;@autoreleasepool {
            vr_source_start(device);
            if(!g.dev||!g.rs)exit(2);
            id<MTLAllocation> resource=(id<MTLAllocation>)[NSObject new];
            for(unsigned i=0;i<512;i++)vr_residency_replace(i?resource:nil,resource);
        }
    });
    if(initialized!=1||committed!=2048||owner)exit(2);
    puts("VR source initialized once; 2048 shared residency transactions passed");
}return 0;}
'''.replace('HELPERS', helpers)
        with tempfile.TemporaryDirectory(prefix='mng-vr-residency-') as folder:
            path = Path(folder)/'probe.m'
            binary = Path(folder)/'probe'
            path.write_text(probe)
            build = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O1', '-fobjc-arc',
                                    '-Wall', '-Wextra', '-Werror', '-framework', 'Foundation',
                                    '-framework', 'Metal', str(path), '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('2048 shared residency transactions passed', result.stdout)
            for flag, message in [('VR_TEST_NO_LOCK', 'VR residency transaction interleaved'),
                                  ('VR_TEST_NO_ONCE', 'VR source initialized multiple times')]:
                result = subprocess.run([str(binary)], env=dict(os.environ, **{flag:'1'}),
                                        capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
