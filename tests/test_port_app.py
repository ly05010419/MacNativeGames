"""tools/port_app: what the build.py and packagers of Super Mario Odyssey, Animal Crossing and Echoes of Wisdom share
(2026-11-18). The adapter keeps MNG Launcher's command line and the express update's entry; the bundle steps sign
and record as build_app.py does for the Zelda games. Everything runs in temporary folders."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.port_app import adapter, bundle  # noqa: E402

GAMES = ('odyssey', 'acnh', 'eow')


def load_build(game):
    sys.path.insert(0, str(ROOT / f'games/{game}'))
    spec = importlib.util.spec_from_file_location(f'port_app_test_{game}', ROOT / f'games/{game}/build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def png(path, size=16):
    """A small opaque PNG, for the icon conversion."""
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    rows = b''.join(b'\0' + b'\x20\x80\xe0' * size for _ in range(size))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


class Temp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()


class Adapter(Temp):
    def test_stage_lines_are_what_the_launcher_reads(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            adapter.stage(5, 8, 'Loader tables')
        self.assertEqual(out.getvalue(), '5/8 Loader tables\n')

    def test_the_work_folder_is_outside_the_checkout_and_the_app(self):
        app = self.root / 'A.app'
        for bad in (ROOT, ROOT / 'build', ROOT.parent, app, app / 'Contents/work'):
            with self.assertRaisesRegex(ValueError, 'outside the checkout'):
                adapter.checked_work(bad, app, ROOT)
        self.assertEqual(adapter.checked_work(self.root / 'work', app, ROOT), self.root / 'work')

    def test_python_for_keeps_a_venv_whose_marker_matches(self):
        for kind in ('packages', 'requirements'):
            work = self.root / kind
            (work / 'venv/bin').mkdir(parents=True)
            (work / 'venv/bin/python').write_text('')
            requirements = self.root / 'requirements.txt'
            requirements.write_text('lz4==4.4.5\n')
            if kind == 'packages':
                (work / 'venv/.packages').write_text('a==1\nb==2\n')
                given = {'packages': ('a==1', 'b==2')}
            else:
                import hashlib
                (work / 'venv/.requirements.sha256').write_text(hashlib.sha256(requirements.read_bytes()).hexdigest() + '\n')
                given = {'requirements': requirements}
            with mock.patch.object(subprocess, 'run', side_effect=AssertionError('no venv should be made')):
                self.assertEqual(adapter.python_for(work, **given), work / 'venv/bin/python')
        with self.assertRaises(TypeError):
            adapter.python_for(self.root)

    def test_python_for_makes_the_venv_again_when_the_pins_change(self):
        work = self.root / 'work'
        (work / 'venv/bin').mkdir(parents=True)
        (work / 'venv/bin/python').write_text('')
        (work / 'venv/.packages').write_text('a==1\n')
        calls = []
        with mock.patch.object(subprocess, 'run', side_effect=lambda cmd, **kw: calls.append(cmd)):
            adapter.python_for(work, packages=('a==2',))
        self.assertEqual(calls[0][1:], ['-m', 'venv', str(work / 'venv')])
        self.assertEqual(calls[1][-1], 'a==2')
        self.assertEqual((work / 'venv/.packages').read_text(), 'a==2\n')

    def test_main_runs_the_update_entry_or_the_build_and_says_errors(self):
        seen = []
        self.assertEqual(adapter.main(lambda argv: seen.append(('build', argv)), lambda a, w: seen.append(('update', a, w)),
                                      ['--update-app', '/A.app', '/w']), 0)
        self.assertEqual(adapter.main(lambda argv: seen.append(('build', argv)), None, ['g.xci', '-k', 'k']), 0)
        self.assertEqual(seen, [('update', Path('/A.app'), Path('/w')), ('build', ['g.xci', '-k', 'k'])])
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            def refuse(argv):
                raise ValueError('no keys')
            self.assertEqual(adapter.main(refuse, None, []), 1)
        self.assertEqual(err.getvalue(), 'ERROR: no keys\n')

    def test_update_reruns_build_py_in_the_work_folder_python(self):
        app, work = self.root / 'A.app', self.root / 'work'
        runs = []
        with mock.patch.object(subprocess, 'run', side_effect=lambda cmd, **kw: runs.append(cmd) or mock.Mock(returncode=0)):
            got = adapter.update_in_venv('/repo/games/x/build.py', app, work, blocker=lambda a, w: '',
                                         python=lambda w: w / 'venv/bin/python')
        self.assertIsNone(got)
        self.assertEqual(runs, [[str(work / 'venv/bin/python'), '/repo/games/x/build.py', '--update-app', str(app), str(work)]])
        with self.assertRaisesRegex(ValueError, 'full build'):
            adapter.update_in_venv('/b.py', app, work, blocker=lambda a, w: 'a full build is required', python=None)
        with mock.patch.object(adapter, 'in_venv', return_value=True):
            self.assertEqual(adapter.update_in_venv('/b.py', app, work, blocker=lambda a, w: '', python=lambda w: 'py'),
                             (app, work, 'py'))

    def test_a_failed_rerun_stops_with_its_words(self):
        with mock.patch.object(subprocess, 'run', return_value=mock.Mock(returncode=1)):
            with self.assertRaisesRegex(ValueError, 'its message above'):
                adapter.reexec('/b.py', 'py', [], stopped='the build stopped (its message above says why)')
            with self.assertRaisesRegex(ValueError, 'ERROR line'):
                adapter.reexec('/b.py', 'py', [])

    def test_the_build_reruns_with_the_work_folder_and_app_named(self):
        runs = []
        with mock.patch.object(subprocess, 'run', side_effect=lambda cmd, **kw: runs.append(cmd) or mock.Mock(returncode=0)):
            self.assertTrue(adapter.rerun_in_venv('/b.py', 'py', self.root / 'w', ['g.xci', '-k', 'k'], self.root / 'A.app'))
        self.assertEqual(runs[0], ['py', '/b.py', 'g.xci', '-k', 'k', '--work', str(self.root / 'w'), '-o', str(self.root / 'A.app')])

    def test_inputs_and_output(self):
        game, keys = self.root / 'g.xci', self.root / 'prod.keys'
        game.write_text(''), keys.write_text('')
        p = adapter.parser('doc')
        adapter.add_inputs(p, game_help='', update_help='', update_required=False)
        adapter.add_fonts(p)
        adapter.add_output(p, 'Some Game', 'some')
        args = p.parse_args([str(game), '-k', str(keys), '--work', str(self.root / 'w')])
        self.assertEqual(adapter.inputs(args), (game, keys, None))
        with contextlib.redirect_stdout(io.StringIO()) as said:
            out, work = adapter.output_and_work(args, game, 'Some Game', 'some')
        self.assertEqual((out, work), (self.root / 'Some Game.app', self.root / 'w'))
        self.assertTrue(work.is_dir())
        self.assertIn('given with --work', said.getvalue())
        with self.assertRaisesRegex(ValueError, 'must end in .app'):
            adapter.output_and_work(p.parse_args([str(game), '-k', str(keys), '-o', str(self.root / 'x')]), game, 'G', 'g')
        with self.assertRaisesRegex(ValueError, 'no such file'):
            adapter.inputs(p.parse_args([str(game), '-k', str(keys), '--update', str(self.root / 'missing.nsp')]))

    def test_fonts_ready_and_first_missing(self):
        fonts = self.root / 'fonts'
        fonts.mkdir()
        rows = [{'type': i, 'name': f'n{i}'} for i in range(6)]
        for row in rows:
            (fonts / f'{row["type"]}-{row["name"]}.bfttf').write_bytes(b'x')
        (fonts / 'manifest.json').write_text(json.dumps({'status': 'pass', 'fonts': rows}))
        self.assertTrue(adapter.fonts_ready(fonts))
        self.assertFalse(adapter.fonts_ready(fonts, ('bfttf_bytes',)))
        self.assertFalse(adapter.fonts_ready(self.root))
        self.assertEqual(adapter.first_missing([(fonts / 'manifest.json', 'x')]), '')
        self.assertEqual(adapter.first_missing([(fonts / 'nope', 'build Python')]),
                         'the work folder has no build Python: a full build is required')


class GameAdapters(Temp):
    """Each game's build.py keeps MNG Launcher's command line, its constants and the update's entry."""

    def test_help_lists_the_launcher_options(self):
        for game in GAMES:
            done = subprocess.run([sys.executable, str(ROOT / f'games/{game}/build.py'), '--help'],
                                  capture_output=True, text=True, cwd=self.root)
            self.assertEqual(done.returncode, 0, done.stderr)
            for option in ('-k KEYS', '--update UPDATE', '--firmware FIRMWARE', '--shared-fonts SHARED_FONTS',
                           '--work WORK', '-o OUT', f'MacNativeGames-work/{game}'):
                self.assertIn(option, done.stdout, (game, option))
        acnh = subprocess.run([sys.executable, str(ROOT / 'games/acnh/build.py'), '--help'], capture_output=True, text=True).stdout
        for option in ('--dlc DLC', '--fps-mod FPS_MOD', '--no-fps-mod'):
            self.assertIn(option, acnh)

    def test_the_launchers_argv_is_accepted_and_the_work_folder_checked(self):
        game, keys, update = self.root / 'g.xci', self.root / 'prod.keys', self.root / 'u.nsp'
        for path in (game, keys, update):
            path.write_text('')
        for name in GAMES:
            build = load_build(name)
            argv = [str(game), '-k', str(keys), '--update', str(update), '--work', str(ROOT / 'inside'), '-o', str(self.root / 'X.app')]
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertEqual(build.main(argv), 1)
            self.assertIn('ERROR: the work folder must be outside the checkout', err.getvalue(), name)

    def test_update_app_refuses_another_app_with_an_error_line(self):
        app = self.root / 'Other.app'
        (app / 'Contents').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.example.other'}))
        for name in GAMES:
            build = load_build(name)
            with contextlib.redirect_stderr(io.StringIO()) as err:
                self.assertEqual(build.main(['--update-app', str(app), str(self.root / 'work')]), 1)
            self.assertTrue(err.getvalue().startswith('ERROR: '), (name, err.getvalue()))
            self.assertTrue(build.blocker(app, self.root / 'work'))

    def test_identity(self):
        names = {'odyssey': ('Super Mario Odyssey', 'org.macnativegames.odyssey'),
                 'acnh': ('Animal Crossing New Horizons', 'org.macnativegames.acnh'),
                 'eow': ('The Legend of Zelda Echoes of Wisdom', 'org.macnativegames.eow')}
        for name, (app, bundle_id) in names.items():
            build = load_build(name)
            self.assertEqual((build.APP_NAME, build.BUNDLE_ID), (app, bundle_id))
            self.assertTrue(callable(build.blocker) and callable(build.update) and callable(build.main))


