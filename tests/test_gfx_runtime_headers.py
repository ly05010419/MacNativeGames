"""The shared graphics layer reaches a game's runtime headers by name, through the include path each game's build sets
(Mac Makefile GFX_FLAGS -Iruntime, iOS project.yml -I../runtime), never by a path relative to its own files: games/oot's
third_party/d2r_gfx is a symlink to games/totk's, so "../../../../runtime/x.h" named games/totk/runtime on the Mac only
through the link, and in the work-folder copy (games/oot/build.py sync_tree copies the layer as its contents) it named
OOT's runtime, which lacked the header: the iOS build stopped (2026-11-13, totk_prompt_icons.m, input_prompts.h).
Every game that builds the layer carries each runtime header the layer includes."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYER = ROOT / 'games/totk/third_party/d2r_gfx/gfx'
GAMES = ('totk', 'oot')   # the games whose builds compile this layer (Bananza and Pokopia build with games/totk)
INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s*"([^"]+)"', re.M)


def layer_sources():
    return [p for p in LAYER.rglob('*') if p.suffix in ('.c', '.m', '.h', '.inc')]


class RuntimeHeaders(unittest.TestCase):
    def test_no_relative_path_into_a_game_runtime(self):
        for src in layer_sources():
            for name in INCLUDE.findall(src.read_text(errors='replace')):
                with self.subTest(file=src.name, include=name):
                    self.assertNotRegex(name, r'(^|/)\.\./(\.\./)*runtime/', 'include it by name; the build puts runtime/ on the path')

    def test_every_game_has_the_runtime_headers_the_layer_includes(self):
        here = {p.name for p in LAYER.rglob('*.h')} | {p.name for p in LAYER.rglob('*.inc')}
        wanted = set()
        for src in layer_sources():
            for name in INCLUDE.findall(src.read_text(errors='replace')):
                if '/' not in name and name not in here and (ROOT / 'games/totk/runtime' / name).is_file():
                    wanted.add(name)
        self.assertIn('input_prompts.h', wanted)
        for game in GAMES:
            runtime = ROOT / 'games' / game / 'runtime'
            for name in sorted(wanted):
                with self.subTest(game=game, header=name):
                    self.assertTrue((runtime / name).is_file(), f'games/{game}/runtime/{name}')
                    for dep in INCLUDE.findall((runtime / name).read_text()):   # and what those include from the runtime
                        if '/' not in dep and (ROOT / 'games/totk/runtime' / dep).is_file():
                            self.assertTrue((runtime / dep).is_file(), f'games/{game}/runtime/{dep} (from {name})')

    def test_the_mac_builds_put_the_runtime_on_the_include_path(self):
        for game in GAMES:
            make = (ROOT / 'games' / game / 'Makefile').read_text()
            flags = re.search(r'^GFX_FLAGS\s*:=\s*(.+)$', make, re.M)
            with self.subTest(game=game):
                self.assertIn(' -Iruntime', flags.group(1))
                self.assertIn('-I../runtime', (ROOT / 'games' / game / 'ios/project.yml').read_text())


if __name__ == '__main__':
    unittest.main()
