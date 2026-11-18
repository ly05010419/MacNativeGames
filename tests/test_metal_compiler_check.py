"""The window said this Mac was ready, and Ocarina of Time's build then stopped at "compile-shader-libraries" with
"[RED] no Metal compiler (Xcode -> Settings -> Components: Metal Toolchain)": checks() tested the macOS SDK, never
Apple's offline Metal compiler. It is Apple's, from an Xcode's optional Metal toolchain, and only Ocarina of Time's build
needs it (games/oot/build.py runs build_shader_libs.py, which stops without it): build_app.py's games still build, their
startup shaders going into macOS's Metal cache instead. So the check is found with build_app.py's own search (an Xcode in
/Applications counts while xcode-select points at the CommandLineTools) and fails only for the game it matters for."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import checks, repo  # noqa: E402

FOUND = ('/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/metal',
         'Apple metal version 32023.883 (metalfe-32023.883)', {'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer'})


def checks_with(game, found):
    """(the 'metal' row or None, whether build_app.py's search ran), `found` being metal_compiler()'s answer."""
    with mock.patch.object(repo.build_app(), 'metal_compiler', return_value=found) as search:
        rows = checks.checks(tempfile.gettempdir(), game)
    return next((c for c in rows if c['id'] == 'metal'), None), search.called


def metal_check(game, found):
    """The 'metal' row: a check of the one game that needs a Metal compiler."""
    row, _ = checks_with(game, found)
    assert row is not None, f'no metal check for {game}'
    return row


class Search(unittest.TestCase):
    """The search checks() shares with build_app.py: xcode-select may point at the command line tools, which have no
    Metal compiler, while an Xcode app with the toolchain sits in /Applications."""

    def metal_compiler(self, tmp, xcode_select):
        b = repo.build_app()

        def run(cmd, **kw):
            developer = (kw.get('env') or {}).get('DEVELOPER_DIR', '')
            if cmd[:3] == ['xcrun', '--find', 'metal']:
                found = (Path(developer)/'XcodeDefault.xctoolchain').exists()   # a developer folder of an Xcode app
                out = developer + '/Toolchains/XcodeDefault.xctoolchain/usr/bin/metal' if found else ''
                return mock.Mock(returncode=0 if found else 1, stdout=out if kw.get('text') else out.encode(), stderr='')
            out = 'Apple metal version 32023.883 (metalfe-32023.883)'   # the compiler's --version
            return mock.Mock(returncode=0, stdout=out if kw.get('text') else out.encode(), stderr='')

        def check_output(cmd, **kw):
            return (xcode_select if cmd[0] == 'xcode-select' else '') + '\n'   # mdfind: only the /Applications Xcode

        class Paths:   # the folders developer_dirs() looks in, here the ones the test made
            def __init__(self):
                self.folders = {'/Applications': tmp/'Applications', '/Volumes': tmp/'Volumes'}
            def __call__(self, path):
                return Path(self.folders.get(str(path), str(path)))
            def home(self):
                return tmp/'home'

        with mock.patch.object(b, 'Path', Paths()), mock.patch.object(b, 'xcodes_on_volumes', lambda path: []), \
             mock.patch.dict(os.environ, {k: v for k, v in os.environ.items() if k != 'DEVELOPER_DIR'}, clear=True), \
             mock.patch.object(b.subprocess, 'run', side_effect=run), \
             mock.patch.object(b.subprocess, 'check_output', side_effect=check_output):
            return b.metal_compiler()

    def xcode(self, tmp):
        toolchain = tmp/'Applications/Xcode.app/Contents/Developer/XcodeDefault.xctoolchain'
        toolchain.mkdir(parents=True)
        (tmp/'home').mkdir(exist_ok=True)
        return str(toolchain.parent)   # the Xcode's Contents/Developer

    def test_an_xcode_in_applications_counts_while_xcode_select_names_the_command_line_tools(self):
        with tempfile.TemporaryDirectory() as tmp:
            developer = self.xcode(Path(tmp))
            found = self.metal_compiler(Path(tmp), '/Library/Developer/CommandLineTools')
        self.assertEqual(found[0], developer + '/Toolchains/XcodeDefault.xctoolchain/usr/bin/metal')
        self.assertIn('metal version', found[1])

    def test_without_an_xcode_the_compiler_is_simply_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp)/'home').mkdir()
            self.assertIsNone(self.metal_compiler(Path(tmp), '/Library/Developer/CommandLineTools'))


class TheCheck(unittest.TestCase):
    def test_an_oc_oot_build_is_stopped_before_it_starts(self):
        c = metal_check('oot', None)
        self.assertFalse(c['ok'])
        self.assertIn('xcodebuild -downloadComponent MetalToolchain', c['fix'])
        self.assertIn('App Store', c['fix'])
        self.assertIn('Metal toolchain', c['text'])

    def test_a_game_that_builds_without_it_does_not_even_look(self):
        """The other games need no Metal compiler, and on a Mac without the developer tools every xcrun opens Apple's
        install dialog (compile_free), so their build must not send the checks looking for one."""
        for game in ('totk', 'botw', 'eow', 'acnh', None):
            row, looked = checks_with(game, None)
            self.assertIsNone(row, game)
            self.assertFalse(looked, game)

    def test_the_compiler_found_passes_and_is_named_with_its_version(self):
        c = metal_check('oot', FOUND)
        self.assertTrue(c['ok'])
        self.assertIn('32023.883', c['text'])

    def test_the_words_are_translated(self):
        with mock.patch.dict(os.environ, {'MNG_LANG': 'zh-CN'}):
            c = metal_check('oot', None)
        self.assertIn('时之笛', c['text'])
        self.assertIn('App Store', c['fix'])

    def test_the_terminal_asks_about_the_game_the_files_make(self):
        from tools.builder_jobs import text_cli
        for files, game in ((['game.nup'], 'oot'), (None, None)):
            answer = mock.Mock(files=files, keys=None, json=True)
            with mock.patch('tools.builder_jobs.classify.report', return_value={'items': [], 'plan': {'game': 'oot'}}), \
                 mock.patch('tools.builder_jobs.checks.checks', return_value=[]) as given:
                text_cli.cmd_check(answer)
            self.assertEqual(given.call_args[0][1], game if files else None)

    def test_the_job_and_the_window_hand_the_game_over(self):
        """The checks job takes --game: without it the Metal toolchain could not be Ocarina's alone."""
        done = subprocess.run([sys.executable, '-m', 'tools.builder_jobs', 'checks', '--game', 'oot',
                               '--work', tempfile.gettempdir()], capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(done.returncode, 0, done.stderr)
        metal = next(c for c in json.loads(done.stdout)['checks'] if c['id'] == 'metal')
        self.assertTrue(metal['text'] and metal['fix'])
        self.assertIn('["--game", $0]', (ROOT/'tools/builder_app/Sources/MNGBuilder/Model.swift').read_text())

    def test_the_game_whose_build_stops_without_it_is_the_one_required(self):
        self.assertIn('build_shader_libs.py', (ROOT/'games/oot/build.py').read_text())   # its step stops without a compiler
        self.assertEqual(checks.METAL_GAMES, ('oot',))
        # the build's own block names it from the same search (classify.oot_needs_metal): two places, one answer
        self.assertIn('no_metal_compiler()', (ROOT/'tools/builder_jobs/classify.py').read_text())


if __name__ == '__main__':
    unittest.main()
