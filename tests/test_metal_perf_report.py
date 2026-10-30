"""games/totk/scripts/metal_perf_report.py on synthetic metalperftrace output (the layout of macOS 27.2's `overview
--json-include-timeline --include-state-transitions` and `overview --aggregate --json`, written by hand: a real trace
holds every process that drew on the Mac, so none is in the repository). metalperftrace itself is replaced; a smoke test
that collects a real trace runs only with MNG_METAL_PERF_SMOKE=1."""
import importlib.util
import json
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('metal_perf_report', ROOT/'games/totk/scripts/metal_perf_report.py')
mpr = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mpr)
L, G = mpr.LIFECYCLE, mpr.GRAPHICS
UUID = '00112233-4455-6677-8899-AABBCCDDEEFF'


def st(avg, mx=None, n=100, sd=1.0):
    return {'Count': n, 'Min (ms)': avg / 2, 'Average (ms)': avg, 'Max (ms)': mx or avg * 2, 'StdDev (ms)': sd, 'Total (ms)': avg * n}


def perf(fps, frames, frame_ms=8.33, gpu=5.0, cpu=2.0, wait=0.01, active=10.0, shader=None):
    shown = {'FPS': fps, 'Frame Count': frames, 'End-to-end Walltime Stats (CPU)': st(cpu, n=frames),
             'On-GPU Walltime Stats': st(gpu, n=frames), 'Next Drawable Wait Walltime Stats': st(wait, n=frames),
             'Command Buffer Count': frames * 20}
    if shader:
        shown['Shader Compiler'] = shader
    return {'Presented Frame Stats': shown, 'Skipped Frame Stats': {'Frame Count': 3},
            'Frame-On-Glass Interval Stats': st(frame_ms, n=frames), 'On-Glass Present Lateness Stats': st(4.0, n=5),
            'Time Active (sec)': active}


T = '2026-10-26T03:00:%02d.000Z'


def counters(t, frames, generated=0, merged=0, upscaled=0):
    return {'gameFrames': frames, 'generatedFrames': generated, 'mergedFrames': merged, 'upscaledFrames': upscaled,
            'antiAliasedFrames': 0, 'paceTicks1': frames, 'paceTicks2': 0, 'paceTicks3': 0, 'shaderTranslations': 3,
            'librariesCreated': 2, 'pipelinesCreated': 5, 'creationMs': 12, 'footprintMiB': 3000, 'metalAllocatedMiB': 1800,
            'scene': '1920x1080', 'sampledAt': T % t}


IDENT = {'game': 'totk', 'gameVersion': '1.4.3', 'shaderChain': 'totk-native-v8-unverified', 'sourceCommit': 'abc1234',
         'runtimeUUID': UUID, 'settings': 'TOTK_UNCAPPED_HZ=120', 'hostModel': 'Mac17,9', 'hostChip': 'Apple M5 Pro',
         'hostOS': 'Version 27.2 (Build 26B118)'}
FI = '1920x1080 | FI 50+50 | held 50 | window@120Hz'
PLAIN = '1920x1080 | cap 120 | window@120Hz'


