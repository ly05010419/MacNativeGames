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
            with mock.patch.object(update_runtime, 'oot_update_blocker', return_value='') as blocker:   # its build files: tests/test_oot_update_plan.py
                p = update_runtime.plan(out)
            blocker.assert_called_once_with(work.resolve(), out.resolve())
        self.assertEqual((p['kind'], p['game'], p['work']), ('express', 'oot', str(work.resolve())))

    def switch_2_app(self, tmp, version, *, data=True):
        """A Switch 2 app as build_app.py lays it out (rewritten/<version>, build/game-data/<version>), its work folder recorded."""
        work = Path(tmp) / f'work/{version}'
        work.mkdir(parents=True)
        out = app(tmp, f'{build_app.APPS[version][0]}.app', CFBundleExecutable=build_app.APPS[version][2], MNGWorkFolder=str(work))
        res = out / 'Contents/Resources/totk'
        (res / f'rewritten/{version}').mkdir(parents=True)
        if data:
            for name in build_app.game_data_files(version):
                (res / f'build/game-data/{version}').mkdir(parents=True, exist_ok=True)
                (res / f'build/game-data/{version}' / name).write_text('x')
        return out, work

    def test_a_switch_2_app_takes_the_express_update_like_the_others(self):
        # Bananza and Pokopia were told "only the normal update" without a look (user 2026-11-12: "为什么要完整打包？"); they
        # are judged by the same checks as Tears of the Kingdom: their game data, modules and seed
        for version in ('dkb', 'pkp'):
            with tempfile.TemporaryDirectory() as tmp, \
                    mock.patch.object(update_runtime, 'check_game_data'), mock.patch.object(update_runtime, 'check_rewriter'), \
                    mock.patch.object(update_runtime, 'check_seed_chain', return_value=False):
                out, work = self.switch_2_app(tmp, version)
                p = update_runtime.plan(out)
            self.assertEqual((p['kind'], p['game'], p['work']), ('express', version, str(work.resolve())), version)

    def test_a_switch_2_app_whose_seed_is_of_another_chain_takes_the_fast_update(self):
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(update_runtime, 'check_game_data'), mock.patch.object(update_runtime, 'check_rewriter'), \
                mock.patch.object(update_runtime, 'check_seed_chain', return_value=True), \
                mock.patch.object(update_runtime, 'check_work_files'):
            out, _ = self.switch_2_app(tmp, 'dkb')
            self.assertEqual(update_runtime.plan(out)['kind'], 'fast')

    def test_a_switch_2_app_without_its_game_data_says_what_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out, _ = self.switch_2_app(tmp, 'pkp', data=False)
            p = update_runtime.plan(out)
        self.assertEqual((p['kind'], p['game']), ('full', 'pkp'))
        self.assertIn('no game data folder', p['reason'])
        self.assertNotIn('only the normal update', p['reason'])

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



class SwitchTwoSeed(unittest.TestCase):
    def test_a_new_seed_is_made_by_the_switch_2_steps_after_the_runtime(self):
        # prepare_ns2_seed.py translates with the game binary (build/totk-native-<version>): make runs first
        calls = []
        with mock.patch.object(build_app, 'step_shaders_ns2', side_effect=lambda *a, **k: calls.append('ns2')), \
                mock.patch.object(build_app, 'step_shaders', side_effect=lambda *a, **k: calls.append('totk')), \
                mock.patch.object(build_app, 'step_shader_libs', side_effect=lambda *a, **k: calls.append('libs')), \
                mock.patch.object(build_app, 'build_warm_tool', side_effect=lambda *a, **k: calls.append('warm')):
            update_runtime.make_seed(Path('/tree'), Path('/work'), 'dkb', 'dkb', Path('/logs'), {})
        self.assertEqual(calls, ['ns2', 'libs', 'warm'])
        self.assertTrue(update_runtime.seed_after_runtime('pkp'))
        self.assertFalse(update_runtime.seed_after_runtime('totk') or update_runtime.seed_after_runtime('botw'))


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
