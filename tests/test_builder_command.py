"""tools/builder/command.py (S1.3): build_argv, checked against build_app.py's own parser. Like test_clone.py and
test_build_mod.py, this imports build_app; parse_args only builds an argparse parser. The paths live in temporary
folders and need not exist: the parser does not open them."""
from pathlib import Path
import tempfile
import unittest

import build_app
from tools.builder import checks_inputs, paths
from tools.builder.checks import BuildPaths
from tools.builder.command import build_argv
from tools.builder.games import BOTW, TOTK
from tools.builder.paths import effective_paths

REPO = Path(__file__).resolve().parents[1]
PYTHON = '/opt/homebrew/bin/python3.14'
NEVER = ('--platform', '--mod', '--install-mod-romfs', '--no-shader-reminder', '--shader-cache')


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.games = Path(tmp.name).resolve() / 'Games'
        self.keys = Path(tmp.name).resolve() / 'keys/prod.keys'
        self.argvs = []

    def g(self, name):
        return self.games / name

    def argv(self, spec, paths_):
        argv = build_argv(spec, paths_, REPO, PYTHON)
        self.argvs.append(argv)
        return argv

    def assertParsesTo(self, spec, paths_):
        """The argv, parsed by build_app.py, means exactly the effective paths on macOS, with no mod."""
        argv = self.argv(spec, paths_)
        args = build_app.parse_args(argv[2:])
        expected = effective_paths(spec, paths_)
        for field in ('game', 'keys', 'update', 'firmware', 'shared_fonts', 'work', 'out'):
            self.assertEqual(getattr(args, field), getattr(expected, field), field)
        self.assertEqual((args.platform, args.mod, args.install_mod_romfs), ('macos', None, False))
        return argv

    def tearDown(self):
        for argv in self.argvs:                           # C3, over every argv the test built
            for flag in NEVER:
                self.assertNotIn(flag, argv)


class ReadmeExamples(Base):
    """The README's Mac examples (README l. 111, 117, 119, 144-149, 211, 217); iOS (l. 127) and mods (l. 164) are out."""

    def test_totk_xci(self):                                                     # l. 111
        self.assertParsesTo(TOTK, BuildPaths(self.g('totk.xci'), self.keys))

    def test_totk_nsp_with_firmware(self):                                       # l. 117
        self.assertParsesTo(TOTK, BuildPaths(self.g('totk.nsp'), self.keys, firmware=self.g('firmware')))

    def test_totk_nsp_with_shared_fonts(self):                                   # l. 119
        fonts = self.g('TOTK_build/totk/extracted/firmware/shared-fonts')
        self.assertParsesTo(TOTK, BuildPaths(self.g('totk.nsp'), self.keys, shared_fonts=fonts))

    def test_totk_xci_with_update(self):                                         # l. 144-149
        for update in ('1.1.2.nsp', '1.1.2.nsz', '1.2.1.nsp', '1.4.3.nsz'):
            with self.subTest(update=update):
                self.assertParsesTo(TOTK, BuildPaths(self.g('totk.xci'), self.keys, update=self.g(update)))

    def test_botw_card_with_shared_fonts(self):                                  # l. 211
        fonts = self.g('TOTK_build/totk/extracted/firmware/shared-fonts')
        self.assertParsesTo(BOTW, BuildPaths(self.g('botw.xci'), self.keys, shared_fonts=fonts))

    def test_botw_base_with_update_and_shared_fonts(self):                       # l. 217
        fonts = self.g('TOTK_build/totk/extracted/firmware/shared-fonts')
        self.assertParsesTo(BOTW, BuildPaths(self.g('botw-base.nsp'), self.keys, update=self.g('botw-1.6.0.nsp'),
                                             shared_fonts=fonts))


class Defaults(Base):
    def test_work_and_out_default_next_to_the_game(self):
        for spec, work, app in ((TOTK, 'TOTK_build', 'Tears of the Kingdom.app'), (BOTW, 'BotW_build', 'Breath of the Wild.app')):
            with self.subTest(spec=spec.id):
                argv = self.assertParsesTo(spec, BuildPaths(self.g('game.xci'), self.keys))
                self.assertEqual(argv[argv.index('--work') + 1], str(self.games / work))
                self.assertEqual(argv[argv.index('-o') + 1], str(self.games / app))
                explicit = self.argv(spec, BuildPaths(self.g('game.xci'), self.keys, work=self.games / work,
                                                      out=self.games / app))
                self.assertEqual(explicit, argv)


class Shape(Base):
    def test_exact_list_for_the_simplest_case(self):
        argv = self.argv(TOTK, BuildPaths(self.g('totk.xci'), self.keys))
        self.assertEqual(argv, [PYTHON, str(REPO / 'build_app.py'), str(self.g('totk.xci')), '-k', str(self.keys),
                                '--work', str(self.games / 'TOTK_build'), '-o', str(self.games / 'Tears of the Kingdom.app')])
        self.assertTrue(all(type(a) is str for a in argv))

    def test_order_of_the_optional_flags(self):
        argv = self.argv(TOTK, BuildPaths(self.g('t.nsp'), self.keys, update=self.g('u.nsz'), firmware=self.g('fw')))
        self.assertEqual(argv[9:], ['--update', str(self.g('u.nsz')), '--firmware', str(self.g('fw'))])

    def test_spaces_and_non_ascii_stay_one_element(self):
        game = self.games / 'Mis Juegos' / 'Zelda – TOTK.xci'
        argv = self.assertParsesTo(TOTK, BuildPaths(game, self.keys))
        self.assertEqual(argv[2], str(game))
        self.assertEqual(build_app.parse_args(argv[2:]).game, game)


class Refusals(Base):
    def test_both_fonts(self):
        with self.assertRaisesRegex(ValueError, '^fonts_both$'):
            build_argv(TOTK, BuildPaths(self.g('t.nsp'), self.keys, firmware=self.g('fw'), shared_fonts=self.g('sf')),
                       REPO, PYTHON)

    def test_output_must_be_an_app(self):
        with self.assertRaisesRegex(ValueError, '^output_not_app$'):
            build_argv(TOTK, BuildPaths(self.g('t.xci'), self.keys, out=self.g('TOTK.ipa')), REPO, PYTHON)
        self.assertParsesTo(TOTK, BuildPaths(self.g('t.xci'), self.keys, out=self.g('TOTK.APP')))


class MovedPathPolicy(unittest.TestCase):
    def test_checks_and_command_share_one_effective_paths(self):
        self.assertIs(checks_inputs.effective_paths, paths.effective_paths)


if __name__ == '__main__':
    unittest.main()
