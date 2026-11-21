#!/usr/bin/env python3
# scripts/regress.py: the parts that need no game, GPU or git revision: the MSL interface reader (GitHub issue #283), the [RED] normalization, the routes, and the comparisons
import importlib.util, json, tempfile, unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace as NS
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('regress', REPO / 'scripts/regress.py')
assert spec is not None and spec.loader is not None
regress = importlib.util.module_from_spec(spec); spec.loader.exec_module(regress)

VS = '''struct main0_out
{
    float4 m_51 [[user(locn0)]];
    float4 m_55 [[user(locn2), flat]];
    float4 gl_Position [[position, invariant]];
};

struct main0_in
{
    float4 a0 [[attribute(0)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
}

vertex main0_out main0_z(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
}
'''
PS = '''struct main0_out
{
    float4 c0 [[color(0)]];
};

struct main0_in
{
    float4 m_1 [[user(locn0)]];
    float4 m_2 [[user(locn1)]];
    int4 m_3 [[user(locn2), flat]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
}
'''


class Regress(unittest.TestCase):
    def test_impact_without_machine_paths(self):
        env = {k: v for k, v in regress.os.environ.items() if k not in ('REGRESS_ROOT', 'REGRESS_GAMES')}
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            command = [regress.sys.executable, REPO / 'scripts/regress.py']
            result = regress.sh([*command, 'impact', 'HEAD..HEAD'], cwd=root, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('## regress.py impact', result.stdout)
            self.assertEqual(list(root.iterdir()), [])   # an unset root must not become cwd/reports
            out = root / 'explicit'
            result = regress.sh([*command, 'impact', 'HEAD..HEAD', '--out', out], cwd=root, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads((out / 'impact.json').read_text())['schema'], 'nxnative-impact/1')
            self.assertTrue((out / 'impact.md').is_file())
            harness = root / 'harness'; harness.mkdir()
            result = regress.sh([*command, '--root', harness, 'impact', 'HEAD..HEAD'], cwd=root, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(list((harness / 'reports').glob('*/impact.json'))), 1)

    def test_game_commands_require_machine_paths(self):
        env = dict(regress.os.environ, REGRESS_ROOT='', REGRESS_GAMES='')
        with tempfile.TemporaryDirectory() as td:
            command = [regress.sys.executable, REPO / 'scripts/regress.py']
            for cmd in ('baseline', 'check', 'chain', 'usage', 'list'):
                with self.subTest(cmd=cmd):
                    result = regress.sh([*command, cmd], cwd=td, env=env)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn('set --root (REGRESS_ROOT) and --games-dir (REGRESS_GAMES)', result.stderr)
            for option, missing in (('--root', '--games-dir (REGRESS_GAMES)'), ('--games-dir', '--root (REGRESS_ROOT)')):
                with self.subTest(option=option):
                    result = regress.sh([*command, option, td, 'check'], cwd=td, env=env)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn('set ' + missing + ':', result.stderr)
            self.assertEqual(list(Path(td).iterdir()), [])

    def test_interface(self):
        vo, fi = regress.msl_io(VS, 'main0_z', 'vs'), regress.msl_io(PS, 'main0', 'ps')
        self.assertEqual(vo, {'locn0': 'float4', 'locn2': 'float4'})
        self.assertEqual(fi, {'locn0': 'float4', 'locn1': 'float4', 'locn2': 'int4'})
        self.assertIsNone(regress.msl_io(VS, 'main1', 'vs'))
        self.assertEqual(regress.msl_io('fragment main0_out main0(constant X& x [[buffer(0)]])\n{\n}\n', 'main0', 'ps'), {})   # no stage inputs

    def test_red_normalized(self):
        a = regress.normalize_red('[dm] [RED] shader 0a91af5bef8578e2: 12 bindings at 0x1f00 in /Volumes/NX/games/x [y]/b')
        b = regress.normalize_red('[dm] [RED] shader 1b91af5bef8578e3: 7 bindings at 0x2000 in /tmp/z')
        self.assertEqual(a, b)
        # Scarlet: one red line, its register zero in one run ("x1=0") and not in the other ("x1=0x1")
        a = regress.normalize_red('[dm] [RED] NVN unimplemented function called: nvnCommandBufferSetConservativeRasterDilate (x0=0x7f3c2a10 x1=0 x2=0x3)')
        b = regress.normalize_red('[dm] [RED] NVN unimplemented function called: nvnCommandBufferSetConservativeRasterDilate (x0=0x7f3c2a10 x1=0x1 x2=0x3)')
        self.assertEqual(a, b)
        self.assertEqual(regress.red_kinds(a.replace('x#=#', 'x#=0x#')), [a])   # a kind stored in an older baseline

    def test_red_missing_libraries(self):   # Tears of the Kingdom, 2026-10-28: ~2,000 shaders per run the chain build does not hold, other names each run
        M = regress.MISSING_LIB
        lib = '/Volumes/T7/nsp/The Legend of Zelda Tears of the Kingdom/0100F2C0115B6000-work/regress/chain/nvn-a78f966a7b38-922b2b7671c9-7fb6953e58a81bc8/b-a78f966a7b38'
        fused = (f'[dm] [nvn-mtl] [RED] failed to load shader c41c950044f80287_b: cannot open {lib}/c41c950044f80287_b.metallib'
                 f'[dm] [nvn-mtl] [RED] failed to load shader 3470b30952386168_b: cannot open {lib}/3470b30952386168_b.metallib')
        self.assertEqual([regress.normalize_red(m) for m in regress.red_messages(fused)], [M, M])   # two threads' messages on one line (LOG before the stream lock)
        self.assertEqual(regress.normalize_red('[dm] [nvn-mtl] [RED] failed to load shader 7ae4bdc2749fd127_b: no reflection json'), M)
        self.assertEqual(regress.normalize_red('[dm] [nvn-mtl] [RED] shader 607bd409ec4ee6e4 (pixel) failed to load: build the shaders with dm/gfx/tools/dmsh_build.py first'), M)
        # a library Metal could not load is not a missing one; a key with a suffix is still a key
        self.assertEqual(regress.normalize_red('[dm] [nvn-mtl] [RED] failed to load shader 7ae4bdc2749fd127_b: Function main0 has a deserialization error'),
                         'failed to load shader <key>_b: Function main# has a deserialization error')
        self.assertEqual(regress.normalize_red(f'[dm] [nvn-mtl] [RED] pulling library of 1cbf4bb2a0d39e17_b not usable (no library: cannot open {lib}/x)'),
                         regress.normalize_red(f'[dm] [nvn-mtl] [RED] pulling library of 9e0d17c3b4a5f601_b not usable (no library: cannot open {lib}/y)'))
        text = '\n'.join([fused, '[dm] [nvn-mtl] [RED] failed to load shader 7ae4bdc2749fd127_b: no reflection json', '[dm] frame 3'])
        self.assertEqual(regress.read_log(text)[0], {M: 3})
        # kinds stored by the harness before (keys cut into "#" fragments, fused messages, cut at 200 characters) read as the one kind
        old = {'failed to load shader #c#f#dce#a_b: cannot open <path> [nvn-mtl] [RED] failed to load shader f#a#cdfe#a#eb_b: cannot open <path>': 1,
               'failed to load shader #d#c#cd#ad_b: cannot open <path> Legend of Zelda Tears of the Kingdom<path> [nvn-mtl] [RED] failed to load shader #e#a#ba#_b: no reflection json': 1,
               'failed to load shader #e#d#e#fbd#_b: cannot open <path> Legend of Zelda Tears of the Kingdom<path> [nvn-mtl] [RED] failed to load shader #e#d#e#fbd#_b: cannot open <path> Legend of Zelda Tears of the ': 1,
               'shader #bd#f#e#c (pixel) failed to load: build the shaders with dm<path> first': 7, 'NVN unimplemented function called: x (x#=#)': 2,
               'pulling library of #f#c#dbf_b not usable (no library: cannot open <path> Legend of Zel': 1, 'compile of #a#e_b failed: x': 1}
        for tail in ('failed to load shader f#d#d#e#a#c#_b: no refle', 'failed to', 'fail'):   # the cut-off last message of a 200-character kind
            head, sep = 'failed to load shader #a#e_b: cannot open <path> Legend of Zelda Tears of the Kingdom<path>', ' [nvn-mtl] [RED] '
            old[head + ' ' * (200 - len(head) - len(sep) - len(tail)) + sep + tail] = 1
        self.assertEqual(regress.red_counts({'red': old}), {M: 20, 'NVN unimplemented function called: x (x#=#)': 2, 'compile of <key>_b failed: x': 1})
        route = {'reach': None}
        base = [{'exit': None, 'max_frame': 500, 'red': old}, {'exit': None, 'max_frame': 500, 'red': {M: 2303}}]
        lv, known, d = regress.smoke_diff(base, [{'exit': None, 'max_frame': 500, 'red': {M: 1641, 'NVN unimplemented function called: x (x#=#)': 1, 'compile of <key>_b failed: x': 1}}], route)
        self.assertFalse(lv['REGRESSION'] or lv['CHANGED'], lv)
        lv, known, d = regress.smoke_diff(base, [{'exit': None, 'max_frame': 500, 'red': {M: 9000}}], route)   # only a jump far beyond the run-to-run spread is news
        self.assertTrue(any('missing shader libraries' in x for x in lv['CHANGED']), lv)
        lv, known, d = regress.smoke_diff([{'exit': None, 'max_frame': 500, 'red': {}}], [{'exit': None, 'max_frame': 500, 'red': {M: 3}}], route)
        self.assertEqual(d['new_red'], [{'text': M, 'count': 3}])   # a game whose baseline holds every shader it asks for

    def test_hang_evidence(self):   # a hung host is asked for its state dump and sampled before the watchdog stops it
        with tempfile.TemporaryDirectory() as td:
            cwd = Path(td); (cwd / 'ctl').write_text('shot x.png\n')
            def fake(cmd, **kw):
                self.assertEqual(cmd[:2], ['sample', '4242']); Path(cmd[cmd.index('-file') + 1]).write_text('Call graph:\n'); return NS(returncode=0)
            with patch.object(regress.time, 'sleep'), patch.object(regress.subprocess, 'run', fake):
                self.assertEqual(regress.hang_evidence(NS(pid=4242), cwd), 'hang_sample.txt')
            self.assertEqual((cwd / 'ctl').read_text(), 'shot x.png\ndump\n')
            with patch.object(regress.time, 'sleep'), patch.object(regress.subprocess, 'run', side_effect=OSError):
                self.assertIsNone(regress.hang_evidence(NS(pid=4242), cwd))

    def test_routes(self):   # every profile's smoke.tsv reads
        for f in (REPO / 'dm/games').glob('*/smoke.tsv'):
            r = regress.read_route(f)
            self.assertTrue(r['secs'] > 0 and r['quick'] > 0, f)
            self.assertIsInstance(r['keys'], str, f)   # title-screen routes need no input
            if r['reach']: self.assertTrue(r['reach'][1] <= r['secs'], f)

    def test_route_keys_are_optional_and_preserved(self):
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / 'smoke.tsv'
            for row, keys in (('', ''), ('keys\t\n', ''), ('keys\t1:A,2:B\n', '1:A,2:B')):
                with self.subTest(row=row):
                    f.write_text('save\tnew\nsecs\t40\nquick\t35\nreach\t1200@30\n' + row)
                    r = regress.read_route(f)
                    self.assertEqual(r['keys'], keys)
                    self.assertEqual(r['save'], 'new')
                    self.assertEqual(r['reach'], (1200, 30.0))

    def test_chain_diff(self):
        b = {'shaders': {'a': {'st': 'ok', 'msl': '1'}, 'b': {'st': 'ok', 'msl': '2'}, 'c': {'st': 'metal failed'}, 'd': {'st': 'ok', 'msl': '4'}},
             'interface': {'bad': [{'vs': 'v', 'entry': 'main0', 'ps': 'p', 'missing': ['locn1'], 'type': []}]}}
        c = {'shaders': {'a': {'st': 'ok', 'msl': '1'}, 'b': {'st': 'metal failed', 'err': 'x'}, 'c': {'st': 'ok', 'msl': '3'}, 'd': {'st': 'ok', 'msl': '5'}, 'e': {'st': 'translation failed', 'err': 'y'}},
             'interface': {'bad': [{'vs': 'v', 'entry': 'main0', 'ps': 'p', 'missing': ['locn1'], 'type': []}, {'vs': 'w', 'entry': 'main0', 'ps': 'p', 'missing': ['locn6'], 'type': []}]}}
        d = regress.chain_diff(b, c)
        self.assertEqual([x['key'] for x in d['newly_failing']], ['b']); self.assertEqual(d['newly_passing'], ['c'])
        self.assertEqual([x['key'] for x in d['added_failing']], ['e'])   # shader_regressions.py does not count a new key; the harness lists it apart
        self.assertEqual(d['changed']['msl'], 1); self.assertEqual(d['changed']['keys'], ['d'])
        self.assertEqual([x['vs'] for x in d['interface']['new']], ['w']); self.assertEqual(len(d['interface']['known']), 1)

    def test_smoke_diff(self):
        route = {'reach': (100, 30.0)}
        shot = {'dhash': '0f0f0f0f0f0f0f0f', 'rgb16': '10' * 768}
        base = [{'exit': None, 'max_frame': 500, 'reached': True, 'fps': 60, 'frame_p99': 17, 'peak_footprint_mb': 4000, 'prepare_s': 1.0, 'red': {'a #': 2}, 'shots': {'45': shot}}]
        same = [dict(base[0])]
        lv, known, d = regress.smoke_diff(base, same, route)
        self.assertFalse(lv['NEW-FAIL'] or lv['REGRESSION'] or lv['CHANGED'], lv)
        bad = [dict(base[0], exit=-11, crash={'signal': 'SIGSEGV'}, red={'a #': 1, 'b #': 3}, fps=40, shots={'45': {'dhash': 'f0f0f0f0f0f0f0f0', 'rgb16': '80' * 768}})]
        lv, known, d = regress.smoke_diff(base, bad, route)
        self.assertTrue(any('crash' in x for x in lv['NEW-FAIL']), lv)
        self.assertTrue(any('RED' in x for x in lv['REGRESSION']) and any('fps' in x for x in lv['REGRESSION']) and any('screenshot' in x for x in lv['REGRESSION']), lv)
        self.assertEqual(d['new_red'], [{'text': 'b #', 'count': 3}])
        lv, known, d = regress.smoke_diff(bad, bad, route)   # a failure the baseline has too is known, not new
        self.assertFalse(lv['NEW-FAIL']); self.assertTrue(known)
        slow = [dict(base[0], reached=False, max_frame=90)]   # the reach missed in both, each run with its own max frame: known, not new
        lv, known, d = regress.smoke_diff(slow, [dict(slow[0], max_frame=95)], route)
        self.assertFalse(lv['NEW-FAIL'], lv); self.assertEqual(known, ['frame 100 not reached by 30 s (max 95)'])
        self.assertTrue(regress.smoke_diff(base, slow, route)[0]['NEW-FAIL'])   # missed only in the candidate: new
        segv, abrt = [dict(base[0], exit=-11, crash={'signal': 'SIGSEGV'})], [dict(base[0], exit=-6, crash={'signal': 'SIGABRT'})]
        self.assertTrue(regress.smoke_diff(segv, abrt, route)[0]['NEW-FAIL'])   # a crash with another signal is new
        q1, q139 = [dict(base[0], exit=1)], [dict(base[0], exit=139)]
        self.assertTrue(regress.smoke_diff(q1, q139, route)[0]['NEW-FAIL'])   # so is an exit with another status
        self.assertFalse(regress.smoke_diff(q1, [dict(base[0], exit=1)], route)[0]['NEW-FAIL'])
        quit0 = [dict(base[0], exit=0)]   # the game ended by itself before the route did: a failure even with exit status 0
        self.assertTrue(any('exited before the end' in x for x in regress.smoke_diff(base, quit0, route)[0]['NEW-FAIL']))

    def test_route_capture_frame(self):
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / 'smoke.tsv'
            f.write_text('save\tnew\nsecs\t40\n')
            self.assertIsNone(regress.read_route(f)['capture'])
            f.write_text('save\tnew\nsecs\t40\ncapture\t3000\n')
            self.assertEqual(regress.read_route(f)['capture'], 3000)

    def test_parse_replays(self):
        live, other = 'a' * 64, 'b' * 64
        text = (f'replay 1: digest {live} == live {live} (1920x1080 format 70); submit CPU 1 ms\nMATCH: the replayed frame equals the live frame\n'
                f'replay 2: 97 pixels differ from the live frame (largest byte difference 46, x 1648-1676 y 739-746)\n'
                f'replay 2: digest {other} != live {live} (1920x1080 format 70); submit CPU 1 ms\n'
                f'replay 3: digest {other} != live {live} (1920x1080 format 70)\n2 of 3 fresh replays match the live frame (within 100 pixels)\n')
        r = regress.parse_replays(text)
        self.assertEqual([x['index'] for x in r], [1, 2, 3])
        self.assertEqual((r[0]['same'], r[0]['pixels']), (True, 0))
        self.assertEqual((r[1]['same'], r[1]['pixels'], r[1]['largest'], r[1]['box']), (False, 97, 46, [1648, 1676, 739, 746]))
        self.assertIsNone(r[2]['pixels'])   # a capture without the live pixels: compared by digest only

    def test_replay_grade(self):
        live = 'a' * 64
        def res(*px, exit=None, red=()):
            xs = [{'digest': live if p == 0 else f'{p:064x}', 'same': p == 0, 'live': live, 'pixels': p, 'largest': 9 if p else 0, 'box': [0, 1, 0, 1] if p else None} for p in px]
            return {'exit': exit if exit is not None else 0 if all(p <= 120 for p in px) else 1, 'replays': xs, 'red': list(red)}
        lv, known = regress.replay_grade(res(0, 0, 0), res(0, 0, 0), 0)
        self.assertFalse(any(lv.values()), lv)
        lv, known = regress.replay_grade(res(0, 97), res(124, 157), 124)   # a noisy capture: other frames within the tolerance (twice the noise) are no change
        self.assertFalse(any(lv.values()), lv)
        lv, known = regress.replay_grade(res(0, 0), res(0, 3), 0)   # an exact one: any other frame is a regression
        self.assertTrue(lv['REGRESSION'], lv)
        lv, known = regress.replay_grade(res(0, 97), res(0, 500), 124)
        self.assertTrue(lv['REGRESSION'], lv)
        lv, known = regress.replay_grade(res(500), res(600), 124)   # beyond the tolerance in both: known, the difference shown
        self.assertFalse(lv['REGRESSION']); self.assertTrue(known and lv['CHANGED'])
        lv, known = regress.replay_grade(res(0), res(exit=2, red=['[RED] nvn_replay: program #3 ...']), 0)
        self.assertTrue(any('capture' in x for x in lv['ERROR']), lv)
        lv, known = regress.replay_grade(res(0), res(exit=-11), 0)
        self.assertTrue(lv['NEW-FAIL'], lv)

    def test_status_order(self):
        self.assertEqual(regress.worst('PASS', 'CHANGED', 'REGRESSION'), 'REGRESSION'); self.assertEqual(regress.worst('NEW-FAIL', 'ERROR'), 'ERROR')

    def test_selection_keeps_games_without_routes(self):
        games = [NS(id='a', name='routed', kind='nvn', derived=False, route=lambda: {'save': 'new'}), NS(id='b', name='unrouted', kind='nvn', derived=False, route=lambda: None)]
        with patch.object(regress, 'find_games', return_value=games), patch.object(regress, 'say'):
            self.assertEqual(regress.select(NS(games=None), smoke=True), games)

    def test_no_smoke_checks_every_chain_without_building_hosts(self):
        route = {'save': 'new', 'keys': '1:A', 'secs': 20, 'quick': 10, 'reach': (1, 5)}
        games = [NS(id='a', name='routed', route=lambda: route), NS(id='b', name='unrouted', route=lambda: None)]
        tree = NS(id='tree')
        manifest = {'kind': 'nvn', 'shaders': {}, 'failed': 0}
        a = NS(baseline='base', candidate='cand', no_smoke=True, rewrite=False, quick=True, runs=1)
        with tempfile.TemporaryDirectory() as td, ExitStack() as stack:
            stack.enter_context(patch.object(regress, 'ROOT', Path(td)))
            stack.enter_context(patch.object(regress, 'Tree', return_value=tree))
            stack.enter_context(patch.object(regress, 'select', return_value=games))
            stack.enter_context(patch.object(regress, 'rewriter_key', return_value='same'))
            chain = stack.enter_context(patch.object(regress, 'chain_build', return_value=(manifest, Path(td))))
            hosts = stack.enter_context(patch.object(regress, 'artifacts', side_effect=AssertionError('a static check built a host')))
            smoke = stack.enter_context(patch.object(regress, 'smoke_series', side_effect=AssertionError('a static check ran a game')))
            stack.enter_context(patch.object(regress, 'new_report', return_value={'games': {}}))
            finish = stack.enter_context(patch.object(regress, 'finish', return_value=0))
            stack.enter_context(patch.object(regress, 'say'))
            self.assertEqual(regress.cmd_check(a), 0)
            self.assertEqual(chain.call_count, 4)
            hosts.assert_not_called(); smoke.assert_not_called()
            self.assertEqual([g['status'] for g in finish.call_args.args[0]['games'].values()], ['PASS', 'PASS'])

    def test_reused_chain_rechecks_current_interface(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); g = NS(id='game', kind='nvn', folder=root)
            out = regress.game_root(g) / 'chain/key'; out.mkdir(parents=True)
            (out / 'manifest.json').write_text(json.dumps({'kind': 'nvn', 'interface': {'bad': []}}))
            current = {'bad': [{'vs': 'new-pair'}]}
            with patch.object(regress, 'ROOT', root), patch.object(regress, 'chain_key', return_value=('v', 'i', 'key')), patch.object(regress, 'interface_check', return_value=current) as check:
                manifest, _ = regress.chain_build(NS(), g, NS(tools_fp='', jobs=1))
            check.assert_called_once()
            self.assertEqual(manifest['interface'], current)

    def test_variant_hash_ignores_chain_stamps(self):   # 2026-10-31: DKC Tropical Freeze reported 720 changed shaders whose variants differed only in the stamps
        k = '0123456789abcdef'
        def lib(td, chain='c1', compiler='x1', bindings=(0,), msl='kernel void main0() {}', pull=b'\x01'):
            d = Path(td); stamp = {'chain': chain, 'chain_metal': 'mac', 'chain_compiler': compiler}
            (d / '_b_build.json').write_text(json.dumps([{'key': k, 'st': 'ok'}]))
            (d / f'{k}_b.json').write_text(json.dumps({'key': k + '_b', **stamp}))
            (d / f'{k}_bg0.json').write_text(json.dumps({'key': k + '_bg0', 'bindings': list(bindings), **stamp}))
            (d / f'{k}_bg0.metal').write_text(msl)
            (d / f'{k}_bp.metallib').write_bytes(pull)   # pull variants leave only their metallib here
            return regress.nvn_manifest(d, 1)[k]['var']
        def var(**kw):
            with tempfile.TemporaryDirectory() as td: return lib(td, **kw)
        base = var()
        self.assertEqual(var(chain='c2', compiler='x2'), base)
        for change in ({'bindings': (1,)}, {'msl': 'kernel void main0() { }'}, {'pull': b'\x02'}):
            self.assertNotEqual(var(**change), base, change)

    def test_reused_manifest_of_older_hashes_is_hashed_again(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); g = NS(id='game', kind='nvn', folder=root)
            out = regress.game_root(g) / 'chain/key'; (out / 'b-v').mkdir(parents=True)
            (out / 'b-v/_b_build.json').write_text('[]')
            (out / 'manifest.json').write_text(json.dumps({'kind': 'nvn', 'shaders': {'old': {}}, 'interface': {'bad': []}}))
            fresh = {'k': {'st': 'ok', 'var': 'new'}}
            with patch.object(regress, 'ROOT', root), patch.object(regress, 'chain_key', return_value=('v', 'i', 'key')), \
                    patch.object(regress, 'interface_check', return_value={'bad': []}), patch.object(regress, 'nvn_manifest', return_value=fresh) as hashed:
                manifest, _ = regress.chain_build(NS(), g, NS(tools_fp='', jobs=1))
                self.assertEqual((manifest['shaders'], manifest['hashes']), (fresh, regress.NVN_HASHES))
                self.assertEqual(json.loads((out / 'manifest.json').read_text())['hashes'], regress.NVN_HASHES)
                regress.chain_build(NS(), g, NS(tools_fp='', jobs=1))
            hashed.assert_called_once()   # a manifest of the current hashes is reused as it is

    def test_smoke_requires_prebuild_and_screenshots(self):
        route = {'reach': (100, 10.0), 'shots': [8]}
        good = {'exit': None, 'max_frame': 500, 'reached': True, 'prebuild': {'exit': 0, 'done': True, 'failed': 0},
                'shots': {'8': {'dhash': '0' * 16, 'rgb16': '00' * 768}}}
        for prebuild in ({'exit': -11, 'hang': False}, {'exit': None, 'hang': False}, {'exit': 0, 'done': False}):
            with self.subTest(prebuild=prebuild):
                bad = dict(good, prebuild=prebuild)
                self.assertTrue(regress.smoke_diff([good], [bad], route)[0]['NEW-FAIL'])
        self.assertTrue(regress.smoke_diff([good], [dict(good, shots={})], route)[0]['NEW-FAIL'])
        self.assertTrue(regress.run_failures({'exit': None, 'max_frame': -1, 'reached': False}, {'reach': None}))

    def test_smoke_detects_archive_profile_mismatches(self):
        # The harness makes its own archive, so a profile/runtime mismatch must also fail without a bundled archive.
        depth = '[dm] [nvn-mtl] depth mode: near=0, vertex shaders use the main0_z entry\n'
        rejected = ('[dm] [nvn-mtl] pipeline archive bundled with the app: made for chain b3ca4b7c621f; depth near=-w; '
                    'pairs 255241 f6cf106d01da3da8, this run needs chain b3ca4b7c621f; depth near=0; pairs 255241 f6cf106d01da3da8: '
                    'the device builds the bases it does not hold (the game\'s depth mode is not the one built for: the profile key depth_mode)\n')
        cases = [(False, depth, 'profile depth_mode gl'), (True, depth, None),
                 (True, '[dm] [nvn] depth mode near=-w (GL)\n', 'profile depth_mode zero'),
                 (True, depth + rejected, 'bundled pipeline archive stamp'),
                 (True, depth + rejected.replace('depth near=-w', 'depth near=0').replace('made for chain b3ca4b7c621f', 'made for chain old'), 'bundled pipeline archive stamp'),
                 (True, depth + '[dm] [nvn-mtl] pipeline archive bundled with the app: made for (no prebuilt.tsv), this run needs chain current; depth near=0: the device builds the bases it does not hold\n', 'bundled pipeline archive stamp'),
                 (False, '', None), (False, '[dm] [nvn] depth mode near=-w (GL)\n', None),
                 (True, depth + '[dm] [nvn-mtl] pipeline archive bundled with the app: holds the bases of every shader pair (16 of 16 probed bases found, 26.0 ms)\n', None)]
        route = dict(secs=10, quick=10, prebuild='off', keys='', reach=None, shots=[], hang=20)
        for zero, text, failure in cases:
            with self.subTest(zero=zero, text=text), tempfile.TemporaryDirectory() as td:
                out = Path(td)
                def watched(cmd, env, cwd, log, *args, text=text, **kw):
                    log.write_text(text)
                    return dict(exit=None, hang_at_s=None, hang_sample=None, first_frame_s=1, max_frame=500, peak_mb=100, busy=False, presents=[])
                with patch.object(regress, 'run_env', return_value=({}, None)), patch.object(regress, 'run_watched', watched):
                    run = regress.smoke_run(NS(kind='nvn', id='game', get_depth_zero=zero), None,
                                            dict(host='host', modules='modules', host_key='h', modules_key='m'), route, True, out, None)
                failures = regress.run_failures(run, route)
                if failure:
                    self.assertTrue(any(failure in f for f in failures), failures)
                    good = dict(run, pipeline={})
                    self.assertTrue(regress.smoke_diff([good], [run], route)[0]['NEW-FAIL'])
                    self.assertIn('pipeline', regress.public_run(run))
                else: self.assertEqual(failures, [])

    def test_each_candidate_screenshot_is_checked(self):
        route = {'reach': None}
        good = {'exit': None, 'max_frame': 500, 'shots': {'8': {'dhash': '0' * 16, 'rgb16': '00' * 768}}}
        bad = dict(good, shots={'8': {'dhash': 'f' * 16, 'rgb16': 'ff' * 768}})
        self.assertTrue(regress.smoke_diff([good], [good, bad], route)[0]['REGRESSION'])

    def test_shader_build_pauses_for_unknown_lock_owner(self):
        with tempfile.TemporaryDirectory() as td:
            with patch.object(regress.subprocess, 'Popen') as spawn, patch.object(regress.bench, 'lock_holder', return_value=0), patch.object(regress.os, 'killpg') as kill, patch.object(regress.time, 'sleep'), patch.object(regress, 'say'):
                spawn.return_value.poll.side_effect = [None, 0, 0]
                regress.run_heavy(['build'], Path(td) / 'build.log', td, {}, NS(pause_lock=Path(td) / 'lock'), 'build')
            self.assertTrue(any(c.args[1] == regress.signal.SIGSTOP for c in kill.call_args_list))

    def test_idle_msbuild_workers_do_not_block_measurements(self):
        worker = '/usr/local/share/dotnet/dotnet /sdk/10.0.403/MSBuild.dll /nodemode:1 /nodeReuse:true'
        jobs = [(300, 1, 'S', '0.0', worker),
                (301, 1, 'S', '12.0', worker),
                (302, 1, 'U', '0.0', worker),
                (303, 70, 'S', '0.0', worker),
                (304, 1, 'S', '0.0', '/usr/local/share/dotnet/dotnet build project.csproj'),
                (305, 1, 'S', '0.0', worker.replace('/nodeReuse:true', '/nodeReuse:false'))]
        def process_list(command, **kwargs):
            has_cpu = '%cpu=' in command[-1]
            return NS(stdout='\n'.join(f'{pid} {parent} {state} ' + (f'{cpu} ' if has_cpu else '') + cmd for pid, parent, state, cpu, cmd in jobs))
        with patch.object(regress, 'sh', process_list):
            self.assertEqual(regress.heavy_jobs(), [f'{pid} {cmd}' for pid, _, _, _, cmd in jobs[1:]])

    def test_heavy_jobs_recognizes_harness_descendants_outside_shared_root(self):
        processes = '100 1 S 0.0 python3 /source/scripts/regress.py check\n101 100 S 0.0 python3 /trees/dmsh_build.py\n102 101 R 90.0 metal /games/title-work/regress/tmp/shader.metal\n103 1 R 90.0 metal /another/build.metal\n104 100 R 90.0 make dmhost\n'
        with patch.object(regress, 'sh', return_value=NS(stdout=processes)), patch.object(regress.os, 'getpid', return_value=200):
            self.assertEqual(regress.heavy_jobs(), ['103 metal /another/build.metal', '104 make dmhost'])

    def test_build_profile_runs_only_when_derived_or_unported(self):
        # A provisional profile the NSP script left from before the game's profile was added must not shadow dm/games' (it lacked OCTOPATH TRAVELER 0's tex_dyn_slots and fps_target)
        with tempfile.TemporaryDirectory() as td, patch.object(regress, 'REPO', Path(td) / 'repo'):
            root, gid = Path(td), '0123456789ABCDEF'
            build = root / f'{gid}_build'; (build / 'profile' / gid).mkdir(parents=True); (build / 'profile' / gid / 'game.tsv').write_text('# Provisional game profile\n')
            self.assertTrue(regress.Game(root, build).derived)                       # no profile in dm/games: the provisional one
            (root / 'repo/dm/games' / gid).mkdir(parents=True); (root / 'repo/dm/games' / gid / 'game.tsv').write_text('title_id\t' + gid + '\n')
            self.assertFalse(regress.Game(root, build).derived)                      # a profile in dm/games: it runs, not the leftover guesses
            (build / 'profile' / f'{gid}.derived.key').write_text('key\n')
            self.assertTrue(regress.Game(root, build).derived)                       # derived from it for another version

    def test_chain_key_tracks_shader_package_and_split_pairs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); pkg = root / 'shader/pkg'; pkg.mkdir(parents=True)
            shader = pkg / 'a.bin'; shader.write_bytes(b'original')
            prof = root / 'profile'; prof.mkdir()
            g = regress.Game(root, root / ('0' * 16 + '_build'))
            g.build, g.id, g.kind, g.derived = root, 'game', 'nvn', False
            tree = NS(path=root, py=lambda *args: '["chain", "inputs"]')
            with patch.object(g, 'profile_dir', return_value=prof):
                original = regress.chain_key(tree, g, 'tools')[2]
                shader.write_bytes(b'changed')
                changed = regress.chain_key(tree, g, 'tools')[2]
                (root / 'shader/pso_pairs.tsv').write_text('vertex\tfragment\n')
                paired = regress.chain_key(tree, g, 'tools')[2]
            self.assertNotEqual(original, changed)
            self.assertNotEqual(changed, paired)

    def test_vk_chain_key_tracks_selected_converter_content(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); tools = root / 'dm/gfx/tools'; tools.mkdir(parents=True)
            bundled = tools / 'spirv_cross_bin/spirv-cross'; bundled.parent.mkdir()
            bundled.write_bytes(b'bundled converter')
            selected = root / 'override'; selected.write_bytes(b'converter A'); selected.chmod(0o755)
            env = dict(regress.os.environ, SPIRV_CROSS=str(selected))
            g = NS(kind='vk', ver='v0', env=lambda tree: env, shader_roots=list, build=root)
            tree = NS(path=root)
            before = regress.chain_key(tree, g, 'tools')[2]
            selected.write_bytes(b'converter B')
            self.assertNotEqual(before, regress.chain_key(tree, g, 'tools')[2])

    def test_baseline_context_tracks_route_save_artifacts_and_settings(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); save = root / 'save'; save.mkdir()
            (save / 'data').write_bytes(b'original')
            prof = root / 'profile'; prof.mkdir(); (prof / 'game.tsv').write_text('depth_mode\tgl\n')
            art = {'host_key': 'host', 'modules_key': 'modules', 'manifest': {'key': 'chain'}, 'profile': prof}
            a = NS(env=[], tools_fp='tools', no_hud=False, runs=1, prebuild_limit=100, prebuild_hang=10)
            route = {'save': 'run', 'keys': '', 'secs': 90, 'quick': 60, 'shots': [40]}
            with patch.object(regress, 'save_source', return_value=save):
                original = regress.smoke_context(NS(), art, route, a, {})
                self.assertNotEqual(original, regress.smoke_context(NS(), art, dict(route, shots=[45]), a, {}))
                self.assertNotEqual(original, regress.smoke_context(NS(), dict(art, host_key='other'), route, a, {}))
                a.env = ['DM_RENDER_SCALE=0.5']
                self.assertNotEqual(original, regress.smoke_context(NS(), art, route, a, {}))
                a.env = []; (save / 'data').write_bytes(b'changed')
                self.assertNotEqual(original, regress.smoke_context(NS(), art, route, a, {}))

    def test_stored_baseline_is_reused_only_with_matching_context(self):
        with tempfile.TemporaryDirectory() as td, ExitStack() as stack:
            root = Path(td); tree = NS(id='tree')
            route = {'reach': None, 'shots': []}
            g = NS(id='game', name='game', folder=root, route=lambda: route)
            a = NS(baseline='base', candidate='cand', no_smoke=False, rewrite=False, quick=True, runs=1)
            art = {'manifest': {'kind': 'nvn', 'shaders': {}, 'failed': 0}}
            good = {'exit': None, 'max_frame': 500, 'shots': {}}
            stack.enter_context(patch.object(regress, 'ROOT', root))
            stack.enter_context(patch.object(regress, 'Tree', return_value=tree))
            stack.enter_context(patch.object(regress, 'select', return_value=[g]))
            stack.enter_context(patch.object(regress, 'rewriter_key', return_value='same'))
            stack.enter_context(patch.object(regress, 'artifacts', return_value=art))
            stack.enter_context(patch.object(regress, 'new_report', side_effect=lambda *args, **kwargs: {'games': {}, 'machine': {}}))
            stack.enter_context(patch.object(regress, 'finish', return_value=0))
            stack.enter_context(patch.object(regress, 'say'))
            context = stack.enter_context(patch.object(regress, 'smoke_context', return_value={'settings': 'a'}))
            smoke = stack.enter_context(patch.object(regress, 'smoke_series', return_value=[good]))
            regress.cmd_check(a)
            self.assertEqual(smoke.call_count, 2)   # baseline and candidate
            smoke.reset_mock(); regress.cmd_check(a)
            self.assertEqual(smoke.call_count, 1)   # candidate only
            smoke.reset_mock(); context.return_value = {'settings': 'b'}; regress.cmd_check(a)
            self.assertEqual(smoke.call_count, 2)   # changed settings record a fresh baseline

    def test_profiles_and_baselines_stay_in_the_game_work_folder(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); folder = root / 'nsp'; folder.mkdir()
            build = folder / ('0' * 16 + '_build'); build.mkdir()
            tree = NS(path=root / 'tree', id='tree')
            source = tree.path / 'dm/games' / ('0' * 16); source.mkdir(parents=True)
            (source / 'game.tsv').write_text('depth_mode\tgl\n')
            g = regress.Game(folder, build)
            prof = g.profile_dir(tree)
            self.assertTrue(prof.is_relative_to(folder / f'{g.id}-work'))
            self.assertTrue(regress.baseline_dir(tree, g).is_relative_to(folder / f'{g.id}-work'))
            (prof / 'c1_full.tsv').write_text('generated\n')
            self.assertFalse((source / 'c1_full.tsv').exists())
            (source / 'game.tsv').write_text('depth_mode\tzero\n')
            self.assertNotEqual(g.profile_dir(tree), prof)
            env = g.env(tree)
            self.assertEqual(env['DM_GAMES_DIR'], str(g.profile_dir(tree).parent))
            self.assertEqual(env['DM_GAMES_FIRST'], '1')

    def test_vk_roots_include_runtime_capture(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); build = root / ('0' * 16 + '_build')
            romfs = build / 'extract/v0/RomFS'; romfs.mkdir(parents=True)
            (build / 'patched_v0').mkdir()
            capture = romfs.parent / 'vk_capture'; capture.mkdir()
            g = regress.Game(root, build)
            self.assertEqual(g.shader_roots(), [romfs, capture])

    def test_rewriter_uses_original_registered_profile_and_keeps_provisional(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for derived in (False, True):
                with self.subTest(derived=derived):
                    env = {'DM_GAME': 'game', 'DM_GAMES_DIR': '/copied/profile', 'DM_GAMES_FIRST': '1'}
                    g = NS(id='game', folder=root / str(derived), build=root / 'build', ver='v0',
                           patched=root / 'patched_v0', derived=derived, env=lambda tree, config=env: dict(config))
                    tree = NS(id='tree', path=root, py=lambda code, e: 'main')
                    process = NS(poll=lambda: 0, returncode=0)
                    with patch.object(regress, 'rewriter_key', return_value='key'), patch.object(regress, 'say'), \
                         patch.object(regress.subprocess, 'Popen', return_value=process) as spawn, patch.object(regress.time, 'sleep'):
                        dst, _ = regress.modules_for(tree, g, True, 1)
                    selected = spawn.call_args.kwargs['env']
                    self.assertEqual(selected.get('DM_GAMES_DIR'), env['DM_GAMES_DIR'] if derived else None)
                    self.assertEqual(selected.get('DM_GAMES_FIRST'), '1' if derived else None)
                    self.assertEqual(selected['DM_GAME'], 'game')
                    self.assertTrue((dst / '.done').is_file())

    def test_vk_build_records_failure_even_without_generated_shaders(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); tool = root / 'dm/gfx/tools/vk_shaders.py'; tool.parent.mkdir(parents=True)
            tool.write_text('raise SystemExit(1)\n')
            g = NS(id='game', folder=root, build=root, kind='vk', env=lambda tree: dict(regress.os.environ), shader_roots=list)
            with patch.object(regress, 'chain_key', return_value=('version', 'inputs', 'key')):
                manifest, _ = regress.chain_build(NS(id='tree', path=root), g, NS(tools_fp='', jobs=1, pause_lock=None))
            self.assertTrue(manifest['build_failed'])
            self.assertEqual(manifest['shaders'], {})

    def test_vk_failure_without_msl_fails_the_check(self):
        with tempfile.TemporaryDirectory() as td, ExitStack() as stack:
            root = Path(td)
            tree = NS(id='tree'); g = NS(id='game', name='game')
            a = NS(baseline='base', candidate='cand', no_smoke=True, rewrite=False, quick=False)
            stack.enter_context(patch.object(regress, 'Tree', return_value=tree))
            stack.enter_context(patch.object(regress, 'select', return_value=[g]))
            stack.enter_context(patch.object(regress, 'rewriter_key', return_value='same'))
            baseline = {'kind': 'vk', 'shaders': {'key': {'st': 'ok'}}, 'failed': 0}
            candidate = {'kind': 'vk', 'shaders': {}, 'failed': 0, 'build_failed': True}
            stack.enter_context(patch.object(regress, 'chain_build', side_effect=[(baseline, root), (candidate, root)]))
            stack.enter_context(patch.object(regress, 'new_report', return_value={'games': {}}))
            finish = stack.enter_context(patch.object(regress, 'finish', return_value=1))
            stack.enter_context(patch.object(regress, 'say'))
            regress.cmd_check(a)
            self.assertEqual(finish.call_args.args[0]['games']['game']['status'], 'NEW-FAIL')


class Validated(unittest.TestCase):
    """--record: dm/games/<id>/validated.tsv, the chain a game was last green on"""
    def run_record(self, td, status, record=True, dirty=False, mode='quick', smoke=True, chain='aaaa11112222', runs=None, command='check', derived=False):
        games = Path(td) / 'dm/games'; (games / 'g1').mkdir(parents=True, exist_ok=True)
        head = Path(td) / 'dm/gfx/nvn'; head.mkdir(parents=True, exist_ok=True)
        (head / 'dmsh_chain.h').write_text(f'#define DMSH_CHAIN "{chain}"\n')
        route = {'reach': (120, 5), 'shots': [4]}
        green = {'exit': None, 'max_frame': 180, 'reached': True, 'secs': 6, 'shots': {'4': {'dhash': '0123456789abcdef', 'rgb16': '00' * 768}}}
        label = 'candidate' if command == 'check' else 'baseline'
        gr = {'status': status, **({'smoke': {'route': route, label: [green] if runs is None else runs}} if smoke else {})}
        rep = {'command': command, 'mode': mode, 'machine': {'model': 'Mac14,6', 'macos': '26.1'}, 'games': {'g1': gr}}
        tree = NS(commit='0123456789abcdef', dirty=dirty, path=Path(td))
        with patch.object(regress, 'REPO', Path(td)), patch.object(regress, 'say'):
            regress.record_validated(NS(record=record), rep, tree, [NS(id='g1', derived=derived)])
        return games / 'g1/validated.tsv'

    def test_green_writes_the_file(self):
        for status in ('PASS', 'CHANGED'):
            with self.subTest(status), tempfile.TemporaryDirectory() as td:
                f = self.run_record(td, status)
                self.assertTrue(f.read_text().startswith('# '))
                row = regress.read_validated(f)['quick']
                self.assertEqual((row['chain'], row['commit'], row['machine'], row['macos']), ('aaaa11112222', '0123456789', 'Mac14,6', '26.1'))
                self.assertRegex(row['date'], r'^\d{4}-\d\d-\d\d$')

    def test_modes_keep_one_row_each(self):
        with tempfile.TemporaryDirectory() as td:
            self.run_record(td, 'PASS', mode='quick', chain='aaaa11112222')
            f = self.run_record(td, 'PASS', mode='full', chain='bbbb33334444')
            self.run_record(td, 'PASS', mode='quick', chain='cccc55556666')
            rows = regress.read_validated(f)
            self.assertEqual((rows['quick']['chain'], rows['full']['chain']), ('cccc55556666', 'bbbb33334444'))
            self.assertEqual(sum(1 for x in f.read_text().splitlines() if not x.startswith('#')), 2)

    def test_red_leaves_the_record(self):
        for status in ('NEW-FAIL', 'REGRESSION', 'ERROR'):
            with self.subTest(status), tempfile.TemporaryDirectory() as td:
                f = self.run_record(td, 'PASS'); before = f.read_text()
                self.run_record(td, status, chain='bbbb33334444')
                self.assertEqual(f.read_text(), before)

    def test_without_record_no_file(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertFalse(self.run_record(td, 'PASS', record=False).exists())

    def test_no_boot_run_no_file(self):   # a chain-only result says nothing about the game's boot
        with tempfile.TemporaryDirectory() as td:
            self.assertFalse(self.run_record(td, 'PASS', smoke=False).exists())
            self.assertFalse(self.run_record(td, 'PASS', mode='static').exists())

    def test_known_boot_failure_keeps_last_green(self):
        route = {'reach': (120, 5), 'shots': [4]}
        green = {'exit': None, 'max_frame': 180, 'reached': True, 'secs': 6, 'shots': {'4': {'dhash': '0123456789abcdef', 'rgb16': '00' * 768}}}
        for failure in ({'crash': {'signal': 'SIGSEGV'}}, {'exit': 1}, {'hang_at_s': 3}, {'max_frame': -1},
                        {'reached': False}, {'prebuild': {'exit': 1}}, {'shots': {}},
                        {'pipeline': {'depth_profile': 'gl', 'depth_runtime': ['zero']}}, {'pipeline': {'stamp_mismatch': True}}):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as td:
                failed = green | failure
                levels, known, _ = regress.smoke_diff([failed], [failed], route)
                self.assertFalse(any(levels.values()))   # unchanged broken boots are PASS in a regression check
                self.assertTrue(known)
                f = self.run_record(td, 'PASS'); before = f.read_text()
                self.run_record(td, 'PASS', chain='bbbb33334444', runs=[failed])
                self.assertEqual(f.read_text(), before)

    def test_empty_or_partially_failed_runs_do_not_record(self):
        for runs in ([], [{'max_frame': 1}, {'max_frame': -1}]):
            with self.subTest(runs=runs), tempfile.TemporaryDirectory() as td:
                self.assertFalse(self.run_record(td, 'PASS', runs=runs).exists())

    def test_baseline_success_can_record(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertTrue(self.run_record(td, 'PASS', command='baseline').is_file())

    def test_derived_profile_does_not_validate_repository_profile(self):
        with tempfile.TemporaryDirectory() as td:
            f = self.run_record(td, 'PASS'); before = f.read_text()
            self.run_record(td, 'PASS', chain='bbbb33334444', derived=True)
            self.assertEqual(f.read_text(), before)

    def test_record_error_keeps_completed_report(self):
        with tempfile.TemporaryDirectory() as td, ExitStack() as stack:
            root = Path(td)
            self.run_record(td, 'PASS')
            tree = NS(id='tree', path=root, commit='0123456789abcdef', dirty=False,
                      describe=lambda: {'rev': 'test', 'sha': '0123456789abcdef'})
            route = {'reach': None, 'shots': []}
            game = NS(id='g1', name='game', folder=root / 'nsp', derived=False, route=lambda: route)
            manifest = {'key': 'key', 'kind': 'nvn', 'failed': 0, 'total': 1, 'ok': 1}
            a = NS(rev='test', no_smoke=False, quick=True, rewrite=False, runs=1, record=True, out=str(root / 'report'))
            stack.enter_context(patch.object(regress, 'REPO', root))
            stack.enter_context(patch.object(regress, 'Tree', return_value=tree))
            stack.enter_context(patch.object(regress, 'select', return_value=[game]))
            stack.enter_context(patch.object(regress, 'artifacts', return_value={'manifest': manifest}))
            stack.enter_context(patch.object(regress, 'smoke_series', return_value=[{'exit': None, 'max_frame': 180}]))
            stack.enter_context(patch.object(regress, 'smoke_context', return_value={}))
            stack.enter_context(patch.object(regress, 'machine', return_value={'model': 'test', 'chip': 'test', 'macos': 'test', 'xcode': 'test'}))
            stack.enter_context(patch.object(regress, 'say'))
            write_text = Path.write_text
            def fail_record(path, text, *args, **kwargs):
                if path.name == 'validated.tsv': raise PermissionError('read-only profile')
                return write_text(path, text, *args, **kwargs)
            stack.enter_context(patch.object(Path, 'write_text', fail_record))
            with self.assertRaisesRegex(regress.HarnessError, 'regression report was saved'):
                regress.cmd_baseline(a)
            self.assertEqual(json.loads((root / 'report/report.json').read_text())['status'], 'PASS')
            self.assertIn('game `g1`: PASS', (root / 'report/report.md').read_text())

    def test_validation_metadata_does_not_change_profile_inputs(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            prof = root / 'dm/games/g1'; prof.mkdir(parents=True)
            (prof / 'game.tsv').write_text('name\tgame\n')
            g = NS(id='g1', folder=root / 'nsp', derived=False)
            first = regress.Game.profile_dir(g, NS(path=root))
            (prof / 'validated.tsv').write_text('quick\tchain=old\n')
            self.assertEqual(regress.Game.profile_dir(g, NS(path=root)), first)
            self.assertFalse((first / 'validated.tsv').exists())

    def test_dirty_tree(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(regress.read_validated(self.run_record(td, 'PASS', dirty=True))['quick']['commit'], '0123456789+dirty')

    def test_list_shows_current_and_old(self):
        with tempfile.TemporaryDirectory() as td:
            self.run_record(td, 'PASS', chain='aaaa11112222')
            with patch.object(regress, 'REPO', Path(td)):
                self.assertIn('quick chain aaaa11112222 (current)', regress.validated_text('g1'))
                self.assertEqual(regress.validated_text('nobody'), 'never recorded')
                (Path(td) / 'dm/gfx/nvn/dmsh_chain.h').write_text('#define DMSH_CHAIN "dddd77778888"\n')
                self.assertIn('(old, now dddd77778888)', regress.validated_text('g1'))
                g = NS(id='g1', name='n', kind='nvn', ver='1', folder=Path(td), route=lambda: None)
                with patch.object(regress, 'find_games', return_value=[g]), patch.object(regress, 'say') as say:
                    regress.cmd_list(NS())
                self.assertIn('green: quick chain aaaa11112222 (old', say.call_args.args[0])

    def test_profiles_validated_files_read(self):
        for f in (REPO / 'dm/games').glob('*/validated.tsv'):
            for mode, row in regress.read_validated(f).items():
                self.assertIn(mode, ('quick', 'full'), f)
                self.assertTrue(row.get('chain') and row.get('commit') and row.get('date'), f)


if __name__ == '__main__':
    unittest.main()