class Bundle(Temp):
    def test_info_keys(self):
        info = bundle.bundle_info(executable='E', bundle_id='org.example.e', name='N', display_name='D', version='1.0',
                                  build='1', minimum_system='26.0', icon_file='AppIcon')
        self.assertEqual(sorted(info), ['CFBundleDevelopmentRegion', 'CFBundleDisplayName', 'CFBundleExecutable',
                                        'CFBundleIconFile', 'CFBundleIdentifier', 'CFBundleInfoDictionaryVersion',
                                        'CFBundleName', 'CFBundlePackageType', 'CFBundleShortVersionString',
                                        'CFBundleVersion', 'LSApplicationCategoryType', 'LSMinimumSystemVersion',
                                        'NSHighResolutionCapable'])
        self.assertNotIn('CFBundleIconFile', bundle.bundle_info(executable='E', bundle_id='i', name='N', display_name='D',
                                                                version='1', build='1', minimum_system='26.0'))

    def test_update_record_has_build_app_keys(self):
        import build_app
        info = {}
        build_app.record_update_source(info, self.root / 'work')
        self.assertEqual(bundle.update_record((self.root / 'work').resolve(), build_app.REPO), info)

    def test_signing_requirement_is_build_app_s(self):
        source = (ROOT / 'build_app.py').read_text()
        sign_app = source[source.index('def sign_app('):]
        self.assertIn("f'-r=designated => identifier \"{bundle_id}\"'", sign_app[:sign_app.index('\ndef ')])
        mine = (ROOT / 'tools/port_app/bundle.py').read_text()
        self.assertIn("f'-r=designated => identifier \"{bundle_id}\"'", mine)

    def test_clones_follow_or_keep_links(self):
        source = self.root / 'src'
        (source / 'sub').mkdir(parents=True)
        (source / 'sub/file').write_text('data')
        os.symlink('sub/file', source / 'link')
        bundle.clone_file(source / 'link', self.root / 'out/followed')
        self.assertFalse((self.root / 'out/followed').is_symlink())
        self.assertEqual((self.root / 'out/followed').read_text(), 'data')
        bundle.clone_tree(source, self.root / 'out/tree')
        self.assertTrue((self.root / 'out/tree/link').is_symlink())
        self.assertEqual((self.root / 'out/tree/sub/file').read_text(), 'data')

    def test_staging_replaces_only_a_complete_app(self):
        out = self.root / 'G.app'
        (out / 'Contents').mkdir(parents=True)
        (out / 'Contents/old').write_text('old')
        with self.assertRaises(RuntimeError):
            with bundle.staged(out) as stage:
                (stage / 'Contents').mkdir(parents=True)
                raise RuntimeError('packaging failed')
        self.assertEqual((out / 'Contents/old').read_text(), 'old')
        self.assertFalse(out.with_name('G.app.partial').exists())
        with bundle.staged(out) as stage:
            self.assertEqual(stage.name, 'G.app.partial')
            (stage / 'Contents').mkdir(parents=True)
            (stage / 'Contents/new').write_text('new')
        self.assertEqual(sorted(p.name for p in (out / 'Contents').iterdir()), ['new'])
        self.assertFalse(stage.exists())

    @unittest.skipUnless(shutil.which('codesign') and shutil.which('xcrun'), 'needs codesign and the command line tools')
    def test_a_signed_app_names_its_bundle_id_and_verifies(self):
        for deep in (False, True):
            app = self.root / f'S{deep}.app'
            (app / 'Contents/MacOS').mkdir(parents=True)
            source = self.root / 'hello.c'
            source.write_text('#include <stdio.h>\nint main(int c, char **v) { (void)v; return c == 2 ? 0 : 1; }\n')
            bundle.compile_launcher(source, app / 'Contents/MacOS/S')
            png(self.root / 'icon.png')
            self.assertEqual(bundle.icon(self.root / 'icon.png', app / 'Contents/Resources', 'Icon'), 'Icon')
            self.assertTrue((app / 'Contents/Resources/Icon.icns').read_bytes().startswith(b'icns'))
            self.assertIsNone(bundle.icon(self.root / 'none.icns', app / 'Contents/Resources', 'X'))
            bundle.write_info(app, bundle.bundle_info(executable='S', bundle_id='org.example.s', name='S', display_name='S',
                                                     version='1', build='1', minimum_system='26.0', icon_file='Icon'))
            bundle.sign(app, 'org.example.s', deep=deep)
            bundle.verify_signature(app)
            bundle.check_launcher(app / 'Contents/MacOS/S')
            shown = subprocess.run(['codesign', '-dr', '-', str(app)], capture_output=True, text=True)
            self.assertIn('designated => identifier "org.example.s"', shown.stdout + shown.stderr)


if __name__ == '__main__':
    unittest.main()
