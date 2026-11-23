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


ROWS_PROGRAM = r'''#import <Foundation/Foundation.h>
%s
static NSString *hex(NSData *d) { NSMutableString *h = [NSMutableString string];
    const uint8_t *p = (const uint8_t *)d.bytes; for (NSUInteger i = 0; i < d.length; i++) [h appendFormat:@"%%02x", p[i]]; return h; }
/* a pipeline state: name, "_b", NUL, a second name, "_b", NUL, NUL, 0x01, 0xff */
static NSData *state(NSString *first, NSString *second) {
    NSMutableData *d = [NSMutableData data]; const uint8_t tail[] = {0, 0, 1, 0xff}, nul = 0;
    [d appendData:[[first stringByAppendingString:@"_b"] dataUsingEncoding:NSASCIIStringEncoding]]; [d appendBytes:&nul length:1];
    [d appendData:[[second stringByAppendingString:@"_b"] dataUsingEncoding:NSASCIIStringEncoding]]; [d appendBytes:tail length:4];
    return d;
}
int main(void) { @autoreleasepool {
    NSString *sh = [@"" stringByPaddingToLength:64 withString:@"ab" startingAtIndex:0];
    NSString *app = [sh stringByAppendingString:@"-b7530f698e76"], *quick = [sh stringByAppendingString:@"-062c9c711c36"];
    NSString *other = [sh stringByAppendingString:@"-aaaaaaaaaaaa"];
    NSDictionary *deps = @{@"attr": @{@"0": @"float"}, @"fout": @{}, @"img": @{}, @"tex": @{}};
    NSArray *seed = @[@{@"variant": app, @"deps": deps}];
    NSArray *cache = @[@{@"variant": quick, @"deps": deps, @"answers": @{@"tex": @{@"0": @"2d"}}},
                       @{@"variant": other, @"deps": @{@"attr": @{}, @"fout": @{}, @"img": @{}, @"tex": @{@"0": @"cube"}}}];
    if (![totk_superseding_variant(quick, seed, cache) isEqual:app]) { puts("FAIL same deps"); return 1; }
    if (![totk_superseding_variant(other, seed, cache) isEqual:other]) { puts("FAIL other deps"); return 1; }
    if (![totk_superseding_variant(app, seed, cache) isEqual:app]) { puts("FAIL seed variant"); return 1; }
    if (![totk_superseding_variant(quick, @[], cache) isEqual:quick]) { puts("FAIL no seed"); return 1; }
    NSString *(^map)(NSString *) = ^NSString *(NSString *v) { return totk_superseding_variant(v, seed, cache); };
    // S row: plain
    NSString *s = [NSString stringWithFormat:@"S\t%%@_b\t1", quick];
    if (![totk_row_map_variants(s, map) isEqual:([NSString stringWithFormat:@"S\t%%@_b\t1", app])]) { puts("FAIL S row"); return 1; }
    // R row: the pipeline state in hex, two names with NULs and other bytes around them
    NSString *r = [@"R\t" stringByAppendingString:hex(state(quick, other))];
    NSString *got = totk_row_map_variants(r, map);
    if (![got isEqual:[@"R\t" stringByAppendingString:hex(state(app, other))]]) { printf("FAIL R row %%s\n", got.UTF8String); return 1; }
    if (got.length != r.length) { puts("FAIL length"); return 1; }
    // M row: a "-" field, then the state; a row without such variants comes back unchanged
    NSString *m = [@"M\t-\t" stringByAppendingString:hex(state(quick, other))];
    if (![totk_row_map_variants(m, map) isEqual:[@"M\t-\t" stringByAppendingString:hex(state(app, other))]]) { puts("FAIL M row"); return 1; }
    NSString *plain = [NSString stringWithFormat:@"C\t%%@_b", app];
    if (![totk_row_map_variants(plain, map) isEqual:plain]) { puts("FAIL unchanged row"); return 1; }
    // 2026-11-16: an M row naming its task variant in the plain second field, inside a whole list
    NSString *mt = [NSString stringWithFormat:@"M\t%%@_b\t%%@", quick, hex(state(app, other))];
    NSString *mtNow = [NSString stringWithFormat:@"M\t%%@_b\t%%@", app, hex(state(app, other))];
    NSString *list = [NSString stringWithFormat:@"# header\n%%@\n\n%%@\n%%@\n", s, mt, plain];
    NSUInteger changed = 0;
    NSString *listNow = totk_list_map_rows(list, ^NSString *(NSString *row) { return totk_row_map_variants(row, map); }, &changed);
    NSString *want = [NSString stringWithFormat:@"# header\nS\t%%@_b\t1\n\n%%@\n%%@\n", app, mtNow, plain];
    if (![listNow isEqual:want] || changed != 2) { printf("FAIL list %%lu\n%%s\n", (unsigned long)changed, listNow.UTF8String); return 1; }
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

    def test_old_device_rows_are_put_on_the_apps_variant(self):
        # 2026-11-15 ("做 换成 App 的变体 加上测试"): rows recorded before the order fix name a quick start's variants
        program = ROWS_PROGRAM % (SOURCE.parent / 'totk_variant_rows.h').read_text()
        with tempfile.TemporaryDirectory() as folder:
            src, exe = Path(folder) / 'rows.m', Path(folder) / 'rows'
            src.write_text(program)
            built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Foundation', str(src), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
        self.assertEqual(ran.stdout.strip(), 'OK', ran.stdout + ran.stderr)

    def test_the_warm_up_rewrites_only_this_devices_lists(self):
        mtl = (SOURCE.parent / 'nvn_mtl.m').read_text()
        self.assertIn('NSString *row = own ? totk_rt_row_prefer_seed(line) : line;', mtl)
        rt = SOURCE.read_text()
        self.assertIn('#include "totk_variant_rows.h"', rt)
        body = function(rt, 'NSString *totk_rt_row_prefer_seed(NSString *line) {')
        self.assertIn('NSMutableArray *cache = [variants_file([g_dir stringByAppendingPathComponent:seedIndex]) mutableCopy];', body)
        # it also runs inside totk_rt_dir's dispatch_once: calling totk_rt_dir() there traps (2026-11-16, signal 5)
        self.assertNotIn('totk_rt_dir()', body)
        self.assertNotIn('index_path(', body)
        self.assertNotIn('totk_rt_dir()', function(rt, 'static NSUInteger rt_list_put_on_seed(void) {'))
        self.assertIn('totk_superseding_variant(variant, seed, cache)', body)

    def test_a_carried_list_finds_its_deps_in_the_old_folders_and_is_written_back(self):
        # 2026-11-16: the list came along to a new folder (another translator); 100 mesh rows named a task variant whose
        # deps were only in the set-aside folder, so they built nothing a draw takes (226 ms, 855 ms at the draw)
        rt = SOURCE.read_text()
        body = function(rt, 'NSString *totk_rt_row_prefer_seed(NSString *line) {')
        self.assertIn('siblings = totk_rt_siblings(g_dir);', body)
        self.assertIn('for (NSString *dir in siblings)', body)
        put = function(rt, 'static NSUInteger rt_list_put_on_seed(void) {')
        self.assertIn('totk_list_map_rows(text', put)
        self.assertIn('writeToFile:path atomically:YES', put)
        # once the seed is known (g_seed_dir) and before the iOS fill import / the warm-up
        self.assertLess(rt.index('g_seed_dir = source;'), rt.index('NSUInteger mapped = rt_list_put_on_seed();'))

    def test_nothing_in_totk_rt_dirs_dispatch_once_takes_g_rt_lock(self):
        # #2267 (2026-11-17): with TOTK_SHADER_PREFETCH=1 the first totk_rt_dir call came under g_rt_lock
        # (totk_rt_known_variants, totk_rt_variant, totk_rt_portable_variant), and its dispatch_once ran
        # rt_list_put_on_seed -> totk_rt_row_prefer_seed, which took the same lock: the boot stopped there
        rt = SOURCE.read_text()
        for name in ('NSString *totk_rt_row_prefer_seed(NSString *line) {', 'static NSUInteger rt_list_put_on_seed(void) {'):
            body = function(rt, name)
            self.assertNotIn('g_rt_lock', body, name)
        self.assertIn('pthread_mutex_lock(&g_prefer_lock);', function(rt, 'NSString *totk_rt_row_prefer_seed(NSString *line) {'))

    def test_the_devices_collected_list_is_not_an_app_list(self):
        # the same evening: the cache's pso_collected.tsv (this device's, never archived at packaging) was counted as an
        # App list, so its 1,716 unarchived rows read "App 自带清单 没存过" in red
        mtl = (SOURCE.parent / 'nvn_mtl.m').read_text()
        self.assertIn('[path isEqualToString:pso_shared_path()]', mtl)
        self.assertIn('own ? PAR_AT_USER_LIST : PAR_AT_APP_LIST', mtl)


if __name__ == '__main__':
    unittest.main()
