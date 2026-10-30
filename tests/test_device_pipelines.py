"""games/totk/ios/tools/device_pipelines.py: the device's pipeline archive compiled on the Mac while packaging (2026-10-29)."""
import io
import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]/'games/totk/ios/tools'
sys.path.insert(0, str(TOOLS))
import device_pipelines  # noqa: E402


def script(*labels):
    return {'libraries': {'paths': [{'label': l, 'path': '/old/' + l} for l in labels]},
            'pipelines': {'render_pipelines': [{'vertex_function': f'alias:{labels[0]}#main0'}]}}


class Compile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); root = Path(self.tmp.name)
        self.seed, self.mac, self.cache = root/'seed', root/'mac', root/'cache'
        for d in (self.seed, self.mac): d.mkdir()
        (self.seed/'a.metallib').write_bytes(b'a'); (self.seed/'b.metallib').write_bytes(b'b')
        (self.seed/'seed-manifest.json').write_text(json.dumps({'chain': 'chain-x', 'files': {'a.metallib': 'aa' * 32, 'b.metallib': 'bb' * 32}}))
        (self.seed/'pipelines-applegpu_old-k0-1.metalarchive').write_bytes(b'old')   # the last build's
        (self.seed/'kept.metal').write_text('x')
        self.scripts = {}
        for name, labels in (('k1-1', ['AA' * 32]), ('k2-1', ['AA' * 32, 'BB' * 32]), ('k3-1', ['CC' * 32])):
            (self.mac/f'{name}.metalarchive').write_bytes(name.encode()); (self.mac/f'{name}.keys').write_text(name[:2] + '\n')
            self.scripts[name] = script(*labels)
        (self.mac/'cut-1.metalarchive').write_bytes(b'x')   # no key list: a write cut short
        self.compiled = []
        self.tool = root/'metal-tt'; self.tool.write_bytes(b'tt')

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *archs):
        real_run = subprocess.run
        def fake_run(cmd, **kw):
            if 'metal-tt' in cmd:
                src = json.loads(Path(cmd[cmd.index('-arch') + 2]).read_text())
                self.compiled.append([p['path'] for p in src['libraries']['paths']])
                Path(cmd[cmd.index('-o') + 1]).write_bytes(b'gpu code')
                return subprocess.CompletedProcess(cmd, 0, '', '')
            return real_run(cmd, **kw)
        argv = ['device_pipelines.py', '--mac', str(self.mac), '--seed', str(self.seed), '--cache', str(self.cache), *[x for a in archs for x in ('--arch', a)]]
        out = io.StringIO()
        with mock.patch.object(sys, 'argv', argv), mock.patch.object(device_pipelines, 'tool', lambda *a: str(self.tool)), \
             mock.patch.object(device_pipelines, 'script_of', lambda p, *a: json.loads(json.dumps(self.scripts[p.stem]))), \
             mock.patch.object(device_pipelines.subprocess, 'run', fake_run), redirect_stdout(out):
            device_pipelines.main()
        return json.loads(out.getvalue().strip().splitlines()[-1])

    def test_pipelines_of_seed_libraries_are_compiled_into_the_seed_flat(self):
        r = self.run_main('applegpu_g14p')
        self.assertEqual(r['archs'][0]['pipelines'], 2); self.assertEqual(r['library from source'], 1)
        self.assertEqual(sorted(p.name for p in self.seed.glob('pipelines-*')),
                         ['pipelines-applegpu_g14p-k1-1.metalarchive', 'pipelines-applegpu_g14p-k2-1.metalarchive',
                          'pipelines-applegpu_g14p.json', 'pipelines-applegpu_g14p.tsv'])
        self.assertEqual((self.seed/'pipelines-applegpu_g14p.tsv').read_text(), 'k1\tk1-1\nk2\tk2-1\n')
        self.assertEqual(json.loads((self.seed/'pipelines-applegpu_g14p.json').read_text()),
                         {'schema': device_pipelines.SCHEMA, 'chain': 'chain-x', 'arch': 'applegpu_g14p', 'pipelines': 2})
        self.assertIn([str(self.seed.resolve()/'a.metallib'), str(self.seed.resolve()/'b.metallib')], self.compiled)   # the seed's own libraries
        self.assertTrue((self.seed/'kept.metal').is_file())

    def test_a_second_build_compiles_nothing_again(self):
        self.run_main('applegpu_g14p'); first = len(self.compiled)
        r = self.run_main('applegpu_g14p')
        self.assertEqual(len(self.compiled), first); self.assertEqual(r['archs'][0]['kept'], 2)

    def test_without_a_gpu_the_seed_carries_none(self):
        self.run_main('applegpu_g14p')
        self.assertEqual(self.run_main(), {'archs': []})
        self.assertEqual(list(self.seed.glob('pipelines-*')), [])


