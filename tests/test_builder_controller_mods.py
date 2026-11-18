"""tools/builder/controller_mods.py (S5.2): the Mods page's model calls on a real library in a temporary folder (the
state, a change, an Add refused or cancelled, the re-check of mods validated by older rules), plus the drift test of
main's mod tools the builder calls in-process (names and signatures). The library's own rules are in
test_builder_mod_library.py and test_builder_mod_tools.py; the page over these calls in test_launcher_tab_mods.py."""
import inspect
import os
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from unittest.mock import patch

from test_mod_manifest import fake_romfs
from tools.builder import controller_mods, mod_add, mod_library, mod_tools
from tools.builder.controller_mods import ModsState
from tools.builder.games import TOTK
from tools.builder.ports import CommandRunner, RealClock

REPO = Path(__file__).resolve().parents[1]


class Mods(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        home = patch.dict(os.environ, HOME=str(self.tmp / 'home'))
        home.start()
        self.addCleanup(home.stop)

    def test_without_a_built_app_the_state_says_so(self):
        state = controller_mods.read_state(REPO, TOTK, self.tmp / 'Tears of the Kingdom.app', self.tmp / 'data')
        self.assertEqual((state.reason, state.mods), ('no_app', ()))
        self.assertEqual(controller_mods.read_state(REPO, TOTK, None, self.tmp / 'data').reason, 'no_game_file')

    def test_a_change_reads_the_library_without_re_checking(self):
        with patch.object(controller_mods, '_library'), patch.object(mod_library, 'move'), \
                patch.object(controller_mods, 'read_state', return_value='quick') as read:
            self.assertEqual(controller_mods.change(REPO, TOTK, None, None, 'move', 'a', 1), 'quick')
        self.assertEqual(read.call_args.kwargs, {'full': False})
        with patch.object(controller_mods, '_library'):
            self.assertRaises(ValueError, controller_mods.change, REPO, TOTK, None, None, 'rename', 'a')

    def test_code_mods_on_are_named_before_the_app_is_built(self):                  # S5.4
        library = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp / 'load')
        library.state.update(order=['s'], names={'s': 'Stamina'}, code=['s'])
        mod_library.save(library)
        state = controller_mods.read_state(REPO, TOTK, self.tmp / 'Tears of the Kingdom.app', self.tmp / 'data',
                                           full=False)
        self.assertEqual((state.reason, state.code_next), ('no_app', ('Stamina',)))

    def test_a_refused_add_comes_back_in_the_state(self):                          # S5.4 C5
        (self.tmp / 'Empty').mkdir()
        with patch.object(controller_mods.mod_tools, 'top_folders', return_value=set()), \
                patch.object(controller_mods, 'read_state', return_value=ModsState('totk', 'T', '1.4.3', None)):
            changed = controller_mods.change(REPO, TOTK, None, self.tmp / 'data', 'add', None, self.tmp / 'Empty', None)
        self.assertEqual(changed.refused, ('nothing', str(self.tmp / 'Empty')))

    def test_a_cancelled_archive_add_is_refused_and_adds_nothing(self):             # S5.5 B2
        (self.tmp / 'pack.7z').write_bytes(b'not read before the cancel')
        cancel = threading.Event()
        cancel.set()
        changed = controller_mods.change(REPO, TOTK, None, self.tmp / 'data', 'add', CommandRunner(RealClock()),
                                         self.tmp / 'pack.7z', cancel)
        self.assertEqual(changed.refused, ('cancelled', ''))
        library = mod_library.open_library(self.tmp / 'data/TOTK', 'v143', self.tmp / 'load')
        self.assertEqual((library.state['order'], (library.root / mod_add.UNZIP).exists()), ([], False))

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
