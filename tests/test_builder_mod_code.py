"""S5.4 C3 (R1-R3): code mods through the build. tools/builder/mod_code.py on a real library in a temporary folder;
the --mod argv parsed by build_app.py itself; and MNG Launcher's full build composing them (tools/builder_jobs/
code_mods.compose over the player's library under the home given), its writer recording the set only when called with
the app a successful build made."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import build_app
from tools.builder import mod_code, mod_library
from tools.builder.mod_tools import Validation
from tools.builder_jobs import code_mods

OK, OTHER = Validation(True, code='ok'), Validation(True, code='other_build')


def library_in(root):
    library = mod_library.open_library(root, 'v143', root / 'load')
    return library


def add_code(library, mod_id, files, on=True):
    exefs = library.path(mod_id) / 'exefs'
    exefs.mkdir(parents=True)
    for name, data in files.items():
        (exefs / name).write_bytes(data)
    library.state['order'].append(mod_id)
    if on:
        mod_code.set_code(library, mod_id, True, OK)
    return library


class Model(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        self.library = library_in(self.tmp / 'TOTK')

    def test_only_mergeable_code_can_be_switched_on_and_the_order_is_the_list(self):
        add_code(self.library, 'b', {'main.pchtxt': b'b'}, on=False)
        add_code(self.library, 'a', {'main.pchtxt': b'a'}, on=False)
        self.assertRaises(ValueError, mod_code.set_code, self.library, 'a', True, OTHER)
        for mod_id in ('a', 'b'):
            mod_code.set_code(self.library, mod_id, True, OK)
        self.assertEqual(mod_code.included(self.library), ['b', 'a'])
        mod_code.set_code(self.library, 'b', False, OTHER)               # switching off needs no check
        self.assertEqual(mod_code.included(self.library), ['a'])

    def test_compose_renames_pchtxt_and_leaves_out_what_is_no_patch(self):
        add_code(self.library, 'a', {'main.pchtxt': b'a', 'main.npdm': b'n', 'subsdk9': b's'})
        add_code(self.library, 'b', {'main.pchtxt': b'b', 'main.ips': b'PATCHEOF'})
        folder, record = mod_code.compose(self.library, ['a', 'b'])
        self.assertEqual(sorted(p.name for p in (folder / 'exefs').iterdir()),
                         ['a-main.pchtxt', 'b-main.pchtxt', 'main.ips'])
        self.assertEqual([(m, [f for f, _ in files]) for m, files in record],
                         [('a', ['a-main.pchtxt']), ('b', ['main.ips', 'b-main.pchtxt'])])

    def test_two_ips_for_main_are_refused_and_nothing_is_left(self):
        add_code(self.library, 'a', {'main.ips': b'PATCHEOF'})
        add_code(self.library, 'b', {'MAIN.ips': b'PATCHEOF'})
        self.assertRaisesRegex(ValueError, 'two IPS patches', mod_code.compose, self.library, ['a', 'b'])
        self.assertFalse(mod_code.folder(self.library).exists())
        self.assertEqual(mod_code.compose(self.library, []), (None, ()))

    def test_the_record_and_build_again(self):
        app = self.tmp / 'Tears of the Kingdom.app'
        self.assertFalse(mod_code.pending(self.library, app))            # nothing on, nothing recorded
        add_code(self.library, 'a', {'main.pchtxt': b'a'})
        self.assertTrue(mod_code.pending(self.library, app))
        _, record = mod_code.compose(self.library, ['a'])
        mod_code.write_record(self.library.root, record, app)
        self.assertFalse(mod_code.pending(self.library, app))
        self.assertTrue(mod_code.pending(self.library, self.tmp / 'Other.app'))   # recorded for another app
        (self.library.path('a') / 'exefs/main.pchtxt').write_bytes(b'changed')
        self.assertTrue(mod_code.pending(self.library, app))

    def test_planned_only_for_totk_with_an_update(self):
        add_code(self.library, 'a', {'main.pchtxt': b'a'})
        self.assertEqual([mod_code.planned(self.library, g, u) for g, u in (('totk', True), ('totk', False),
                                                                             ('botw', True))], [('a',), (), ()])


def library_in(root):
    library = mod_library.open_library(root, 'v143', root / 'load')
    return library


def add_code(library, mod_id, files, on=True):
    exefs = library.path(mod_id) / 'exefs'
    exefs.mkdir(parents=True)
    for name, data in files.items():
        (exefs / name).write_bytes(data)
    library.state['order'].append(mod_id)
    if on:
        mod_code.set_code(library, mod_id, True, OK)
    return library


class Model(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        self.library = library_in(self.tmp / 'TOTK')

    def test_only_mergeable_code_can_be_switched_on_and_the_order_is_the_list(self):
        add_code(self.library, 'b', {'main.pchtxt': b'b'}, on=False)
        add_code(self.library, 'a', {'main.pchtxt': b'a'}, on=False)
        self.assertRaises(ValueError, mod_code.set_code, self.library, 'a', True, OTHER)
        for mod_id in ('a', 'b'):
            mod_code.set_code(self.library, mod_id, True, OK)
        self.assertEqual(mod_code.included(self.library), ['b', 'a'])
        mod_code.set_code(self.library, 'b', False, OTHER)               # switching off needs no check
        self.assertEqual(mod_code.included(self.library), ['a'])

    def test_compose_renames_pchtxt_and_leaves_out_what_is_no_patch(self):
        add_code(self.library, 'a', {'main.pchtxt': b'a', 'main.npdm': b'n', 'subsdk9': b's'})
        add_code(self.library, 'b', {'main.pchtxt': b'b', 'main.ips': b'PATCHEOF'})
        folder, record = mod_code.compose(self.library, ['a', 'b'])
        self.assertEqual(sorted(p.name for p in (folder / 'exefs').iterdir()),
                         ['a-main.pchtxt', 'b-main.pchtxt', 'main.ips'])
        self.assertEqual([(m, [f for f, _ in files]) for m, files in record],
                         [('a', ['a-main.pchtxt']), ('b', ['main.ips', 'b-main.pchtxt'])])

    def test_two_ips_for_main_are_refused_and_nothing_is_left(self):
        add_code(self.library, 'a', {'main.ips': b'PATCHEOF'})
        add_code(self.library, 'b', {'MAIN.ips': b'PATCHEOF'})
        self.assertRaisesRegex(ValueError, 'two IPS patches', mod_code.compose, self.library, ['a', 'b'])
        self.assertFalse(mod_code.folder(self.library).exists())
        self.assertEqual(mod_code.compose(self.library, []), (None, ()))

    def test_the_record_and_build_again(self):
        app = self.tmp / 'Tears of the Kingdom.app'
        self.assertFalse(mod_code.pending(self.library, app))            # nothing on, nothing recorded
        add_code(self.library, 'a', {'main.pchtxt': b'a'})
        self.assertTrue(mod_code.pending(self.library, app))
        _, record = mod_code.compose(self.library, ['a'])
        mod_code.write_record(self.library.root, record, app)
        self.assertFalse(mod_code.pending(self.library, app))
        self.assertTrue(mod_code.pending(self.library, self.tmp / 'Other.app'))   # recorded for another app
        (self.library.path('a') / 'exefs/main.pchtxt').write_bytes(b'changed')
        self.assertTrue(mod_code.pending(self.library, app))

    def test_planned_only_for_totk_with_an_update(self):
        add_code(self.library, 'a', {'main.pchtxt': b'a'})
        self.assertEqual([mod_code.planned(self.library, g, u) for g, u in (('totk', True), ('totk', False),
                                                                             ('botw', True))], [('a',), (), ()])


class Command(unittest.TestCase):
    def test_build_app_reads_the_folder_as_its_mod_and_installs_no_romfs(self):
        argv = ['/G/t.xci', '-k', '/k/prod.keys', '--update', '/G/u.nsp', '--mod', '/L/Builder code mods',
                '--work', '/w', '-o', '/A.app']
        args = build_app.parse_args(argv)
        self.assertEqual((args.mod, args.install_mod_romfs), (Path('/L/Builder code mods'), False))
        self.assertIsNone(build_app.parse_args(['/G/t.xci', '-k', '/k/prod.keys']).mod)

    def test_build_app_still_merges_a_mod_into_the_updated_main_only(self):        # drift, R1
        source = Path(build_app.__file__).read_text()
        self.assertIn("step_code(tree, game, keys, env, logs, mod=None if update else code_mod)", source)
        self.assertIn("(mod / 'exefs').is_dir() or (mod / 'romfs').is_dir()", source)


class LauncherBuild(unittest.TestCase):
    """code_mods.compose as MNG Launcher's full build calls it: the player's library under the home given (HOME there
    too, as the build job runs with MNG_HOME), main's tools from this checkout."""

    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        self.home = self.tmp / 'home'
        env = patch.dict(os.environ, HOME=str(self.home))
        env.start()
        self.addCleanup(env.stop)
        self.data = self.home / 'Library/Application Support/MacNativeGames/TOTK'
        library = add_code(library_in(self.data), 'a', {'main.pchtxt': b'a'})
        mod_library.save(library)
        self.app = self.tmp / 'A.app'

    def library(self):
        return library_in(self.data)

    def compose(self, update='/u.nsp', game='totk'):
        return code_mods.compose({'game': game, 'update': update}, self.home)

    def build(self, app=None, **plan):
        """A successful build: composed at its start, recorded with its app at its end. The --mod words."""
        extra, writer = self.compose(**plan)
        writer(str(app or self.app))
        return extra

    def recorded(self, app=None):
        return [m['id'] for m in mod_code.read_records(self.library().root)[str(app or self.app)]]

    def switch(self, on):
        library = self.library()
        mod_code.set_code(library, 'a', on, OK)
        mod_library.save(library)

    def test_the_toggles_at_the_start_are_composed_and_recorded_on_success(self):
        extra, writer = self.compose()
        folder = mod_code.folder(self.library())
        self.assertEqual(extra, ['--mod', str(folder)])
        self.assertEqual([p.name for p in (folder / 'exefs').iterdir()], ['a-main.pchtxt'])
        self.switch(False)                                               # after the start: no effect on this run
        writer(str(self.app))
        self.assertEqual(self.recorded(), ['a'])

    def test_nothing_is_recorded_until_the_writer_is_called(self):        # a failed or cancelled build
        self.compose()
        self.assertEqual(mod_code.read_records(self.library().root), {})
        self.assertTrue(mod_code.pending(self.library(), self.app))

    def test_switched_off_then_built_clears_the_record(self):              # O2, the audit's B1
        self.build()
        self.switch(False)
        self.assertEqual((self.build(), self.recorded()), ([], []))
        self.assertFalse(mod_code.pending(self.library(), self.app))

    def test_without_the_update_no_code_and_build_again_stays(self):
        self.assertEqual(self.compose(update=None), ([], None))
        self.assertEqual((mod_code.read_records(self.library().root), mod_code.pending(self.library(), self.app)),
                         ({}, True))

    def test_two_apps_keep_their_own_records(self):
        self.build()
        self.switch(False)
        self.build(app=self.tmp / 'B.app')
        self.assertEqual((self.recorded(), self.recorded(self.tmp / 'B.app')), (['a'], []))

    def test_another_game_has_no_code(self):
        self.assertEqual(self.compose(game='botw'), ([], None))


if __name__ == '__main__':
    unittest.main()
