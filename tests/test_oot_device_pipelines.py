"""games/oot/ios/tools/device_pipelines.py: the device's pipelines merged into a few dozen files (--groups, 2026-11-13).

The iPad (A16) build had 19,750 pipelines-<arch>-<name>.metalarchive files, one per pipeline; the game was killed by
jetsam reason "vnode-limit" during warm-up (each opened archive holds a vnode). Pipelines are now grouped by the hash of
their Mac file's name into merged files, one metal-tt run each; the runtime's contract (totk_pipeline_archive.h
par_load_prebuilt: tsv "<key>\\t<name>", pipelines-<arch>-<name>.metalarchive) is unchanged, several keys simply share
a name."""
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / 'games/oot/ios/tools'


def load_oot_tool():
    """OOT's own device_pipelines, with OOT's compile_seed (tests of the TotK copy import another compile_seed)."""
    saved = sys.modules.get('compile_seed')
    spec = importlib.util.spec_from_file_location('oot_compile_seed_for_pipelines', TOOLS / 'compile_seed.py')
    compile_seed = importlib.util.module_from_spec(spec); spec.loader.exec_module(compile_seed)
    sys.modules['compile_seed'] = compile_seed
    try:
        spec = importlib.util.spec_from_file_location('oot_device_pipelines', TOOLS / 'device_pipelines.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    finally:
        if saved is None: sys.modules.pop('compile_seed', None)
        else: sys.modules['compile_seed'] = saved
    return module


dp = load_oot_tool()


def script(*labels, kind='render_pipelines', fn='main0'):
    return {'version': {'major': 0, 'minor': 3, 'sub_minor': 0}, 'generator': 'MetalFramework', 'functions': {},
            'libraries': {'paths': [{'label': l, 'path': '/old/' + l} for l in labels]},
            'pipelines': {kind: [{'vertex_function': f'alias:{labels[0]}#{fn}'}]}}


class Grouping(unittest.TestCase):
    def jobs(self, n):
        return [(f'{i:040x}-1', [f'{i:040x}'], script('AA' * 32, fn=f'f{i}'), f'{i:064x}') for i in range(n)]

    def test_groups_are_deterministic_and_cover_every_pipeline_once(self):
        jobs = self.jobs(500)
        a, b = dp.plan_groups(jobs, 16), dp.plan_groups(list(reversed(jobs)), 16)
        self.assertEqual([(n, d, [m[0] for m in ms]) for n, d, ms in a], [(n, d, [m[0] for m in ms]) for n, d, ms in b])
        self.assertLessEqual(len(a), 16)
        members = [m[0] for _, _, ms in a for m in ms]
        self.assertEqual(sorted(members), sorted(j[0] for j in jobs))
        for _, _, ms in a: self.assertEqual([m[0] for m in ms], sorted(m[0] for m in ms))
        self.assertEqual(len({n for n, _, _ in a}), len(a))
        for n, _, _ in a: self.assertNotIn('/', n)

    def test_a_new_pipeline_changes_only_its_own_group(self):
        jobs = self.jobs(300)
        before = {n for n, _, _ in dp.plan_groups(jobs, 16)}
        after = {n for n, _, _ in dp.plan_groups(jobs + [('f' * 40 + '-1', ['f' * 40], script('BB' * 32), 'e' * 64)], 16)}
        self.assertEqual(len(before - after), 1); self.assertEqual(len(after - before), 1)

    def test_merge_keeps_every_pipeline_and_each_library_once(self):
        m = dp.merge([script('AA' * 32, 'BB' * 32), script('aa' * 32, kind='compute_pipelines'), script('CC' * 32)])
        self.assertEqual([p['label'] for p in m['libraries']['paths']], ['AA' * 32, 'BB' * 32, 'CC' * 32])
        self.assertEqual({k: len(v) for k, v in m['pipelines'].items()}, {'render_pipelines': 2, 'compute_pipelines': 1})
        self.assertEqual(m['functions'], {}); self.assertEqual(m['version'], script('AA')['version'])

    def test_a_script_merge_does_not_know_keeps_its_own_file(self):
        odd = script('AA' * 32); odd['functions'] = {'x': [1]}
        jobs = self.jobs(3) + [('e' * 40 + '-1', ['e' * 40], odd, 'd' * 64)]
        planned = dp.plan_groups(jobs, 1)
        self.assertIn(('e' * 40 + '-1', 'd' * 64), [(n, d) for n, d, _ in planned])
        self.assertEqual(sorted(len(ms) for _, _, ms in planned), [1, 3])


class Compile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); root = Path(self.tmp.name)
        self.seed, self.mac, self.cache = root / 'seed', root / 'mac', root / 'cache'
        for d in (self.seed, self.mac): d.mkdir()
        (self.seed / 'a.metallib').write_bytes(b'a'); (self.seed / 'b.metallib').write_bytes(b'b')
        (self.seed / 'seed-manifest.json').write_text(json.dumps({'chain': 'chain-x', 'files': {'a.metallib': 'aa' * 32, 'b.metallib': 'bb' * 32}}))
        (self.seed / 'pipelines-applegpu_g15p-old-1.metalarchive').write_bytes(b'old')   # the last build's
        self.scripts = {}
        for i in range(40):
            name = f'{i:040x}-1'
            (self.mac / f'{name}.metalarchive').write_bytes(name.encode()); (self.mac / f'{name}.keys').write_text(f'key{i:02d}\n')
            self.scripts[name] = script('AA' * 32, 'BB' * 32, fn=f'f{i}') if i % 2 else script('AA' * 32, fn=f'f{i}')
        self.tool = root / 'metal-tt'; self.tool.write_bytes(b'tt')
        self.calls = []   # pipelines per metal-tt run
        self.fail = lambda n: False

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *extra, arch='applegpu_g15p'):
        real_run = subprocess.run
        def fake_run(cmd, **kw):
            if 'metal-tt' in cmd:
                src = json.loads(Path(cmd[cmd.index('-arch') + 2]).read_text())
                n = sum(len(v) for v in src['pipelines'].values())
                self.calls.append(n)
                for p in src['libraries']['paths']: self.assertTrue(Path(p['path']).is_file(), p)
                if self.fail(n): return subprocess.CompletedProcess(cmd, 1, '', 'error: something went wrong\n')
                Path(cmd[cmd.index('-o') + 1]).write_text(json.dumps(src))
                return subprocess.CompletedProcess(cmd, 0, '', '')
            return real_run(cmd, **kw)
        argv = ['device_pipelines.py', '--mac', str(self.mac), '--seed', str(self.seed), '--cache', str(self.cache), '--arch', arch, *extra]
        out = io.StringIO()
        with mock.patch.object(sys, 'argv', argv), mock.patch.object(dp, 'tool', lambda *a: str(self.tool)), \
             mock.patch.object(dp, 'script_of', lambda p, *a: json.loads(json.dumps(self.scripts[p.stem]))), \
             mock.patch.object(dp.subprocess, 'run', fake_run), redirect_stdout(out):
            dp.main()
        self.out = out.getvalue()
        return json.loads(self.out.strip().splitlines()[-1])

    def index(self, arch='applegpu_g15p'):
        return dict(l.split('\t') for l in (self.seed / f'pipelines-{arch}.tsv').read_text().splitlines())

    def test_pipelines_are_merged_into_few_files_and_every_key_maps_to_its_file(self):
        r = self.run_main('--groups', '4')['archs'][0]
        files = sorted(self.seed.glob('pipelines-applegpu_g15p-*.metalarchive'))
        self.assertEqual(r['pipelines'], 40); self.assertEqual(r['files'], len(files)); self.assertLessEqual(len(files), 4)
        self.assertEqual(sum(self.calls), 40); self.assertEqual(len(self.calls), len(files))
        index = self.index()
        self.assertEqual(sorted(index), [f'key{i:02d}' for i in range(40)])
        for f in files:   # the file a key names carries that key's pipeline
            name = f.name[len('pipelines-applegpu_g15p-'):-len('.metalarchive')]
            fns = {p['vertex_function'] for v in json.loads(f.read_text())['pipelines'].values() for p in v}
            keys = [k for k, v in index.items() if v == name]
            self.assertEqual(fns, {self.scripts[f'{int(k[3:]):040x}-1']['pipelines']['render_pipelines'][0]['vertex_function'] for k in keys})
        self.assertFalse((self.seed / 'pipelines-applegpu_g15p-old-1.metalarchive').exists())
        self.assertEqual(json.loads((self.seed / 'pipelines-applegpu_g15p.json').read_text()),
                         {'schema': dp.SCHEMA, 'chain': 'chain-x', 'arch': 'applegpu_g15p', 'pipelines': 40})
        self.assertEqual(dp.seed_check(self.seed, 'applegpu_g15p')[0], 'ok')

    def test_a_second_build_compiles_nothing_and_names_the_same_files(self):
        self.run_main('--groups', '4'); first, names = len(self.calls), sorted(p.name for p in self.seed.glob('pipelines-*'))
        r = self.run_main('--groups', '4')['archs'][0]
        self.assertEqual(len(self.calls), first); self.assertEqual(r['kept'], 40)
        self.assertEqual(sorted(p.name for p in self.seed.glob('pipelines-*')), names)

    def test_a_failing_group_falls_back_to_one_file_per_pipeline(self):
        self.fail = lambda n: n > 1   # metal-tt refuses every merged script, takes single pipelines
        r = self.run_main('--groups', '2')['archs'][0]
        self.assertEqual(r['group_fallbacks'], 2); self.assertEqual(r['failed'], 0); self.assertEqual(r['pipelines'], 40)
        self.assertEqual(r['files'], 40); self.assertIn('failed as one file', self.out)
        self.assertEqual(self.index()['key00'], f'{0:040x}-1')   # the Mac file's own name, as before
        self.assertEqual(dp.seed_check(self.seed, 'applegpu_g15p')[0], 'ok')

    def test_a_pipeline_that_fails_alone_too_is_left_out(self):
        self.scripts[f'{3:040x}-1']['pipelines']['render_pipelines'][0]['bad'] = True
        real = dp.metal_tt
        def metal_tt(s, kept, *a):
            if any(p.get('bad') for v in s['pipelines'].values() for p in v): return 'error: bad pipeline'
            return real(s, kept, *a)
        with mock.patch.object(dp, 'metal_tt', metal_tt):
            r = self.run_main('--groups', '2')['archs'][0]
        self.assertEqual(r['group_fallbacks'], 1); self.assertEqual(r['failed'], 1); self.assertEqual(r['pipelines'], 39)
        self.assertNotIn('key03', self.index())
        self.assertEqual(dp.seed_check(self.seed, 'applegpu_g15p')[0], 'ok')

    def test_groups_zero_is_one_file_per_mac_file(self):
        r = self.run_main('--groups', '0')['archs'][0]
        self.assertEqual(r['files'], 40); self.assertEqual(self.index()['key05'], f'{5:040x}-1')

    def test_merged_by_default(self):
        # one file per pipeline used up the iPad's vnodes (jetsam vnode-limit, 2026-11-13; see device_pipelines' top)
        self.assertEqual(dp.GROUPS, 10)
        r = self.run_main()['archs'][0]
        self.assertLessEqual(r['files'], 10)
        self.assertTrue(self.index()['key07'].startswith('g'), 'keys name merged group files')


class SeedCheck(unittest.TestCase):
    def test_merged_files_shared_by_many_keys(self):
        with tempfile.TemporaryDirectory() as d:
            seed = Path(d); arch = 'applegpu_g15p'
            rows = [f'k{i}\tg{i % 3}-abc' for i in range(30)]
            (seed / f'pipelines-{arch}.tsv').write_text('\n'.join(rows) + '\n')
            (seed / f'pipelines-{arch}.json').write_text(json.dumps({'schema': dp.SCHEMA, 'chain': 'c', 'arch': arch, 'pipelines': 30}))
            for g in range(3): (seed / f'pipelines-{arch}-g{g}-abc.metalarchive').write_bytes(b'x')
            status, line = dp.seed_check(seed, arch)
            self.assertEqual(status, 'ok'); self.assertIn('30', line); self.assertIn('3 files', line)
            (seed / f'pipelines-{arch}-g1-abc.metalarchive').unlink()
            self.assertEqual(dp.seed_check(seed, arch)[0], 'wrong')


if __name__ == '__main__':
    unittest.main()
