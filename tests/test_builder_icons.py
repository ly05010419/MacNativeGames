"""E6 S6.3: the prompt icons that follow the player's layout. icons_plan (which button each glyph names, B4),
icons_env (the pinned pip, the venv made aside and checked, B2-B3; processes faked, no network), icons_mod (the seeded
size table, B1; building with a faked generator; installing in place with the reserved id, kept last, B5)."""
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from test_builder_mod_library import REPO, Base as LibraryBase
from tools.builder import cancellable, controls_file as cf, icons_env, icons_mod, icons_plan, mod_add, mod_library


def inputs(raw=b'', by_label=False):
    return cf.layout(cf.parse(raw), by_label)[0]


def drawn(entries):
    return {cp: rest for cp, *rest in entries}


class Plan(unittest.TestCase):
    def test_by_position_the_switch_letters_name_other_xbox_buttons(self):
        p = drawn(icons_plan.plan(inputs()))
        self.assertEqual([p[cp] for cp in (0xE040, 0xE041, 0xE042, 0xE043)],
                         [['face', 'B'], ['face', 'A'], ['face', 'Y'], ['face', 'X']])
        self.assertEqual((p[0xE044], p[0xE047], p[0xE04B], p[0xE04C], p[0xE056]),
                         (['bumper', 'LB'], ['trigger', 'RT'], ['menu'], ['view'], ['face', 'L3']))
        self.assertEqual((p[0xE060], p[0xE064], p[0xE049]),
                         (['dpad', ['up']], ['dpad', ['up', 'down']], ['dpad', ['up', 'down', 'left', 'right']]))

    def test_xbox_letters_jump_on_lb_none_and_shared(self):
        p = drawn(icons_plan.plan(cf.xbox_letters(inputs())))
        self.assertEqual([p[cp][1] for cp in (0xE040, 0xE041, 0xE042, 0xE043)], ['A', 'B', 'X', 'Y'])
        self.assertEqual(drawn(icons_plan.plan(cf.swap(inputs(), 'x', 'l1')))[0xE042], ['bumper', 'LB'])
        self.assertEqual(drawn(icons_plan.plan(inputs(b'north = none\n')))[0xE042], ['none'])
        two = inputs(b'l1 = x\n')                                       # Jump on Y and on LB
        self.assertEqual((drawn(icons_plan.plan(two))[0xE042], icons_plan.shared(two)), (['face', 'Y'], ['x']))

    def test_the_d_pad_glyphs_light_real_arms_or_show_labels(self):
        turned = cf.swap(cf.swap(inputs(), 'up', 'left'), 'down', 'right')   # up on the left arm, down on the right
        self.assertEqual(drawn(icons_plan.plan(turned))[0xE064], ['dpad', ['left', 'right']])
        moved = cf.swap(inputs(), 'up', 'l1')
        p = drawn(icons_plan.plan(moved))
        self.assertEqual((p[0xE064], p[0xE049][0]), (['labels', ['LB', '↓']], 'labels'))
        self.assertNotIn(0xE050, p)                                     # stick movements stay