def overview(drawables=None, pid=4242, extra_states=()):
    """A 40 s game run: starting 0-2, shaderWarmup 2-5, focused 5-25, unfocused 25-30, focused 30-40 (open at the end);
    Graphics plain 5-20, then 50+50 interpolation from 20 (open). Game presents: 100/s until 20, then 50/s."""
    def frames_at(t):
        return 0 if t < 5 else 100 * (min(t, 20) - 5) + 50 * max(0, t - 20)
    life = [('starting', 0, 2, IDENT), ('shaderWarmup (archive rebuilt)', 2, 5, IDENT), ('focused', 5, 25, IDENT),
            ('unfocused', 25, 30, IDENT), ('focused', 30, None, IDENT)]
    gfx = [(PLAIN, 5, 20, {'interpolation': 'off'}), (FI, 20, None, {'interpolation': '50+50'})]
    def states(rows):
        return [{'State Label': lab, 'Start Date': T % a, 'End Date': T % b if b is not None else 'Unknown', 'Stable States': s,
                 'Volatile States': counters(a, frames_at(a), 50 * max(0, a - 20), upscaled=frames_at(a) // 2)} for lab, a, b, s in rows]
    layer = {'Total Session Stats': {**perf(90.0, 3600), 'End Date': T % 40}, 'Layer ID': 1,
             'Configuration Timeline': [{'Width (pixels)': 1920, 'Height (pixels)': 1080, 'MTL Pixel Format Name': 'BGRA8Unorm'}],
             'Stats Timeline': [{**perf(60 + i, 60 + i, active=1.0), 'Start Date': T % i, 'End Date': T % (i + 1), 'Total Duration (sec)': 1.0}
                                for i in range(40)]}
    if drawables:
        layer['On-Glass Drawable Timeline'] = drawables
    lstates, gstates = states(life), states(gfx)
    for domain, s in extra_states:
        (lstates if domain == L else gstates).append(s)
    game = {'Process': 'totk-native', 'PID': pid, 'Start Date': T % 0, 'End Date': T % 39, 'Layers': [layer],
            'States': {L: {'States': lstates}, G: {'States': gstates}},
            'Resource Usage': {'Physical Footprint (MiB)': 3100.5, 'Peak Physical Footprint (MiB)': 3200.0,
                               'Metal Device Allocated Size (MiB)': 1850.0, 'CPU User Time (Seconds)': 100.0,
                               'CPU Sys Time (Seconds)': 50.0, 'Date': T % 39}}
    other = {'Process': 'OtherApp', 'PID': 1, 'Start Date': T % 0, 'End Date': T % 39, 'States': {},
             'Layers': [{'Total Session Stats': perf(60, 60), 'Configuration': {'Name': '/Users/someone/secret/path'}}]}
    return [other, game]


def aggregate(pid=4242):
    def group(domain, label, fps, frames, seconds):
        return {'Domain': domain, 'State Label': label, 'Total Duration (sec)': seconds, 'Residency (%)': seconds / 40 * 100,
                'Segments': 1, 'Layers': [{'Performance Stats': perf(fps, frames, active=seconds)}]}
    return [{'Process': 'totk-native', 'PID': pid, 'Skipped Sub-Second Transitions': 0,
             'Groups': [group(L, 'starting', 1.0, 2, 2), group(L, 'shaderWarmup (archive rebuilt)', 0.5, 1, 3),
                        group(L, 'focused', 99.0, 2970, 30), group(L, 'unfocused', 50.0, 250, 5),
                        group(G, PLAIN, 100.0, 1500, 15), group(G, FI, 100.0, 2000, 20)], 'Skipped States': []}]


def drawable(t_ms, on_glass_ms, gpu_ms=5.0, interp=False):
    sec = t_ms / 1000
    return {'Start Date': f'2026-10-26T03:00:{int(sec):02d}.{int(t_ms % 1000):03d}Z', 'Duration MCT Seconds': on_glass_ms / 1000,
            'In-Flight Drawable Info': {'On-GPU Time (seconds)': gpu_ms / 1000, 'Wait for Next Drawable Walltime Interval (seconds)': 0.0001,
                                        'CPU End-to-End Walltime Interval': {'Duration MCT Seconds': 0.002},
                                        'MetalFX Frame Interpolator Enabled': interp, 'MetalFX Enabled': interp}}


def rows_of(report):
    return {(r['domain'], r['label']): r for r in report['sessions'][0]['by_state']}


class Report(unittest.TestCase):
    def test_whole_run_and_states(self):
        r = mpr.build_report(overview(), aggregate())
        s, = r['sessions']
        self.assertEqual(s['game']['id'], 'totk')
        self.assertEqual(s['game']['host'], dict(model='Mac17,9', chip='Apple M5 Pro', os='Version 27.2 (Build 26B118)'))
        self.assertEqual(s['overall']['output_fps'], 90.0)
        self.assertEqual(s['memory']['peak_footprint_mib'], 3200.0)
        self.assertEqual(len(s['timeline']), 40)
        last = s['states'][-1]
        self.assertTrue(last['end_inferred'])
        self.assertEqual(last['end'], '2026-10-26T03:00:40.000Z')   # the layer's end, not the process entry's earlier one
        self.assertFalse(any(x['began_before_trace'] for x in s['states']))

    def test_three_frame_rates_per_state(self):
        rows = rows_of(mpr.build_report(overview(), aggregate()))
        plain, fi = rows[(G, PLAIN)], rows[(G, FI)]
        self.assertEqual((plain['game']['game_fps'], plain['game']['covers_s']), (100.0, 15.0))
        self.assertEqual(plain['apple_aggregate']['output_fps'], 100.0)
        self.assertEqual(plain['game']['metalfx_upscaled_pct'], 50.0)
        self.assertEqual(plain['game']['pace_ticks_pct'], {'1': 100.0, '2': 0.0, '3+': 0.0})
        # 20-30 s: the last reading inside the open FI state is the unfocused/focused change at 30 s
        self.assertEqual((fi['game']['game_fps'], fi['game']['generated_fps'], fi['game']['covers_s']), (50.0, 50.0, 10.0))
        self.assertEqual(fi['apple_aggregate']['output_fps'], 100.0)
        self.assertIn('generated ones included', fi['note'])
        fg = rows[(L, 'focused')]
        self.assertEqual((fg['segments'], fg['game']['covers_s']), (2, 20.0))   # 30-40 s has no reading after its start

    def test_visible_play_uses_whole_seconds_while_the_picture_shows(self):
        s = mpr.build_report(overview(), aggregate())['sessions'][0]
        v = s['visible_play']
        self.assertEqual(v['buckets'], 35)   # 5-40 s: focused, unfocused, focused
        self.assertEqual(v['drawables_shown'], sum(60 + i for i in range(5, 40)))
        rows = rows_of(mpr.build_report(overview(), aggregate()))
        self.assertEqual(rows[(G, PLAIN)]['visible']['buckets'], 15)
        self.assertEqual(rows[(G, PLAIN)]['visible']['frame_time_ms']['avg'], 8.33)
        self.assertIsNone(rows[(L, 'starting')].get('visible'))

    def test_pooled_statistics(self):
        p = mpr.pooled([st(10.0, n=10, sd=0.0), st(20.0, n=10, sd=0.0)])
        self.assertEqual((p['count'], p['avg'], p['min'], p['max']), (20, 15.0, 5.0, 40.0))
        self.assertAlmostEqual(p['stddev'], (10 * 25 * 2 / 19) ** 0.5, places=3)

    def test_heartbeat_repeated_state(self):
        # the system repeats a state with its original reading (sampledAt 10 s before the trace saw it)
        late = {'State Label': 'tick', 'Start Date': T % 33, 'End Date': 'Unknown', 'Stable States': {}, 'Volatile States': counters(23, 1)}
        r = mpr.build_report(overview(extra_states=[(G, late)]), aggregate())
        s = r['sessions'][0]
        self.assertTrue(next(x for x in s['states'] if x['label'] == 'tick')['began_before_trace'])
        row = rows_of(r)[(G, 'tick')]
        self.assertTrue(row['began_before_trace'])
        self.assertNotIn('game', row)   # its reading is from before it was seen: no rate from it
        self.assertTrue(any('heartbeat' in n for n in s['notes']))

    def test_percentiles_only_from_per_frame_data(self):
        s = mpr.build_report(overview(), aggregate())['sessions'][0]
        self.assertFalse(s['percentiles']['available'])
        self.assertIn('METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1', s['percentiles']['reason'])
        # 100 drawables at 6.0-6.9 s with on-glass 1..100 ms; 10 interpolated at 21 s
        frames = [drawable(6000 + 9 * i, i + 1) for i in range(100)] + [drawable(21000 + 9 * i, 10, interp=True) for i in range(10)]
        r = mpr.build_report(overview(frames), aggregate())
        s = r['sessions'][0]
        # 110 values: 1..9, eleven 10s, 11..100; nearest rank: the ceil(p% x 110)-th smallest
        self.assertEqual(s['percentiles']['frame_time_ms'], dict(p50=45.0, p90=89.0, p95=95.0, p99=99.0))
        self.assertEqual(s['percentiles']['interpolator_drawables'], 10)
        rows = rows_of(r)
        self.assertEqual(rows[(G, PLAIN)]['percentiles']['frame_time_ms'], dict(p50=50.0, p90=90.0, p95=95.0, p99=99.0))
        self.assertEqual(rows[(G, FI)]['percentiles']['drawables'], 10)

    def test_a_state_after_the_last_frame(self):
        late = {'State Label': 'closing', 'Start Date': T % 41, 'End Date': 'Unknown', 'Stable States': IDENT, 'Volatile States': counters(41, 3000)}
        r = mpr.build_report(overview(extra_states=[(L, late)]), aggregate())
        x = next(x for x in r['sessions'][0]['states'] if x['label'] == 'closing')
        self.assertEqual((x['start'], x['end']), (T % 41, T % 41))
        self.assertEqual(rows_of(r)[(L, 'closing')]['seconds'], 0)

    def test_states_too_short_to_aggregate(self):
        brief = {'State Label': 'settings', 'Start Date': '2026-10-26T03:00:26.000Z', 'End Date': '2026-10-26T03:00:26.400Z',
                 'Stable States': IDENT, 'Volatile States': counters(26, 2050)}
        agg = aggregate()
        agg[0]['Skipped States'] = [{'Domain': L, 'State Label': 'settings'}]
        row = rows_of(mpr.build_report(overview(extra_states=[(L, brief)]), agg))[(L, 'settings')]
        self.assertIsNone(row['apple_aggregate'])
        self.assertEqual(row['seconds'], 0.4)
        self.assertIn('shorter than one second', row['not_aggregated_because'])

    def test_a_long_state_without_drawables_is_not_called_short(self):
        # metalperftrace also skips a long state with no frames (the start-up screen during the warm-up) and says no why
        W = 'shaderWarmup (archive rebuilt)'
        agg = aggregate()
        agg[0]['Groups'] = [g for g in agg[0]['Groups'] if g['State Label'] != W]
        agg[0]['Skipped States'] = [{'Domain': L, 'State Label': W}]
        o = overview()
        for bucket in o[1]['Layers'][0]['Stats Timeline'][:5]:
            bucket['Presented Frame Stats']['Frame Count'] = 0
        r = mpr.build_report(o, agg)
        row = rows_of(r)[(L, W)]
        self.assertEqual(row['seconds'], 3)
        self.assertIn('no drawable', row['not_aggregated_because'])
        self.assertTrue(any(u['reason'] == row['not_aggregated_because'] for u in mpr.unavailable(r)))
        # with frames in it, the report does not guess either
        row = rows_of(mpr.build_report(overview(), agg))[(L, W)]
        self.assertEqual(row['not_aggregated_because'], 'metalperftrace skipped it and gives no reason')

    def test_a_run_without_states(self):
        o = overview()
        o[1]['States'] = {}
        s = mpr.build_report(o, [])['sessions'][0]
        self.assertFalse(s['state_reporting'])
        self.assertIsNone(s['game'])
        self.assertEqual(s['by_state'], [])
        self.assertIsNone(s['visible_play'])
        self.assertIn('TOTK_STATE_REPORTING=1', s['notes'][0])

    def test_pid_selects_one_run(self):
        o = overview() + overview(pid=5000)[1:]
        self.assertEqual(len(mpr.build_report(o, [])['sessions']), 2)
        self.assertEqual([s['pid'] for s in mpr.build_report(o, [], pid=5000)['sessions']], [5000])


def fake_tool(outputs, calls=None):
    """metalperftrace replaced: {'overview': (rc, stdout), 'aggregate': ..., 'collect': ...}."""
    def run(args, timeout=900):
        if calls is not None:
            calls.append(args)
        key = 'collect' if args[0] == 'collect' else 'aggregate' if '--aggregate' in args else 'overview'
        if key == 'collect':
            rc, out = outputs['collect']
            target = Path(args[-1])
            (target/'trace_x.atrc').write_bytes(b'trace')
            return rc, out.replace('DIR', str(target)), f'collect failed in {Path.home()}/x' if rc else ''
        rc, out = outputs[key]
        if rc is None:
            return None, '', 'metalperftrace not found: it ships with macOS 27'
        return rc, out, f'boom: {args[-1]}' if rc else ''
    return run


def macho(uuid_bytes):
    """A minimal thin arm64 Mach-O: header and one LC_UUID."""
    cmd = struct.pack('<II', 0x1b, 24) + uuid_bytes
    return struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, 2, 1, len(cmd), 0, 0) + cmd


class Command(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='metal-perf-report-'))
        self.trace = self.tmp/'MetalPerfTrace_x.atrc'
        self.trace.write_bytes(b'raw trace bytes')
        self.out = self.tmp/'report'

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def app(self, uuid=bytes.fromhex(UUID.replace('-', '')), name='Tears of the Kingdom.app'):
        app = self.tmp/name
        (app/'Contents/MacOS').mkdir(parents=True)
        (app/'Contents/Resources/totk/build').mkdir(parents=True)
        (app/'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundleExecutable': 'TOTK', 'CFBundleIdentifier': 'org.macnativegames.totk', 'CFBundleShortVersionString': '1.4.3',
            'MNGSourceCommit': 'abc1234', 'LSEnvironment': {'TOTK_UNCAPPED_HZ': '120', 'TOTK_PROFILE_DIR': str(Path.home()/'x'), 'HOME_X': 'y'}}))
        (app/'Contents/MacOS/TOTK').write_bytes(b'launcher')
        (app/'Contents/Resources/totk/build/totk-native').write_bytes(macho(uuid))
        return app

    def main(self, *args, outputs=None, calls=None):
        outputs = outputs or {'overview': (0, json.dumps(overview())), 'aggregate': (0, json.dumps(aggregate()))}
        with mock.patch.object(mpr, 'tool', fake_tool(outputs, calls)), mock.patch('builtins.print'):
            return mpr.main(list(map(str, args)))

    def read(self, name):
        return json.loads((self.out/name).read_text()) if name.endswith('.json') else (self.out/name).read_text()

    def test_report_writes_the_four_files(self):
        calls = []
        self.assertEqual(self.main('report', self.trace, '--out', self.out, '--app', self.app(), '--note', 'route=plateau', calls=calls), 0)
        self.assertEqual(sorted(p.name for p in self.out.iterdir()), ['metadata.json', 'performance.json', 'performance.md', 'trace.atrc'])
        self.assertEqual((self.out/'trace.atrc').read_bytes(), b'raw trace bytes')
        self.assertTrue(all(c[c.index('--predicate') + 1] == "processName == 'totk-native'" for c in calls))   # only the game is read
        perf, meta = self.read('performance.json'), self.read('metadata.json')
        self.assertEqual(perf['status'], 'ok')
        run, = meta['runs']
        self.assertEqual(run['game'], dict(id='totk', title='Tears of the Kingdom', version='1.4.3'))
        b = run['build']
        self.assertEqual((b['source_commit'], b['runtime_uuid']), ('abc1234', UUID))
        self.assertEqual(b['runtime_check'], 'UUID matches --app Tears of the Kingdom.app')
        self.assertEqual(len(b['runtime_sha256']), 64)
        self.assertEqual(run['host']['model'], 'Mac17,9')
        self.assertEqual(run['settings_in_force'], 'TOTK_UNCAPPED_HZ=120')
        self.assertEqual(run['app_defaults'], {'TOTK_PROFILE_DIR': '<path>', 'TOTK_UNCAPPED_HZ': '120'})
        self.assertEqual(run['first_graphics_state'], PLAIN)
        self.assertEqual(meta['test_config']['notes'], {'route': 'plateau'})
        for k in ('model', 'chip', 'memory_gb', 'product_version'):
            self.assertIn(k, meta['report_host'])
        self.assertEqual(meta['trace']['files'][0]['name'], 'trace.atrc')
        self.assertEqual(meta['app_problems'], [])

    def test_nothing_of_other_processes_or_user_paths(self):
        self.main('report', self.trace, '--out', self.out, '--app', self.app())
        text = ''.join((self.out/n).read_text() for n in ('metadata.json', 'performance.json', 'performance.md'))
        for secret in ('OtherApp', '/Users/someone', str(Path.home()), str(self.tmp)):
            self.assertNotIn(secret, text)
        self.assertIn('every process that drew', self.read('performance.md'))

    def test_an_app_that_did_not_run(self):
        self.main('report', self.trace, '--out', self.out, '--app', self.app(uuid=bytes(16), name='Other.app'))
        meta = self.read('metadata.json')
        self.assertEqual(meta['runs'][0]['build']['runtime_check'], 'unverified: no --app with this runtime UUID')
        self.assertNotIn('runtime_sha256', meta['runs'][0]['build'])
        self.assertTrue(any('not the build that ran' in p for p in meta['app_problems']))

    def test_an_app_with_state_reports_off(self):
        # no run reported a UUID: the report cannot tell whether --app ran, and does not say it did not
        o = overview()
        o[1]['States'] = {}
        self.main('report', self.trace, '--out', self.out, '--app', self.app(),
                  outputs={'overview': (0, json.dumps(o)), 'aggregate': (0, json.dumps(aggregate()))})
        problems = self.read('metadata.json')['app_problems']
        self.assertTrue(any('is not known' in p for p in problems), problems)
        self.assertFalse(any('not the build that ran' in p for p in problems), problems)

    def test_markdown(self):
        late = {'State Label': 'tick', 'Start Date': T % 33, 'End Date': 'Unknown', 'Stable States': {}, 'Volatile States': counters(23, 1)}
        self.main('report', self.trace, '--out', self.out, outputs={'overview': (0, json.dumps(overview(extra_states=[(G, late)]))),
                                                                     'aggregate': (0, json.dumps(aggregate()))})
        md = self.read('performance.md')
        self.assertIn('Frame time p50 / p95 / p99 | not available', md)
        self.assertIn('| Graphics: 1920x1080 \\| cap 120 \\| window@120Hz | 15.0 | 100.0 / ', md)   # | escaped inside a cell
        self.assertIn('| Lifecycle: focused | 30.0 | 99.0 / ', md)
        self.assertIn('runtime unverified: no --app with this runtime UUID', md)
        self.assertIn('never added or subtracted', md)
        perf = self.read('performance.json')
        self.assertTrue(any('p95' in u['metric'] for u in perf['unavailable']))
        self.assertIn(dict(metric='run 1: game frame rate of 1 state(s): Graphics tick', reason=mock.ANY), perf['unavailable'])
        self.assertIn('Graphics: tick (began before the trace)', md)

    def test_metalperftrace_missing(self):
        rc = self.main('report', self.trace, '--out', self.out, outputs={'overview': (None, ''), 'aggregate': (None, '')})
        self.assertEqual(rc, mpr.EXIT_TOOL)
        perf = self.read('performance.json')
        self.assertEqual(perf['status'], 'failed')
        self.assertIn('macOS 27', perf['errors'][0]['message'])
        self.assertIn('report_host', self.read('metadata.json'))
        self.assertIn('**metalperftrace overview (overview) trace.atrc**', self.read('performance.md'))

    def test_metalperftrace_fails_or_prints_no_json(self):
        rc = self.main('report', self.trace, '--out', self.out, outputs={'overview': (0, 'not json'), 'aggregate': (1, '')})
        self.assertEqual(rc, mpr.EXIT_TOOL)
        errors = self.read('performance.json')['errors']
        self.assertTrue(any('not JSON' in e['message'] for e in errors))
        self.assertTrue(any(e['message'] == 'boom: <trace>' for e in errors))   # the trace's path scrubbed

    def test_aggregate_fails_overview_works(self):
        rc = self.main('report', self.trace, '--out', self.out, outputs={'overview': (0, json.dumps(overview())), 'aggregate': (1, '')})
        self.assertEqual(rc, mpr.EXIT_TOOL)
        perf = self.read('performance.json')
        self.assertEqual(perf['status'], 'partial')
        self.assertEqual(perf['sessions'][0]['overall']['output_fps'], 90.0)
        self.assertFalse(any(r['apple_aggregate'] for r in perf['sessions'][0]['by_state']))
        self.assertTrue(any(r.get('game') for r in perf['sessions'][0]['by_state']))

    def test_no_game_in_the_trace(self):
        rc = self.main('report', self.trace, '--out', self.out, outputs={'overview': (0, json.dumps(overview()[:1])), 'aggregate': (0, '[]')})
        self.assertEqual(rc, mpr.EXIT_NO_GAME)
        self.assertEqual(self.read('performance.json')['status'], 'no game process')
        self.assertIn('No `totk-native` process is in the trace.', self.read('performance.md'))

    def test_collect(self):
        calls = []
        outputs = {'collect': (0, json.dumps({'traces': ['DIR/trace_x.atrc']})), 'overview': (0, json.dumps(overview())),
                   'aggregate': (0, json.dumps(aggregate()))}
        self.assertEqual(self.main('collect', '--last', '90s', '--out', self.out, '--pid', 4242, outputs=outputs, calls=calls), 0)
        self.assertEqual(calls[0][:3], ['collect', '--last', '90s'])
        self.assertTrue(all(c[c.index('--predicate') + 1] == 'pid == 4242' for c in calls[1:]))
        self.assertEqual((self.out/'trace.atrc').read_bytes(), b'trace')
        self.assertTrue(self.read('metadata.json')['trace']['collected_by_this_command'])
        self.assertEqual([p.name for p in self.out.iterdir() if p.name.startswith('.collect')], [])

    def test_collect_fails(self):
        rc = self.main('collect', '--out', self.out, outputs={'collect': (1, '')})
        self.assertEqual(rc, mpr.EXIT_TOOL)
        perf = self.read('performance.json')
        self.assertEqual((perf['status'], perf['errors'][0]['message']), ('failed', 'collect failed in ~/x'))
        self.assertTrue(self.read('metadata.json')['trace']['collected_by_this_command'])

    def test_usage(self):
        with self.assertRaises(SystemExit) as e, mock.patch('sys.stderr'):
            mpr.main(['collect', '--out', str(self.out), '--end', '2026-10-26'])
        self.assertEqual(e.exception.code, 2)
        with self.assertRaises(SystemExit), mock.patch('sys.stderr'):
            mpr.main(['report', str(self.tmp/'missing.atrc'), '--out', str(self.out)])
        with self.assertRaises(SystemExit), mock.patch('sys.stderr'):
            mpr.main(['report', str(self.trace), '--out', str(self.out), '--note', 'no-equals-sign'])

    def test_macho_uuid(self):
        p = self.tmp/'bin'
        p.write_bytes(macho(bytes.fromhex('00112233445566778899aabbccddeeff')))
        self.assertEqual(mpr.macho_uuid(p), UUID)
        p.write_bytes(b'#!/bin/sh\n')
        self.assertIsNone(mpr.macho_uuid(p))


@unittest.skipUnless(os.environ.get('MNG_METAL_PERF_SMOKE') == '1' and shutil.which('metalperftrace'),
                     'collects a real system trace: run with MNG_METAL_PERF_SMOKE=1 on macOS 27')
class RealMetalPerfTrace(unittest.TestCase):
    def test_collect_the_last_seconds(self):
        with tempfile.TemporaryDirectory(prefix='metal-perf-report-') as tmp:
            r = subprocess.run([sys.executable, str(ROOT/'games/totk/scripts/metal_perf_report.py'), 'collect', '--last', '30s',
                                '--out', tmp], capture_output=True, text=True, timeout=600)
            self.assertIn(r.returncode, (0, mpr.EXIT_NO_GAME), r.stdout + r.stderr)
            names = {p.name for p in Path(tmp).iterdir()}
            self.assertLessEqual({'metadata.json', 'performance.json', 'performance.md', 'trace.atrc'}, names)
            self.assertIn(json.loads((Path(tmp)/'performance.json').read_text())['status'], ('ok', 'no game process'))


if __name__ == '__main__':
    unittest.main()
