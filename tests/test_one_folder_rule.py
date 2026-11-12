"""One folder rule for build_app.py, update_runtime.py and MNG Launcher (user 2026-11-12: "目录规范要和 launcher 统一 这样用
launcher的时候 就可以极速打包了"), and update_runtime.py --plan, which tells the fastest update that fits without changing
anything (the Launcher's Update button runs it)."""
import contextlib
import io
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
import build_app  # noqa: E402
import update_runtime  # noqa: E402


def app(folder, name='Tears of the Kingdom.app', **info):
    path = Path(folder) / name
    (path / 'Contents').mkdir(parents=True)
    (path / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    return path


class DefaultWork(unittest.TestCase):
    def test_the_folder_the_app_records_comes_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            recorded = Path(tmp) / 'disk/scratch/build/totk'
            recorded.parent.mkdir(parents=True)
            out = app(Path(tmp) / 'disk/Applications', MNGWorkFolder=str(recorded))
            self.assertEqual(build_app.default_work(out, 'totk')[0], recorded)

    def test_else_beside_the_apps_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'disk/Applications/Breath of the Wild.app'
            out.parent.mkdir(parents=True)
            self.assertEqual(build_app.default_work(out, 'botw')[0], Path(tmp) / 'disk/MacNativeGames-work/botw')

    def test_a_recorded_folder_on_a_disk_that_is_gone_is_not_used(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = app(Path(tmp) / 'Applications', MNGWorkFolder='/Volumes/no such disk/work/totk')
            self.assertEqual(build_app.default_work(out, 'totk')[0], Path(tmp) / 'MacNativeGames-work/totk')

    def test_apps_in_a_folder_whose_parent_cannot_be_written_go_to_the_home_folder(self):
        self.assertEqual(build_app.default_work(Path('/Applications/Tears of the Kingdom.app'), 'totk')[0],
                         Path.home() / 'MacNativeGames-work/totk')


class Plan(unittest.TestCase):
    def test_not_an_app_needs_a_full_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(update_runtime.plan(Path(tmp) / 'nothing.app')['kind'], 'full')

    def test_ocarina_of_time_takes_its_express_update_with_its_recorded_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / 'work/oot'
            work.mkdir(parents=True)
            out = app(tmp, 'Ocarina of Time.app', CFBundleExecutable='OcarinaOfTime', MNGWorkFolder=str(work))
            p = update_runtime.plan(out)
        self.assertEqual((p['kind'], p['game'], p['work']), ('express', 'oot', str(work.resolve())))

    def test_a_switch_2_app_has_only_the_full_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = app(tmp, 'Pokémon Pokopia.app', CFBundleExecutable=build_app.APPS['pkp'][2])
            p = update_runtime.plan(out)
        self.assertEqual((p['kind'], p['game']), ('full', 'pkp'))

    def test_a_work_folder_on_a_disk_that_is_gone_is_an_error_naming_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = app(tmp, 'Ocarina of Time.app', CFBundleExecutable='OcarinaOfTime', MNGWorkFolder='/Volumes/gone disk/w/oot')
            p = update_runtime.plan(out)
        self.assertEqual(p['kind'], 'error')
        self.assertIn('gone disk', p['reason'])

    def test_the_command_line_prints_the_plan_and_changes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = app(tmp, 'Pokémon Pokopia.app', CFBundleExecutable=build_app.APPS['pkp'][2])
            before = sorted((p.name, p.stat().st_mtime_ns) for p in Path(tmp).rglob('*'))
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                update_runtime.main([str(out), '--plan'])
            self.assertEqual(json.loads(printed.getvalue())['kind'], 'full')
            self.assertEqual(sorted((p.name, p.stat().st_mtime_ns) for p in Path(tmp).rglob('*')), before)



class ExpressUpdateArguments(unittest.TestCase):
    def test_prerequisites_take_the_express_update_s_arguments(self):
        # update_runtime.py's arguments have no --platform; fa394da8 read args.platform and every express update of
        # Tears of the Kingdom and Breath of the Wild stopped at once (found by the MNG Launcher update test, 2026-11-12)
        import argparse
        args = argparse.Namespace(app=Path('/x.app'), work=None, seed=False, vr=False, native_5k=None, plan=False)
        with mock.patch.object(build_app, 'codec_libraries') as codecs, \
             mock.patch.object(build_app.subprocess, 'run', return_value=mock.Mock(returncode=0, stderr='')), \
             mock.patch.object(build_app.shutil, 'which', return_value='/usr/bin/x'):
            build_app.prerequisites(args)
        codecs.assert_called_once_with('macos')

if __name__ == '__main__':
    unittest.main()
