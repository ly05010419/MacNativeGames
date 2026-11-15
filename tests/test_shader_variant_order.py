"""A shader's variants: the app's (seed) first, the device cache's own only where the seed has none of that name.

2026-11-15 ("为什么有这么多 新管线？？？"): a quick start (title shaders only in its seed) translated 1,040 shaders during
play into the cache the full app shares, each as a variant named after all its answers, beside the seed's default variant
with the same deps. variants_of put the cache's rows first and variant_lookup takes the first match, so the full app used
the cache's variants and its pipelines missed the app's archive: 866 render and 36 mesh pipelines of its own warm-up
lists compiled at every start (0 with an empty cache). This compiles totk_variants_join out of totk_shader_rt.m and runs
it, and checks that variants_of joins in that order."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m'


def function(source, signature):
    start = source.index(signature)
    depth, i = 0, source.index('{', start)
    for j in range(i, len(source)):
        depth += {'{': 1, '}': -1}.get(source[j], 0)
        if depth == 0:
            return source[start:j + 1]
    raise AssertionError(signature + ': unbalanced braces')


PROGRAM = r'''#import <Foundation/Foundation.h>
%s
static NSArray *names(NSArray *rows) { NSMutableArray *n = [NSMutableArray array]; for (NSDictionary *r in rows) [n addObject:r[@"variant"]]; return n; }
int main(void) { @autoreleasepool {
    NSDictionary *deps = @{@"tex": @{@"4": @"*2d"}};
    NSArray *seed = @[@{@"variant": @"s-b7530f698e76", @"deps": deps}];
    NSArray *cache = @[@{@"variant": @"s-90d07effe7fb", @"deps": deps, @"answers": @{@"tex": @{@"4": @"2d"}}},
                       @{@"variant": @"s-b7530f698e76", @"deps": deps},          /* a seed row written back into the cache */
                       @{@"variant": @"s-aaaaaaaaaaaa", @"deps": @{@"tex": @{@"4": @"cube"}}}];
    NSArray *got = names(totk_variants_join(seed, cache));
    NSArray *want = @[@"s-b7530f698e76", @"s-90d07effe7fb", @"s-aaaaaaaaaaaa"];
    if (![got isEqual:want]) { printf("FAIL order %%s\n", got.description.UTF8String); return 1; }
    if (![names(totk_variants_join(@[], cache)) isEqual:(@[@"s-90d07effe7fb", @"s-b7530f698e76", @"s-aaaaaaaaaaaa"])]) { puts("FAIL no seed"); return 1; }
    if (totk_variants_join(@[@"junk", @{@"deps": deps}], nil).count != 0) { puts("FAIL junk rows"); return 1; }
    puts("OK"); return 0;
} }
'''


class SeedVariantsFirst(unittest.TestCase):
    def test_the_seed_variant_comes_before_the_cache_one(self):
        source = SOURCE.read_text()
        program = PROGRAM % function(source, 'NSArray *totk_variants_join(NSArray *seed, NSArray *cache)')
        with tempfile.TemporaryDirectory() as folder:
            src, exe = Path(folder) / 'join.m', Path(folder) / 'join'
            src.write_text(program)
            built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Foundation', str(src), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.stdout.strip(), 'OK', ran.stdout + ran.stderr)

    def test_variants_of_joins_seed_then_cache(self):
        body = function(SOURCE.read_text(), 'static NSMutableArray *variants_of(NSString *shader) {')
        self.assertIn('totk_variants_join(seed, variants_file(index_path(shader)))', body)

    def test_the_devices_collected_list_is_not_an_app_list(self):
        # the same evening: the cache's pso_collected.tsv (this device's, never archived at packaging) was counted as an
        # App list, so its 1,716 unarchived rows read "App 自带清单 没存过" in red
        mtl = (SOURCE.parent / 'nvn_mtl.m').read_text()
        self.assertIn('[path isEqualToString:pso_shared_path()]', mtl)
        self.assertIn('own ? PAR_AT_USER_LIST : PAR_AT_APP_LIST', mtl)


if __name__ == '__main__':
    unittest.main()
