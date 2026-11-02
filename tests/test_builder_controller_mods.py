"""tools/builder/controller_mods.py (S5.2): the Mods tab's part of a real BuildController with InlineJobs (jobs on
load_jobs, one at a time, Apply excluding builds and Play: O3), plus the drift test of main's mod tools the builder
calls in-process (names and signatures). The model's behaviour is in test_builder_mod_library.py and
test_builder_mod_tools.py."""
import inspect
import os
from pathlib import Path
import unittest
from unittest.mock import patch

from test_mod_manifest import fake_romfs
from tools.builder import controller_mods, mod_add, mod_library, mod_tools
from tools.builder.games import TOTK
from tools.builder.jobs import InlineJobs
from tools.builder.controller_mods import ModsState
from tools.builder.session import ModsApplied, ModsStateReady, ProcessError
from tools.builder.start_options import StartPaths
from test_builder_controller import Base

REPO = Path(__file__).resolve().parents[1]


class Mods(Base):
    def setUp(self):
        super().setUp()
        home = patch.dict(os.environ, HOME=str(self.tmp / 'home'))
        home.start()
        self.addCleanup(home.stop)
        self.load_jobs = self.ctl.load_jobs = InlineJobs()
        self.ctl.repo_root = REPO                       # main's mod tools are imported from it (any test order)
        self.ctl.start.paths = StartPaths(self.tmp / 'Builder/start-options.json', self.tmp / 'data')
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'))
        self.events()

    def run_load(self):
        self.load_jobs.run_next()
        self.ctl.tick()
        return self.events()

    def test_state_runs_off_the_worker(self):
        self.ctl.mods.state()
        self.assertEqual((self.jobs.pending(), self.load_jobs.pending(), self.ctl.mods.busy), (0, 1, True))
        [event] = self.run_load()
        self.assertIsInstance(event, ModsStateReady)
        self.assertEqual(event.state.reason, 'no_app')                 # no app built in this folder

    def test_one_job_at_a_time(self):
        self.ctl.mods.state()
        self.assertRaises(RuntimeError, self.ctl.mods.move, 'a', 1)

    def test_apply_makes_the_controller_busy_and_excludes_builds_and_play(self):
        with patch.object(controller_mods, 'apply', return_value=mod_library.ApplyResult(True)):
            self.ctl.mods.apply()
            self.assertFalse(self.ctl.idle)                              # O3: no build, no Play beside it
            self.assertRaises(RuntimeError, self.ctl.start_build)
            self.assertRaises(RuntimeError, self.ctl.start.play)
            self.assertEqual(self.run_load(), [ModsApplied(mod_library.ApplyResult(True))])
        self.assertTrue(self.ctl.idle)

    def test_apply_waits_for_a_build_or_a_play(self):
        self.started()
        self.assertRaises(RuntimeError, self.ctl.mods.apply)

    def test_apply_waits_for_a_settings_job(self):
        self.ctl.start.state()
        self.assertRaises(RuntimeError, self.ctl.mods.apply)

    def test_a_change_reads_the_library_without_re_checking(self):
        with patch.object(controller_mods, '_library'), patch.object(mod_library, 'move'), \
                patch.object(controller_mods, 'read_state', return_value='quick') as read:
            self.assertEqual(controller_mods.change(REPO, TOTK, None, None, 'move', 'a', 1), 'quick')
        self.assertEqual(read.call_args.kwargs, {'full': False})

    def test_code_mods_on_are_named_before_the_app_is_built(self):                  # S5.4
        library = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp / 'load')
        library.state.update(order=['s'], names={'s': 'Stamina'}, code=['s'])
        mod_library.save(library)
        self.ctl.mods.state(full=False)
        [event] = self.run_load()
        self.assertEqual((event.state.reason, event.state.code_next), ('no_app', ('Stamina',)))

    def test_a_refused_add_comes_back_in_the_state(self):                          # S5.4 C5
        (self.tmp / 'Empty').mkdir()
        with patch.object(controller_mods.mod_tools, 'top_folders', return_value=set()), \
                patch.object(controller_mods, 'read_state', return_value=ModsState('totk', 'T', '1.4.3', None)):
            changed = controller_mods.change(REPO, TOTK, None, self.tmp / 'data', 'add', None, self.tmp / 'Empty', None)
        self.assertEqual(changed.refused, ('nothing', str(self.tmp / 'Empty')))

    def test_an_archive_add_holds_the_controller_and_cancel_stops_it(self):          # S5.5 B2
        (self.tmp / 'pack.7z').write_bytes(b'not read before the cancel')
        self.ctl.mods.add(self.tmp / 'pack.7z')
        self.assertEqual((self.ctl.mods.holds, self.ctl.idle), (True, False))   # closing asks and waits
        self.assertRaises(RuntimeError, self.ctl.close)
        self.ctl.cancel()
        [event] = self.run_load()
        self.assertEqual((event.state.refused, self.ctl.idle), (('cancelled', ''), True))
        self.ctl.mods.add(self.tmp)                                  # a folder holds it too (its copy)
        self.assertEqual((self.ctl.mods.holds, self.ctl.idle), (True, False))
        self.run_load()
        self.assertTrue(self.ctl.idle)

    def test_mods_checked_by_older_rules_are_checked_again_once(self):              # S5.5 C1b
        app = self.tmp / 'Tears of the Kingdom.app'
        fake_romfs(app / 'Contents/Resources/totk/extracted/v143/romfs',
                   {'Pack/Actor/Player.pack.zs': b'p', 'GameData/GameDataList.Product.140.byml.zs': b'g'})
        library = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp / 'load')
        for path in ('romfs/Pack/Actor/Player.pack.zs', 'romfs/GameData/GameDataList.Product.110.byml.zs'):
            (library.path('ambient') / path).parent.mkdir(parents=True, exist_ok=True)
            (library.path('ambient') / path).write_bytes(b'old')
        (library.root / mod_add.UNZIP).mkdir(parents=True)                         # a crash left it
        library.state.update(order=['ambient'], enabled=['ambient'], code=['ambient'], names={'ambient': 'Ambient'},
                             validation={'ambient': {'valid': True, 'replaced': 1}})        # stored before S5.5
        mod_library.save(library)
        first = controller_mods.read_state(REPO, TOTK, app, self.tmp / 'data')
        self.assertEqual((first.switched_off, first.mods[0].enabled, first.mods[0].validation.reason),
                         ((('Ambient', ('1.1.0',)),), False, 'other_version'))
        reopened = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp / 'load')
        self.assertEqual((reopened.state['code'], (library.root / mod_add.UNZIP).exists()), (['ambient'], False))
        with patch.object(mod_library, 'revalidate') as again:
            self.assertEqual(controller_mods.read_state(REPO, TOTK, app, self.tmp / 'data').switched_off, ())
        again.assert_not_called()

    def test_no_recheck_while_the_app_has_no_index(self):                           # an app being replaced
        app = self.tmp / 'Tears of the Kingdom.app'
        (app / 'Contents/Resources/totk/extracted/v143').mkdir(parents=True)
        library = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp / 'load')
        library.state.update(order=['a'], enabled=['a'], names={'a': 'A'}, validation={'a': {'valid': True}})
        mod_library.save(library)
        with patch.object(mod_library, 'revalidate') as checked:
            self.assertEqual(controller_mods.read_state(REPO, TOTK, app, self.tmp / 'data').switched_off, ())
        checked.assert_not_called()

    def test_errors_are_reported(self):
        with patch.object(controller_mods, 'read_state', side_effect=OSError('disk')):
            self.ctl.mods.state()
            self.assertEqual(self.run_load(), [ProcessError('mods', "OSError('disk')")])


class MainsTools(unittest.TestCase):
    """Drift: the functions and names of main's mod tools the builder calls."""

    def test_mod_folder(self):
        _, folder_tool = mod_tools.tools(REPO)
        self.assertEqual(list(inspect.signature(folder_tool.install).parameters),
                         ['mod', 'version', 'app', 'py', 'tree', 'say'])
        for name in ('mods_folder', 'backup_name', 'InstallError'):
            self.assertTrue(hasattr(folder_tool, name), name)

    def test_make_mod_manifest(self):
        manifest_tool, _ = mod_tools.tools(REPO)
        for name in ('manifest', 'manifest_text', 'app_overlays', 'build_patches', 'changes_original', 'read_index',
                     'KEPT', 'APP_VERSIONS', 'MANIFEST_HEADER'):
            self.assertTrue(hasattr(manifest_tool, name), name)
        self.assertEqual(list(inspect.signature(manifest_tool.manifest).parameters),
                         ['mod', 'romfs', 'excludes', 'app_replaced', 'merged'])
        source = inspect.getsource(manifest_tool.main)
        self.assertLess(source.index('merged = {}'), source.index('found = manifest('))   # merged before the call


if __name__ == '__main__':
    unittest.main()