class Variants(unittest.TestCase):
    """The patched variants the runtime compiles from source, compiled for the device into the seed by their source's hash."""
    def test_recorded_sources_become_seed_libraries_and_stale_ones_go(self):
        import hashlib
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); seed, rec, cache = root/'seed', root/'variants', root/'cache'
            seed.mkdir(); rec.mkdir()
            src = b'fragment float4 f() { return 1; }'; digest = hashlib.sha256(src).hexdigest()
            (rec/f'{digest}.metal').write_bytes(src)
            (rec/f'{"0" * 64}.metal').write_bytes(b'renamed')    # its name is not its hash: left out
            (seed/'variant-old.metallib').write_bytes(b'old')
            calls = []; real_run = subprocess.run
            def fake_run(cmd, **kw):
                if cmd[0] == 'cp': return real_run(cmd, **kw)
                calls.append(cmd)
                if '--version' in cmd: return subprocess.CompletedProcess(cmd, 0, 'metal 1\n', '')
                Path(cmd[cmd.index('-o') + 1]).write_bytes(b'lib')
                return subprocess.CompletedProcess(cmd, 0, '', '')
            with mock.patch.object(device_pipelines.subprocess, 'run', fake_run):
                r = device_pipelines.variants(rec, seed, cache, 'iphoneos', {})
                self.assertEqual(r, {'variants': 1, 'compiled': 1, 'kept': 0, 'failed': 0})
                self.assertIn('-fmetal-math-mode=safe', next(c for c in calls if '-c' in c))   # the seed's own options
                self.assertEqual(device_pipelines.variants(rec, seed, cache, 'iphoneos', {})['kept'], 1)
            self.assertEqual(sorted(p.name for p in seed.iterdir()), [f'variant-{digest}.metallib'])


class Devices(unittest.TestCase):
    def test_only_wired_iphones_and_ipads_by_their_gpu(self):
        devices = [{'hardwareProperties': {'platform': 'iOS', 'productType': 'iPhone14,7'}, 'deviceProperties': {'name': 'phone'},
                    'connectionProperties': {'transportType': 'wired'}},
                   {'hardwareProperties': {'platform': 'iOS', 'productType': 'iPad15,7'}, 'deviceProperties': {'name': 'tablet'},
                    'connectionProperties': {'transportType': 'localNetwork'}},
                   {'hardwareProperties': {'platform': 'iOS', 'productType': 'iPhone99,1'}, 'deviceProperties': {'name': 'new'},
                    'connectionProperties': {'transportType': 'wired'}}]
        def fake_run(cmd, **kw):
            Path(cmd[cmd.index('--json-output') + 1]).write_text(json.dumps({'result': {'devices': devices}}))
            return subprocess.CompletedProcess(cmd, 0, b'', b'')
        with mock.patch.object(device_pipelines.subprocess, 'run', fake_run):
            self.assertEqual(device_pipelines.connected_gpus({}), [('phone', 'iPhone14,7', 'applegpu_g14p'), ('new', 'iPhone99,1', None)])


if __name__ == '__main__':
    unittest.main()
