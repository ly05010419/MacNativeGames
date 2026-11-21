#!/usr/bin/env python3
# scripts/bench.py: the measured window of the host's per-frame rows (DM_NVN_TSV) and the percentiles taken from it, and the profiles' bench.tsv routes
import argparse, contextlib, importlib.util, io, json, os, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('bench', REPO / 'scripts/bench.py'); bench = importlib.util.module_from_spec(spec); spec.loader.exec_module(bench)


class Bench(unittest.TestCase):
    def test_window_and_percentiles(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'frames.tsv'
            rows = ['# kind: nvn-per-frame', 'frame\tname\tvalue']
            for i in range(200):   # 100 frames before the window, 100 in it: 99 of 10 ms and one of 50 ms; the GPU time of the last frame in the window not known yet
                t = 10 + i * 0.01
                rows += [f'{i}\t@t_s\t{t:.3f}', f'{i}\t@frame_ms\t{50.0 if i == 150 else 10.0}', f'{i}\t@presented_ms\t{-1 if i == 160 else 8.333}']
                if i < 199: rows.append(f'{i}\t@gpu_ms\t{7.0 + (i % 10) * 0.1:.1f}')
                rows.append(f'{i}\tnvnQueueSubmitCommands\t3')   # call-count rows are not timing rows
            f.write_text('\n'.join(rows) + '\n')
            ft, shown, gpu, dropped, n = bench.frames(f, 11.0, 12.0)
            self.assertEqual(n, 100); self.assertEqual(len(ft), 100); self.assertEqual(dropped, 1); self.assertEqual(len(shown), 99); self.assertEqual(len(gpu), 99)
            s = bench.summary(ft, shown, gpu, dropped, n, 1.0, 1000 / 120)
            self.assertEqual(s['fps'], 100.0); self.assertEqual(s['frame_p50'], 10.0); self.assertEqual(s['frame_p99'], 10.0); self.assertEqual(s['frame_max'], 50.0)
            self.assertEqual(s['over_budget'], 1)   # only the 50 ms frame is longer than 1.5 × 8.33 ms

    def test_percentile(self):
        xs = list(range(1, 101))
        self.assertEqual(bench.pct(xs, 50), 50); self.assertEqual(bench.pct(xs, 99), 99); self.assertEqual(bench.pct(xs, 100), 100); self.assertIsNone(bench.pct([], 50))

    def test_profile_routes(self):   # every bench.tsv parses: four columns at least, numbers in the window columns
        for t in (REPO / 'dm/games').glob('*/bench.tsv'):
            rs = bench.routes(t.parent.name)
            self.assertTrue(rs, t)
            for name, r in rs.items(): self.assertTrue(r['keys'] and r['secs'] > 0 and r['from'] >= 0, f'{t}: {name}')

    def test_optional_metrics_across_runs(self):
        runs = [{'frames': 2, 'fps': 60, 'gpu_p50': None}, {'frames': 2, 'fps': 58, 'gpu_p50': 7, 'game_speed': 1.0},
                {'frames': 2, 'fps': 62, 'gpu_p50': 9}, {'frames': 0, 'fps': 1, 'game_speed': 0.1}]
        for order in (runs, list(reversed(runs))):
            row = bench.table({'base': order}, ['fps', 'gpu_p50', 'game_speed', 'missing']).splitlines()[-1]
            self.assertEqual(row, '| base | 3 | 60 (58–62) | 8 (7–9) | 1 | – |')

    def test_cli_relative_paths(self):
        with mock.patch.dict(os.environ), tempfile.TemporaryDirectory() as d:
            os.environ.pop('DM_PSO_BUNDLE', None)   # exercise the default regardless of the caller's archive override
            root = Path(d); gid = '01007EF00011E000'; build = root / f'{gid}_build'
            (build / 'run/save').mkdir(parents=True); (build / 'run/save/sentinel').write_text('original')
            (build / 'extract/v0/RomFS').mkdir(parents=True); (build / 'pso/mac').mkdir(parents=True)   # the offline archive the Mac app bundles
            (root / 'variant/patched_v0').mkdir(parents=True)
            (root / 'variant/msl').mkdir(); (root / 'variant/pso').mkdir(); (root / 'variant/shmap.tsv').write_text('# synthetic\n')
            host = root / 'variant/host'
            host.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
paths = [sys.argv[1]] + [os.environ[k] for k in ('DM_ROOT', 'DM_ROMFS', 'DM_SAVE', 'DM_TEMP', 'DM_MSL_DIR', 'DM_NVN_SHMAP', 'DM_NVN_TSV', 'DM_CTL', 'DM_PSO_BUNDLE')]
assert all(pathlib.Path(p).is_absolute() for p in paths), paths
assert pathlib.Path(sys.argv[1]).is_dir()
assert pathlib.Path(os.environ['DM_MSL_DIR']).is_dir()
assert pathlib.Path(os.environ['DM_PSO_BUNDLE']) == pathlib.Path(os.environ['DM_ROOT']) / 'pso/mac'
assert pathlib.Path(os.environ['DM_NVN_SHMAP']).is_file()
archive = os.environ['DM_PSO_ARCHIVE']
assert archive == 'off' or pathlib.Path(archive).is_absolute() and pathlib.Path(archive).is_dir()
save = pathlib.Path(os.environ['DM_SAVE']) / 'sentinel'
assert save.read_text() == 'original'
save.write_text('child changed its copy')
pathlib.Path(os.environ['DM_NVN_TSV']).write_text('1\\t@t_s\\t0.01\\n1\\t@frame_ms\\t10\\n')
pathlib.Path('paths.json').write_text(json.dumps(paths))
''')
            host.chmod(0o755)
            route = {'keys': '0:A', 'from': 0, 'secs': 0.1, 'about': 'synthetic'}
            cwd = Path.cwd()
            try:
                os.chdir(root)
                variant = 'base:host=variant/host,patched=variant/patched_v0,msl=variant/msl,shmap=variant/shmap.tsv'
                for extra in (['--variant', variant + ',pso=variant/pso'], ['--variant', variant, '--pso-archive', 'variant/pso'],
                              ['--variant', variant + ',pso=off'], ['--variant', variant, '--pso-archive', 'off']):
                    with mock.patch.object(sys, 'argv', ['bench.py', build.name, '--out', 'relative-results', '--runs', '1', '--no-hud'] + extra), \
                         mock.patch.object(bench, 'routes', return_value={'test': route}), contextlib.redirect_stdout(io.StringIO()):
                        with self.assertRaises(SystemExit) as ex: bench.main()
                    self.assertEqual(ex.exception.code, 0, (root / 'relative-results/base-1/log.txt').read_text())
                    results = json.loads((root / 'relative-results/results.json').read_text())
                    self.assertEqual(results['results']['base'][0]['frames'], 1)
                    self.assertTrue((root / 'relative-results/results.md').is_file())
                    self.assertEqual((build / 'run/save/sentinel').read_text(), 'original')
            finally:
                os.chdir(cwd)

    def test_failed_launch_releases_lock(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); build = root / 'build'; (build / 'patched_v0').mkdir(parents=True); (build / 'extract/v0/RomFS').mkdir(parents=True)
            save = root / 'source-save'; save.mkdir(); lock = root / 'bench.lock'
            a = argparse.Namespace(save=save, pso_archive=None, fps=None, scale=None, env=[], no_hud=True, shots=None)
            with self.assertRaises(FileNotFoundError):
                bench.run_one(a, '01007EF00011E000', build, {'host': str(root / 'missing-host')}, {'keys': '0:A', 'from': 0, 'secs': 1}, root / 'out', lock)
            self.assertFalse(lock.exists(), 'a failed launch must not block the next benchmark')


if __name__ == '__main__':
    unittest.main()
