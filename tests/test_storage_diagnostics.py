"""Opt-in storage totals use base entries, including detached ones, without changing them."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'macOS Foundation')
class StorageDiagnostics(unittest.TestCase):
    def test_collection_and_opt_in(self):
        source = (NVN / 'nvn_mtl.m').read_text()
        start = source.index('typedef struct { uint64_t mem, size;')
        types = source[start:source.index('static uint64_t g_stor_entries', start)]
        marker = '/* Storage diagnostic accounting (g_smu held). */'
        # The old implementation has no report; its empty output must fail below.
        diag = 'static void stor_diag_report(void) {}'
        if marker in source:
            start = source.index(marker)
            diag = source[start:source.index('// 预排布贴图上传', start)]
        probe = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include "nvn_hmap.h"
#include "totk_serial_set.h"
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static totk_serial_set g_dead_serial;
TYPES
static stor_ent **g_stor_det; static int g_stor_ndet;
void report_log(const char *fmt, ...) {
    if (!strstr(fmt, "skipped")) {
        assert(!pthread_mutex_trylock(&g_smu));
        pthread_mutex_unlock(&g_smu);
    }
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}
#define MLOG report_log
DIAG
@interface Allocation : NSObject
@property NSUInteger allocatedSize;
@end
@implementation Allocation
@end
int main(int argc, char **argv) { @autoreleasepool {
    if (argc > 1 && !strcmp(argv[1], "busy")) {
        pthread_mutex_lock(&g_smu); stor_diag_report(); pthread_mutex_unlock(&g_smu); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "empty")) { stor_diag_report(); return 0; }
    uint64_t a[] = {1}, b[] = {2, 3}, c[] = {4, 5};
    stor_ent e[5] = {0};
    for (int i = 0; i < 5; i++) {
        Allocation *t = [Allocation new]; t.allocatedSize = 100 * (i + 1);
        e[i].tex = (void *)CFBridgingRetain(t); e[i].k.mem = (uint64_t)i;
        if (i < 3) nvn_hm_put(&g_by_storage, &e[i].k, &e[i]);
    }
    e[0].ser = a; e[0].nser = 1; e[2].ser = b; e[2].nser = 2; e[3].ser = c; e[3].nser = 2;
    stor_ent *det[] = {&e[3], &e[4]}; g_stor_det = det; g_stor_ndet = 2;
    totk_serial_add(&g_dead_serial, 2); totk_serial_add(&g_dead_serial, 4); totk_serial_add(&g_dead_serial, 5);
    for (uint64_t view = 10; view < 20; view++) nvn_hm_put(&g_stor_of_obj, &view, &e[0]);
    stor_diag_report();
    assert(g_by_storage.n == 3 && g_stor_of_obj.n == 10 && g_stor_ndet == 2 && g_dead_serial.n == 3);
    assert(e[0].nser == 1 && e[1].nser == 0 && e[2].nser == 2 && e[3].nser == 2 && e[4].nser == 0);
    for (int i = 0; i < 3; i++) assert(nvn_hm_get(&g_by_storage, &e[i].k) == &e[i]);
    for (int i = 0; i < 5; i++) CFRelease(e[i].tex);
    return 0;
} }
'''.replace('TYPES', types).replace('DIAG', diag)
        with tempfile.TemporaryDirectory(prefix='mng-storage-') as folder:
            root = Path(folder)
            (root / 'probe.m').write_text(probe)
            exe = root / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined', '-fno-sanitize-recover=all', '-I'+str(NVN), str(root / 'probe.m'),
                '-framework', 'Foundation', '-o', str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            env = dict(os.environ)
            env.pop('TOTK_STORAGE_REPORT', None)
            def run(*args):
                p = subprocess.run([str(exe), *args], env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                return p.stdout
            self.assertEqual(run(), '')
            env['TOTK_STORAGE_REPORT'] = '0'
            self.assertEqual(run(), '')
            env['TOTK_STORAGE_REPORT'] = '1'
            self.assertIn('bases=5 bytes=1500 held=3 idle=2 idle-bytes=700 dead-only=1 dead-bytes=400 holders=5 dead-holders=3 detached=2', run())
            self.assertIn('bases=0 bytes=0 held=0 idle=0', run('empty'))
            self.assertIn('[storage-live] skipped: storage registry busy', run('busy'))
        self.assertIn('res_site_report(16);\n    stor_diag_report();', source)
        self.assertIn('res_site_report(8);\n    stor_diag_report();', source)


if __name__ == '__main__':
    unittest.main()
