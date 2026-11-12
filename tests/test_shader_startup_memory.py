"""Seed import must release temporary metadata before returning to a guest thread."""
import hashlib
import json
import os
import re
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(platform.system() == 'Darwin', 'Foundation allocation lifetime')
class ShaderStartupMemory(unittest.TestCase):
    def test_import_temporaries_do_not_survive_initialization(self):
        text = (NVN / 'totk_shader_rt.m').read_text()
        begin = text.index('NSString *totk_rt_dir(void) {')
        body = text[begin:text.index('\n/* PR #84', begin)]
        probe = r'''
#import "totk_shader_seed.h"
#include <malloc/malloc.h>
static NSString *g_dir, *g_seed_dir;
static NSSet *g_seed_names;
static BOOL g_seed_all_default;
static void (*runtime_boot_status)(const char *, const char *);
static NSString *root_dir(void) { return @"unused"; }
static NSString *seed_dir(void) { return @(getenv("TOTK_SHADER_SEED_DIR")); }
static void seed_mark_broken(NSString *name) { (void)name; }
static void seed_progress(int pass, NSUInteger done, NSUInteger total) {
    (void)pass; (void)done; (void)total;
}
#define TOTK_UI(zh, en) en
#define LOG(...) fprintf(stderr, __VA_ARGS__)
BODY
static size_t live_bytes(void) {
    malloc_statistics_t stats = {0}; malloc_zone_statistics(NULL, &stats); return stats.size_in_use;
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    BOOL overlay = !strcmp(argv[1], "overlay");
    size_t before_drain;
    @autoreleasepool {
        if (![totk_rt_dir() isEqual:@(getenv("TOTK_SHADER_CACHE"))] || !g_seed_all_default) return 3;
        before_drain = live_bytes();
    }
    size_t after_drain = live_bytes();
    size_t retained = before_drain > after_drain ? before_drain - after_drain : 0;
    printf("startup temporaries retained until caller drain: %zu bytes\n", retained);
    // The cache path and overlay must still be usable after the caller's pool drains.
    if (![totk_rt_dir() isEqual:@(getenv("TOTK_SHADER_CACHE"))] || !g_seed_all_default) return 4;
    if (overlay) {
        if (![g_seed_dir isEqual:seed_dir()] || g_seed_names.count != 6144) return 5;
        for (NSString *name in g_seed_names)
            if (![NSFileManager.defaultManager fileExistsAtPath:[g_seed_dir stringByAppendingPathComponent:name]]) return 6;
    } else if (g_seed_dir || g_seed_names) return 7;
    // Allow small Foundation/allocator fluctuations; the unfixed import retains many MiB.
    return retained > 2 * 1024 * 1024;
} }
'''.replace('BODY', body)
        with tempfile.TemporaryDirectory(prefix='mng-shader-startup-') as folder:
            root = Path(folder)
            source = root/'seed'
            source.mkdir()
            files = {}
            for i in range(2048):
                key = hashlib.sha256(str(i).encode()).hexdigest()
                variant = key + '-bbbbbbbbbbbb'
                payloads = {key+'.variants': json.dumps([dict(variant=variant, deps={})]).encode(),
                            variant+'_b.metal': b'// synthetic shader', variant+'_b.json': b'{}'}
                for name, data in payloads.items():
                    (source/name).write_bytes(data)
                    files[name] = hashlib.sha256(data).hexdigest()
            chain = re.search(r'#define DMSH_CHAIN "([^"]+)"\s*/\* game: totk\b', (NVN/'dmsh_chain.h').read_text())[1]
            (source/'seed-manifest.json').write_text(json.dumps(dict(schema=1, chain=chain,
                shaders=2048, all_default=True, files=files)))
            path = root/'probe.m'
            path.write_text(probe)
            for mode, flags in [('ns2', ['-DGAME_NS2=1']), ('switch1', [])]:
                exe, cache = root/mode, root/('cache-'+mode)
                cache.mkdir()
                # An existing pipeline directory forces the ordinary per-file merge path.
                (cache/'pipelines').mkdir()
                build = subprocess.run(['xcrun', 'clang', '-O2', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-deprecated-declarations', '-I'+str(NVN), *flags, str(path),
                    '-framework', 'Foundation', '-o', str(exe)], capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
                for phase in ('first import', 'installed cache', 'overlay'):
                    with self.subTest(mode=mode, phase=phase):
                        # Use an empty cache for the read-in-place path, without copied seed files.
                        target = root/('overlay-'+mode) if phase == 'overlay' else cache
                        env = dict(os.environ, TOTK_SHADER_SEED_DIR=str(source), TOTK_SHADER_CACHE=str(target),
                            TOTK_SHADER_SEED='on', TOTK_SHADER_SEED_IN_PLACE='1' if phase == 'overlay' else '0')
                        run = subprocess.run([str(exe), phase], capture_output=True, text=True, env=env, timeout=60)
                        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
                        if phase != 'overlay':
                            self.assertTrue((cache/'seed-installed.json').is_file())
                            self.assertEqual((cache/(variant+'_b.metal')).read_bytes(), b'// synthetic shader')


if __name__ == '__main__':
    unittest.main()