class Env(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.builder, self.calls = Path(tmp) / 'Builder', []

    def fake(self, fail=None, version=icons_env.VERSION):
        def run(argv, cancel, timeout_s, env=None):
            argv = [str(a) for a in argv]
            self.calls.append((argv, env))
            if argv[-2:] == ['venv', argv[-1]] or '-m' in argv and argv[argv.index('-m') + 1] == 'venv':
                (Path(argv[-1]) / 'bin').mkdir(parents=True)
                (Path(argv[-1]) / 'bin/python').write_text('')
                return (1 if fail == 'venv' else 0), b'', 'no venv'
            if 'pip' in argv:
                return (1 if fail == 'pip' else 0), b'', 'hash mismatch'
            return 0, version.encode() + b'\n', ''
        return patch.object(icons_env.cancellable, 'run', side_effect=run)

    def test_the_exact_pip_line_and_environment(self):                   # B2
        with patch.dict('os.environ', PIP_INDEX_URL='https://elsewhere', PIP_EXTRA_INDEX_URL='x'), self.fake():
            icons_env.install(self.builder)
        argv, env = next(c for c in self.calls if 'pip' in c[0])
        self.assertEqual(argv[1:], ['-I', '-m', 'pip', 'install', '--isolated', '--disable-pip-version-check',
                                    '--no-input', '--index-url', 'https://pypi.org/simple', '--no-deps',
                                    '--only-binary=:all:', '--require-hashes', '-r', argv[-1]])
        self.assertTrue(argv[0].endswith('icons-venv.partial/bin/python'))     # never bin/pip
        self.assertFalse(any(k.startswith('PIP_') for k in env))
        self.assertEqual(icons_env.requirements().count('--hash=sha256:'), 3)
        self.assertTrue(icons_env.requirements().startswith('fonttools==4.67.0 '))

    def test_made_aside_moved_and_checked(self):                         # B3
        with self.fake():
            self.assertEqual(icons_env.install(self.builder), icons_env.venv(self.builder))
            self.assertEqual(icons_env.status(self.builder), 'ready')
        check = self.calls[-1][0]
        self.assertEqual(check[1:3], ['-I', '-c'])
        self.assertFalse((self.builder / 'icons-venv.partial').exists())

    def test_a_venv_that_fails_its_check_after_the_move_is_removed(self):  # B3
        real = icons_env._check
        with self.fake(), patch.object(icons_env, '_check',
                                       side_effect=lambda folder, cancel=None: False if folder.name == 'icons-venv'
                                       else real(folder, cancel)):
            with self.assertRaises(icons_env.EnvError) as caught:
                icons_env.install(self.builder)
        self.assertEqual((caught.exception.reason, list(self.builder.iterdir())), ('check', []))

    def test_a_failure_leaves_nothing_and_a_wrong_version_is_broken(self):
        with self.fake(fail='pip'):
            with self.assertRaises(icons_env.EnvError) as caught:
                icons_env.install(self.builder)
        self.assertEqual((caught.exception.reason, list(self.builder.iterdir())), ('pip', []))
        icons_env.venv(self.builder).mkdir(parents=True)
        with self.fake(version='4.0.0'):
            self.assertEqual(icons_env.status(self.builder), 'broken')
        self.assertEqual(icons_env.status(self.builder / 'none'), 'missing')


class Mod(LibraryBase):
    def font_mod(self, name, table=None):
        mod_id = self.add(name, {'UI/C.bfres': b'modded c'})
        romfs = self.library.path(mod_id) / 'romfs'
        (romfs / 'Font').mkdir()
        (romfs / 'Font/Font.Nin_NX_NVN.bfarc.zs').write_bytes(b'font')
        if table is not None:
            (romfs / 'System/Resource').mkdir(parents=True)
            (romfs / 'System/Resource/T.rsizetable.zs').write_bytes(table)
        return mod_id

    def work(self):
        work = self.tmp / 'work'
        (work / 'romfs/UI').mkdir(parents=True)
        (work / 'romfs/UI/C.bfres').write_bytes(b'icons')
        return work

    def test_seeded_table_font_mods_and_status(self):                    # B1
        a = self.font_mod('A', b'table a')
        b = self.font_mod('B', b'table b')
        for mod_id in (a, b):
            mod_library.set_enabled(self.library, mod_id, True)
        self.assertEqual(icons_mod.seed_table(self.library, 'System/Resource/T.rsizetable.zs'), b'table b')
        self.assertEqual(icons_mod.font_mods(self.library), [a, b])
        self.assertEqual(icons_mod.status(self.library, {}), 'none')

    def test_installed_in_place_reserved_last_and_recorded(self):        # B5
        other = self.font_mod('xbox icons')
        mod_library.set_enabled(self.library, other, True)
        v = icons_mod.install(REPO, self.app, 'v143', self.library, self.work(), {'controls': 'h1'}, (other,))
        self.assertTrue(v.valid)
        state = mod_library.open_library(self.data / 'TOTK', 'v143', self.load).state
        self.assertEqual((state['order'][-1], state['enabled'], state['icons']),
                         (mod_library.ICONS, [mod_library.ICONS], {'controls': 'h1'}))
        later = self.add('Later', {'Pack/A.pack.zs': b'later'})
        bad = self.tmp / 'bad'
        (bad / 'romfs/Elsewhere').mkdir(parents=True)                  # adds only: not valid
        (bad / 'romfs/Elsewhere/x').write_bytes(b'x')
        enabled = list(self.library.state['enabled'])
        self.assertFalse(icons_mod.install(REPO, self.app, 'v143', self.library, bad, {'controls': 'bad'},
                                           (later,)).valid)
        self.assertEqual((self.library.state['enabled'], self.library.state['icons']),
                         ([m for m in enabled if m != mod_library.ICONS], {'controls': 'h1'}))
        self.assertEqual(self.library.state['order'][-1], mod_library.ICONS)    # kept last after an Add
        icons_mod.install(REPO, self.app, 'v143', self.library, self.work(), {'controls': 'h2'})
        self.assertEqual(self.library.state['order'].count(mod_library.ICONS), 1)
        self.assertEqual(sorted(p.name for p in self.library.root.iterdir() if p.name.startswith('builder')),
                         [mod_library.ICONS])                            # no .new or .old left
        self.assertEqual(icons_mod.status(self.library, {'controls': 'h2'}), 'current')
        self.assertNotEqual(mod_add.add(self.runner, REPO, self.app, 'v143', self.library,
                                        self.source('builder button icons', {'Pack/A.pack.zs': b'x'}))[0],
                            mod_library.ICONS)
        self.assertIn(later, self.library.state['order'])

    def test_build_seeds_runs_and_cleans(self):
        seen = {}

        def run(argv, cancel, timeout_s, env=None):
            romfs = Path(argv[3])
            seen['seed'] = (romfs / 'System/Resource/T.rsizetable.zs').read_bytes()
            (romfs / 'Font').mkdir()
            (romfs / 'Font/Font.Nin_NX_NVN.bfarc.zs').write_bytes(b'drawn')
            (romfs / 'manifest.tsv').write_text('script')
            return 0, b'', ''
        mod_library.set_enabled(self.library, self.font_mod('A', b'table a'), True)
        with patch.object(icons_mod, 'table_name', return_value='System/Resource/T.rsizetable.zs'), \
                patch.object(icons_mod.cancellable, 'run', side_effect=run):
            work, table = icons_mod.build(REPO, self.app, 'v143', self.library, inputs(), self.tmp / 'Builder')
        self.assertEqual((seen['seed'], table, (work / 'romfs/manifest.tsv').exists()), (b'table a', b'table a', False))
        with patch.object(icons_mod, 'table_name', return_value='System/Resource/T.rsizetable.zs'), \
                patch.object(icons_mod.cancellable, 'run', side_effect=cancellable.Stopped('cancelled')):
            self.assertRaises(icons_mod.BuildError, icons_mod.build, REPO, self.app, 'v143', self.library,
                              inputs(), self.tmp / 'Builder')
        self.assertFalse((self.library.root / '.icons-build').exists())


if __name__ == '__main__':
    unittest.main()
