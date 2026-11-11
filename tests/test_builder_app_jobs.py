"""The drag-and-drop builder's job layer (tools/builder_jobs): what a dropped file is, the plan, the progress record and a
whole build run against a stand-in build_app.py (events, a failure, a cancel)."""
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import build_job, checks, classify, history, repo  # noqa: E402
from tools.builder_jobs.events import Emitter  # noqa: E402

KEYS = 'header_key = ' + 'ab' * 32 + '\n'


def pfs0(path, size=64):
    """A PFS0 package with no files (no tickets): its game comes from its name."""
    path.write_bytes(b'PFS0' + b'\0' * (size - 4))
    return path


class Classify(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def roles(self, *paths):
        return {Path(i.path).name: (i.role, i.game, i.version) for i in classify.classify(paths)}

    def test_files_by_their_title_ids_and_versions(self):
        pfs0(self.dir / 'TOTK [0100F2C0115B6000][v0].nsp')
        pfs0(self.dir / 'v1.4.3 [0100F2C0115B6800][v720896][UPD].nsp')
        pfs0(self.dir / '1.9.0 01007EF00011E800.nsz')
        (self.dir / 'prod.keys').write_text(KEYS)
        (self.dir / 'title.keys').write_text('x = 00\n')
        (self.dir / '._prod.keys').write_text('resource fork')
        got = self.roles(self.dir)
        self.assertEqual(got['TOTK [0100F2C0115B6000][v0].nsp'], ('base', 'totk', 0))
        self.assertEqual(got['v1.4.3 [0100F2C0115B6800][v720896][UPD].nsp'], ('update', 'totk', 720896))
        self.assertEqual(got['1.9.0 01007EF00011E800.nsz'], ('update', 'botw', 1114112))   # the display version, known
        self.assertEqual(got['prod.keys'][0], 'keys')
        self.assertEqual(got['title.keys'][0], 'ignored')
        self.assertNotIn('._prod.keys', got)

    def test_keys_without_header_key_and_empty_files(self):
        (self.dir / 'prod.keys').write_text('titlekek_00 = ' + '00' * 16 + '\n')
        (self.dir / 'empty [0100F2C0115B6000].xci').write_bytes(b'')
        got = self.roles(self.dir)
        self.assertEqual(got['prod.keys'][0], 'ignored')
        self.assertEqual(got['empty [0100F2C0115B6000].xci'][0], 'ignored')

    def test_a_switch_2_dump_by_its_npdm(self):
        exefs = self.dir / 'dump/updates/458752/exefs'
        exefs.mkdir(parents=True)
        npdm = bytearray(0x200)
        npdm[:4] = b'META'
        npdm[0x70:0x78] = (0x100).to_bytes(4, 'little') + (0x40).to_bytes(4, 'little')
        npdm[0x100:0x104] = b'ACI0'
        npdm[0x110:0x118] = int('040074A01BF12000', 16).to_bytes(8, 'little')
        (exefs / 'main.npdm').write_bytes(bytes(npdm))
        self.assertEqual(self.roles(self.dir / 'dump')['dump'], ('ns2_dump', 'dkb', None))

    def test_plan_picks_the_newest_supported_update_and_needs_keys_and_fonts(self):
        base = pfs0(self.dir / 'TOTK [0100F2C0115B6000][v0].nsp')
        old = pfs0(self.dir / 'TOTK [0100F2C0115B6800][v131072].nsp')     # 1.1.2: removed from the repository
        new = pfs0(self.dir / 'TOTK [0100F2C0115B6800][v393216].nsp')     # 1.2.1
        p = classify.plan(classify.classify([base, old, new]))
        self.assertEqual((p.game, p.base, p.update), ('totk', str(base.resolve()), str(new.resolve())))
        self.assertTrue(any('no keys' in b for b in p.blocks))
        self.assertTrue(any('system fonts' in b for b in p.blocks))
        self.assertTrue(any(old.name in n for n in p.notes))

    def test_messages_follow_the_window_s_language(self):
        u = pfs0(self.dir / 'u [0100F2C0115B6800][v720896].nsp')
        with mock.patch.dict(os.environ, {'MNG_LANG': 'zh'}):
            self.assertIn('还没有游戏本体', classify.plan(classify.classify([u])).blocks[0])
        with mock.patch.dict(os.environ, {'MNG_LANG': 'en'}):
            self.assertIn('no game', classify.plan(classify.classify([u])).blocks[0])

    def test_plan_refuses_two_games(self):
        a = pfs0(self.dir / 'a [0100F2C0115B6000].nsp')
        b = pfs0(self.dir / 'b [01007EF00011E000].nsp')
        p = classify.plan(classify.classify([a, b]))
        self.assertIsNone(p.game)
        self.assertIn('several games', p.blocks[0])

    def test_plan_without_a_base(self):
        u = pfs0(self.dir / 'u [0100F2C0115B6800][v720896].nsp')
        p = classify.plan(classify.classify([u]))
        self.assertIn('an update alone', p.blocks[0])


class Command(unittest.TestCase):
    def test_arguments(self):
        plan = dict(game='totk', base='/g/b.xci', keys='/k/prod.keys', update='/g/u.nsp', dlc=[], firmware=None,
                    fonts='/f', blocks=[])
        with tempfile.TemporaryDirectory() as tmp:
            argv, work, out = build_job.command(dict(plan=plan, output_dir=tmp, work_root='/w'))
        self.assertEqual(argv, ['/g/b.xci', '-k', '/k/prod.keys', '--update', '/g/u.nsp', '--shared-fonts', '/f',
                                '--work', '/w/totk', '-o', str(Path(tmp) / 'Tears of the Kingdom.app')])

    def test_a_blocked_plan_is_refused(self):
        with self.assertRaises(build_job.Refused):
            build_job.command(dict(plan=dict(game='totk', base='/b', blocks=['no keys']), output_dir='/o', work_root='/w'))

    def test_a_dump_needs_no_keys(self):
        plan = dict(game='dkb', base='/d', keys='/k', blocks=[])
        argv, _, _ = build_job.command(dict(plan=plan, output_dir='/o', work_root='/w'))
        self.assertNotIn('-k', argv)


class History(unittest.TestCase):
    def test_measured_fraction_and_skipped_steps(self):
        e = history.Estimator([('a', 10.0), ('b', 30.0), ('c', 60.0)])
        self.assertEqual(e.update('a', 5, 1)[:2], (0.05, 95.0))
        self.assertEqual(e.update('c', 0, 3)[:2], (0.4, 60.0))   # b skipped (inputs unchanged): counted as done
        self.assertEqual(e.update('c', 600, 3)[0], 0.99)         # never 1 before the build ends
        self.assertEqual(e.update('zzz', 1, 3), (0.99, None, True))

    def test_without_a_record_the_stage(self):
        e = history.Estimator(None)
        self.assertEqual(e.update('x', 5, 3), (history.STAGE_STARTS[3], None, False))

    def test_record_and_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'r.json'
            history.record(path, [('a', 100.0), ('b', 110.0)], 150.0)
            self.assertEqual(history.load(path), [('a', 10.0), ('b', 40.0)])
            path.write_text('{damaged')
            self.assertIsNone(history.load(path))


class Checks(unittest.TestCase):
    def test_build_app_prerequisites_are_restated(self):
        source = repo.BUILD_APP.read_text()
        start = source.index('def prerequisites(')
        body = source[start:source.index('\ndef ', start + 1)]
        for package in checks.BREW_PACKAGES:
            self.assertIn(f"'{package}'", body)
        self.assertIn("('opus', 'libvpx')", body, 'build_app.py needs other Homebrew packages: update checks.BREW_PACKAGES')

    def test_every_check_has_text(self):
        for c in checks.checks(tempfile.gettempdir()):
            self.assertTrue(c['text'])
            self.assertTrue(c['ok'] or c['fix'], c['id'])


FAKE_BUILD = textwrap.dedent('''
    import sys, time, pathlib
    args = sys.argv[1:]
    work = pathlib.Path(args[args.index('--work') + 1])
    (work / 'logs').mkdir(parents=True, exist_ok=True)
    def say(m): print(f'[{time.strftime("%H:%M:%S")}] {m}', flush=True)
    say('1/4 code: rewriting main')
    (work / 'logs/rewrite-main.log').write_text('x')
    time.sleep(1.2)
    say('3/4 shaders: translating')
    (work / 'logs/prepare-shader-seed.log').write_text('prepared 100/400, skipped 0; 1.0s\\nprepared 300/400, skipped 0; 2.0s\\n')
    time.sleep(1.5)
    mode = (work / 'mode').read_text() if (work / 'mode').exists() else 'ok'
    if mode == 'fail':
        print('\\nERROR: tool failed (exit 3); full output: ' + str(work / 'logs/prepare-shader-seed.log'), file=sys.stderr)
        print('last line of the tool', file=sys.stderr)
        sys.exit(1)
    if mode == 'slow':
        time.sleep(30)
    say('done in 0 min: ' + args[args.index('-o') + 1])
''')


class Run(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.script = self.dir / 'build_app.py'
        self.script.write_text(FAKE_BUILD)
        self.logs = mock.patch.object(build_job, 'LOGS', self.dir / 'logs')
        self.logs.start()
        self.request = dict(plan=dict(game='dkb', base=str(self.dir), blocks=[]), output_dir=str(self.dir / 'out'),
                            work_root=str(self.dir / 'work'))

    def tearDown(self):
        self.logs.stop()
        self.tmp.cleanup()

    def events(self, mode='ok'):
        (self.dir / 'work/dkb').mkdir(parents=True, exist_ok=True)
        (self.dir / 'work/dkb/mode').write_text(mode)
        out = io.StringIO()
        code = build_job.run(self.request, Emitter(out), self.dir / 'history', script=self.script)
        return code, [json.loads(line) for line in out.getvalue().splitlines()]

    def test_a_build_reports_stages_steps_progress_and_records_its_times(self):
        code, events = self.events()
        kinds = [e['event'] for e in events]
        self.assertEqual(code, 0)
        self.assertEqual(kinds[0], 'plan')
        self.assertEqual([e['index'] for e in events if e['event'] == 'stage'], [1, 3])
        self.assertEqual([e['name'] for e in events if e['event'] == 'step'], ['rewrite-main', 'prepare-shader-seed'])
        self.assertIn('progress', kinds)
        self.assertIn((300, 400), [(e.get('step_done'), e.get('step_total')) for e in events if e['event'] == 'progress'])
        self.assertEqual(events[-1]['event'], 'done')
        self.assertTrue(events[-1]['ok'])
        steps = history.load(history.record_path(self.dir / 'history', 'dkb', 'macos'))
        self.assertEqual([n for n, _ in steps], ['rewrite-main', 'prepare-shader-seed'])
        _, again = self.events()   # the second build's bar comes from the first one's times
        self.assertTrue(again[0]['measured'])
        self.assertTrue(any(e['event'] == 'progress' and e['left'] is not None for e in again))

    def test_a_failure_names_the_error_and_the_step_log(self):
        code, events = self.events('fail')
        done = events[-1]
        self.assertNotEqual(code, 0)
        self.assertFalse(done['ok'])
        self.assertTrue(done['message'].startswith('tool failed (exit 3)'))
        self.assertIn('last line of the tool', done['detail'])
        self.assertTrue(done['step_log'].endswith('prepare-shader-seed.log'))
        self.assertTrue(Path(done['log']).is_file())

    def test_a_cancel_stops_the_build_group(self):
        (self.dir / 'work/dkb').mkdir(parents=True)
        (self.dir / 'work/dkb/mode').write_text('slow')
        request = self.dir / 'request.json'
        request.write_text(json.dumps(self.request))
        runner = textwrap.dedent(f'''
            import sys; sys.path.insert(0, {str(ROOT)!r})
            from unittest import mock; from pathlib import Path
            from tools.builder_jobs import build_job
            from tools.builder_jobs.events import Emitter
            import json
            build_job.LOGS = Path({str(self.dir / 'logs')!r})
            sys.exit(build_job.run(json.loads(Path({str(request)!r}).read_text()), Emitter(sys.stdout),
                                   Path({str(self.dir / 'history')!r}), script=Path({str(self.script)!r})))
        ''')
        p = subprocess.Popen([sys.executable, '-c', runner], stdout=subprocess.PIPE, text=True)
        lines = []
        for line in p.stdout:
            lines.append(json.loads(line))
            if lines[-1]['event'] == 'step' and lines[-1]['name'] == 'prepare-shader-seed':
                p.send_signal(signal.SIGINT)
        p.wait(timeout=20)
        self.assertIn('cancelling', [e['event'] for e in lines])
        self.assertFalse(lines[-1]['ok'])
        self.assertTrue(lines[-1]['cancelled'])


if __name__ == '__main__':
    unittest.main()
