"""E17 S17.1: native 4K as the Settings tab's picture (start_options), the build that follows it (command and
controller_start, build_app.py's own parser reading the flag), Play refusing a native 4K app in the texture pack's
8 GB mode, and the storage save moved into config.py. Fake apps and folders in temporary directories."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import build_app
from tools.builder import controller_start, mod_code
from tools.builder.checks import BuildPaths
from tools.builder.command import build_argv
from tools.builder.config import SavedPaths, Settings, SettingsStore
from tools.builder.games import BOTW, TOTK
from tools.builder.jobs import InlineJobs
from tools.builder.start_options import FOUR_K, StartPaths, build_native_4k, choose, play, read_state
from test_builder_controller import Base as ControllerBase
from test_builder_start_options import Base, Runner

REPO = Path(__file__).resolve().parents[1]


class Picture4K(Base):
    def four_k(self, app):
        (app / FOUR_K).mkdir(parents=True)
        return app

    def test_the_choice_is_the_panels_native_4k_and_1_4_3s_only(self):
        app = self.app('v143')
        state = choose(TOTK, app, self.paths, 'picture', 'native-4k')
        self.assertEqual(((self.paths.data / 'TOTK/graphics-output.txt').read_text(), state.choices['picture']),
                         ('5\n', 'native-4k'))
        botw = read_state(BOTW, self.app('botw190', name='Breath of the Wild.app'), self.paths)
        self.assertNotIn('native-4k', next(o for o in botw.options if o.id == 'picture').choices)

    def test_when_the_app_must_be_built_again(self):
        app = self.app('v143')
        self.assertEqual(read_state(TOTK, app, self.paths).rebuild, '')
        choose(TOTK, app, self.paths, 'picture', 'native-4k')
        self.assertEqual(read_state(TOTK, app, self.paths).rebuild, 'picture_4k')     # chosen, the app has none
        held = read_state(TOTK, app, self.paths, heaps=True)
        self.assertEqual((held.rebuild, len(held.notes)), ('', 1))              # the pack's heaps: none built, said
        self.four_k(app)
        self.assertEqual(read_state(TOTK, app, self.paths).rebuild, '')
        self.assertEqual(read_state(TOTK, app, self.paths, heaps=True).rebuild, 'native_4k_8gb')
        choose(TOTK, app, self.paths, 'picture', 'auto')
        self.assertEqual(read_state(TOTK, app, self.paths, heaps=True).rebuild, 'native_4k_8gb')   # whatever chosen

    def test_what_the_build_passes(self):
        app, fields = self.app('v143'), {'update': '/u.nsp'}
        self.assertIs(build_native_4k(TOTK, fields, self.paths, False), False)          # auto: no 4K
        choose(TOTK, app, self.paths, 'picture', 'native-4k')
        self.assertIs(build_native_4k(TOTK, fields, self.paths, False), True)
        self.assertIs(build_native_4k(TOTK, fields, self.paths, True), False)           # the pack's heap table
        self.assertIsNone(build_native_4k(TOTK, {}, self.paths, False))                 # no update: no flag
        self.assertIsNone(build_native_4k(BOTW, fields, self.paths, False))

    def test_play_refuses_a_native_4k_app_in_the_8_gb_mode(self):
        app, runner = self.four_k(self.app('v143')), Runner()
        result = play(runner, TOTK, app, self.paths, 'yes', {'TOTK_DRAM_8GB': '1'})
        self.assertEqual((result.ok, result.reason, runner.calls), (False, 'native_4k_8gb', []))
        self.assertTrue(play(runner, TOTK, app, self.paths, 'yes').ok)                   # without the pack's mode


class Argv(unittest.TestCase):
    def test_the_flag_as_build_app_reads_it(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name).resolve()
        paths = BuildPaths(game=root / 'g.xci', keys=root / 'prod.keys', update=root / 'u.nsp')
        for native, flags in ((None, []), (True, ['--native-4k']), (False, ['--no-native-4k'])):
            with self.subTest(native=native):
                argv = build_argv(TOTK, paths, REPO, '/py', native_4k=native)
                self.assertEqual([a for a in argv if a.endswith('native-4k')], flags)
                self.assertEqual(build_app.parse_args(argv[2:]).native_4k, native)


class Settings4K(ControllerBase):
    def setUp(self):
        super().setUp()
        self.load_jobs = self.ctl.load_jobs = InlineJobs()
        self.ctl.start.paths = StartPaths(self.tmp / 'Builder/start-options.json', self.tmp / 'data')
        self.ctl.select_game('totk')
        self.ctl.set_paths(game=str(self.tmp / 'g.xci'), keys=str(self.tmp / 'k'), update=str(self.tmp / 'u.nsp'))
        self.events()
        self.app = (self.tmp / 'Tears of the Kingdom.app').resolve()       # effective_paths resolves
        (self.app / 'Contents/Resources/totk/extracted/v143').mkdir(parents=True)

    def test_the_read_knows_whether_the_app_has_the_packs_heaps(self):
        (self.app / FOUR_K).mkdir(parents=True)
        with patch.object(controller_start.controller_textures, 'play_env', return_value={'TOTK_DRAM_8GB': '1'}):
            self.ctl.start.state()
            self.load_jobs.run_next()
        self.ctl.tick()
        [event] = self.events()
        self.assertEqual(event.state.rebuild, 'native_4k_8gb')

    def test_a_build_carrying_the_packs_heap_table_gets_no_native_4k(self):
        choose(TOTK, self.app, self.ctl.start.paths, 'picture', 'native-4k')
        native, code = self.ctl.start.native_4k_for(self.ctl._snapshot()), self.tmp / 'code'
        self.assertEqual((native(None), native(code)), (True, True))           # no folder; a folder without one
        (code / 'romfs' / mod_code.HEAPDEF).parent.mkdir(parents=True)
        (code / 'romfs' / mod_code.HEAPDEF).write_bytes(b'heaps')
        self.assertIs(native(code), False)


class Storage(unittest.TestCase):
    def test_work_and_other_names_are_saved_and_a_path_field_is_refused(self):     # moved from the controller
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        store = SettingsStore(Path(tmp.name) / 'settings.json', clock=None)
        store.save(Settings(paths={'totk': SavedPaths(game='/g.xci', work='/old')}))
        entry = SavedPaths(game='/g.xci', work='/w', extra={'shader_cache': '/s'})
        self.assertEqual(store.save_storage('totk', {'work': '/w', 'shader_cache': '/s'}), ('totk', entry))
        self.assertEqual(store.load().settings.paths['totk'], entry)
        self.assertRaises(ValueError, store.save_storage, 'totk', {'keys': '/k'})


if __name__ == '__main__':
    unittest.main()
