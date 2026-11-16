"""The steps of an express update that only check that nothing changed do it on all cores and without reading the same
files twice (user 2026-11-16: "极速更新 还是 太慢 看看如何优化", then "做 不要问我"). Measured that day on the external disk
(scratch/runs/2026-11/express-speed): one index read took 649 us on one thread, 73 us with 16; a stat 117 / 34 us. The seed
check (95 s) asked 42,848 shaders one after another, each reading its index and its Metal source; the packaging's
pipeline archive check (123 s) opened its 3,791 archive files under the lock every lookup takes and reloaded a vertex
shader in every block of rows that named it."""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
SCRIPTS = ROOT / 'games/oot/scripts'


def body(text, head, end='\n}\n'):
    start = text.index(head)
    return text[start:text.index(end, start)]


class SeedCheck(unittest.TestCase):
    rt = (NVN / 'totk_shader_rt.m').read_text()
    mtl = (NVN / 'nvn_mtl.m').read_text()

    def test_the_seed_run_prefetches_before_it_asks(self):
        seed = body(self.mtl, 'void runtime_shader_seed(', '\n}}\n')   # its body is an @autoreleasepool block
        self.assertLess(seed.index('totk_rt_prefetch(asks);'), seed.index('totk_rt_variant(key, stage, ctl, code, answers'))
        self.assertIn('for (NSDictionary *a in listed[k]) [asks addObject:@[k, a]];', seed)   # the list's states too

    def test_the_prefetch_checks_a_source_as_variant_lookup_does(self):
        lookup = body(self.rt, 'static NSString *variant_lookup(')
        prefetch = body(self.rt, 'void totk_rt_prefetch(')
        for check in ('msl.length < 64', '[msl containsString:@"double "]', '[msl containsString:@"unsupported_GLSLstd450"]'):
            self.assertIn(check, lookup)
        self.assertIn('msl && msl.length >= 64 && ![msl containsString:@"double "] && ![msl containsString:@"unsupported_GLSLstd450"]', prefetch)
        self.assertIn('totk_variants_join(seed, variants_file(index_path(shader)))', prefetch)   # variants_of's join
        self.assertIn('if (!totk_deps_match(row[@"deps"], answers)) continue;', prefetch)       # the row variant_lookup takes
        self.assertIn('if (!g_variants[shader]) g_variants[shader] = v;', prefetch)            # never over what the run knows
        # 2026-11-16: [asks valueForKey:@"firstObject"] asked each inner element and ended the seed run (-6)
        self.assertNotIn('valueForKey:', prefetch)


class ArchiveCheck(unittest.TestCase):
    archive = (NVN / 'totk_pipeline_archive.h').read_text()
    mtl = (NVN / 'nvn_mtl.m').read_text()

    def test_an_archive_file_is_opened_outside_the_lock(self):
        lookup = body(self.archive, 'static id<MTLBinaryArchive> par_archive_for(')
        opened = lookup.index('newBinaryArchiveWithDescriptor')
        self.assertLess(lookup.index('pthread_mutex_unlock(&g_par_mu);'), opened)
        self.assertLess(opened, lookup.index('pthread_mutex_lock(&g_par_mu);', opened))
        self.assertIn('if ((a = g_par_open[name])) PAR_OPEN_NOTE(name);   // another thread opened it meanwhile', lookup)

    def test_a_waited_fill_sorts_its_rows(self):
        fill = body(self.mtl, 'static void warm_fill_archive(')
        self.assertLess(fill.index('if (done) [todo sortUsingSelector:@selector(compare:)];'), fill.index('warm_load_rows(block, dict);'))


def load(name):
    spec = importlib.util.spec_from_file_location(f'speed_{name}', SCRIPTS / f'{name}.py')
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(SCRIPTS))
    spec.loader.exec_module(module)
    return module


class SeedFiles(unittest.TestCase):
    """seed_files on all cores gives what one thread gave: the files, the reflections checked, the indexes cut down."""

    def test_the_same_files_and_indexes(self):
        seed = load('prepare_ns2_seed')
        with tempfile.TemporaryDirectory() as folder:
            cache = Path(folder)
            names = []
            for i in range(40):
                shader = f'{i:064x}'
                rows = []
                for v in ('b7530f698e76', '90d07effe7fb'):
                    name = f'{shader}-{v}'
                    rows.append({'variant': name, 'deps': {}})
                    (cache / f'{name}_b.metal').write_text('x' * 80)
                    (cache / f'{name}_b.json').write_text(json.dumps({'chain': seed.CHAIN, 'key': name + '_b', 'entry': 'main0'}))
                    names.append(name)
                (cache / f'{shader}.variants').write_text(json.dumps(rows))
            verified = set(names[::2])   # one variant of each shader checked by the run
            files, indexes = seed.seed_files(cache, verified)
            self.assertEqual(len(indexes), 40)
            self.assertEqual(sorted(files), sorted([f'{n}_b.metal' for n in verified] + [f'{n}_b.json' for n in verified]
                                                   + [f'{i:064x}.variants' for i in range(40)]))
            self.assertEqual([r['variant'] for r in indexes[f'{0:064x}.variants']], [names[0]])


if __name__ == '__main__':
    unittest.main()
