"""Ocarina of Time contributes shaders and uses its lists like every other game (user 2026-11-10: "oot 一定要能 正确贡献 正确使用",
"要统一").

Its own build.py put the warm-up lists (pso_title.tsv, pso_manifest.tsv) only into the shader seed, while the Mac runtime
(warm-up, the user list, the contribution filter) and the launcher's prune read them beside the shader map in build/, where
build_app.py resolve_lists writes them for every game it packages: every pipeline the game drew went into the contribution
file (3,091 rows, 3,040 of them in the list). Its contributions also named Tears of the Kingdom's form and title, Breath of
the Wild's file, and its iPhone / iPad reminder looked for Tears of the Kingdom on the device.
"""
import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OOT = ROOT / 'games/oot'
sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
import collect_shader_issues  # noqa: E402


def function(source, name):
    """The first definition of name (its line holds the body's colon or brace) up to the next top-level line."""
    for found in re.finditer(re.escape(name), source):
        start = source.rindex('\n', 0, found.start()) + 1
        line = source[start:source.index('\n', found.start())]
        if line.rstrip().endswith((':', '{')) or ('{' in line and not line.rstrip().endswith(';')):
            end = re.search(r'\n(?=\S)', source[found.end():])
            return source[start:found.end() + end.start()] if end else source[start:]
    raise AssertionError(f'no definition of {name}')


def oot_block(header):
    """The #if defined(GAME_OOT) block of a game_version.h."""
    start = header.index('#if defined(GAME_OOT)')
    return header[start:header.index('#elif', start)]


class Packaging(unittest.TestCase):
    source = (OOT / 'build.py').read_text()

    def test_lists_go_beside_the_shader_map(self):
        body = function(self.source, 'def package_app')
        shader_map = body.index("clone_file(tree/'build/shader-map.tsv',data/'build/shader-map.tsv')")
        self.assertLess(shader_map, body.index('resolve_lists(tree,data,'))
        resolve = function(self.source, 'def resolve_lists')
        self.assertIn("'resolve',seed,shared,*lists", resolve)
        self.assertIn("lists=(data/'build/pso_manifest.tsv',data/'build/pso_title.tsv')", resolve)   # shader_list.py resolve order

    def test_the_fast_update_takes_the_same_path(self):
        self.assertIn('package_app(work,tree,app,', function(self.source, 'def update_runtime'))

    def test_contribution_folder_as_build_app(self):
        self.assertIn("info['MNGContributionFolder']=str(contrib_folder())", self.source)

    def test_ios_build_installs_its_own_reminder(self):
        self.assertIn("ios/tools/shader_contrib_watch.py','install','--store',contrib_folder()", self.source)


class Route(unittest.TestCase):
    def test_game_version_names_the_other_games_form(self):
        header = (OOT / 'runtime/game_version.h').read_text()
        block = oot_block(header)
        self.assertIn('#define GAME_CONTRIB_TEMPLATE "shader-list-other.yml"', block)
        self.assertIn('#define GAME_CONTRIB_TITLE "Shader list (Ocarina of Time):"', block)
        self.assertTrue((ROOT / '.github/ISSUE_TEMPLATE/shader-list-other.yml').is_file())
        for path in (OOT / 'runtime/game_version.h', OOT / 'app/launcher.m'):   # the reminder is on, as for every game
            self.assertNotIn('GAME_SHADER_CONTRIB_SUPPORTED', path.read_text())

    def test_launcher_file_is_the_games_own(self):
        body = function((OOT / 'app/launcher.m').read_text(), 'static NSString *contrib_file')
        self.assertIn('@"shader-list-contribution-%s.txt", TOTK_GAME_DIR', body)
        self.assertIn('!strcmp(GAME_FOLDER, "BotW") ? @"shader-list-contribution-botw.txt"', body)

    def test_merge_tool_takes_its_titles(self):
        titled = collect_shader_issues.titled_game
        self.assertEqual(titled({'title': 'Shader list (Ocarina of Time): 32 shaders from Mac (OcarinaOfTime 131072)', 'body': ''}), 'oot')
        self.assertEqual(titled({'title': 'Shader list: 3091 shaders from Mac (OcarinaOfTime 131072)', 'body': ''}), 'oot')   # an older build's

    def test_ios_reminder_is_the_games_own(self):
        spec = importlib.util.spec_from_file_location('oot_shader_contrib_watch', OOT / 'ios/tools/shader_contrib_watch.py')
        watch = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(watch)
        totk = {'name': 'TOTK', 'bundleIdentifier': 'org.macnativegames.totk.csl9l993h3'}
        oot = {'name': 'OcarinaOfTime', 'bundleIdentifier': 'org.macnativegames.oot.csl9l993h3'}
        self.assertEqual(watch.game_bundle({'result': {'apps': [totk, oot]}}), oot['bundleIdentifier'])
        self.assertIsNone(watch.game_bundle({'result': {'apps': [totk]}}))
        self.assertEqual(watch.MERGED, 'shader-list-contribution-oot.txt')
        self.assertIn('template=shader-list-other.yml', watch.ISSUE_URL)
        self.assertNotEqual(watch.LABEL, 'org.macnativegames.totk.ios-shader-contrib')   # would replace Tears of the Kingdom's agent
        issue = {'title': f'{watch.GAME_TITLE} 120 shaders from iPhone (OcarinaOfTime 131072)',
                 'body': watch.issue_body('iPhone', 'https://gist.githubusercontent.com/a/b/raw/x', 'OcarinaOfTime 131072')}
        self.assertEqual(collect_shader_issues.titled_game(issue), 'oot')


PROBE = r'''
#import <Foundation/Foundation.h>
#include "totk_shader_contrib.h"
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 4) return 2;
        NSUInteger moved = totk_contrib_prune(@(argv[1]), @(argv[2]), @(argv[3]));
        printf("%lu\n", (unsigned long)moved);
    }
    return 0;
}
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class Prune(unittest.TestCase):
    """The launcher's prune (Ocarina of Time's totk_shader_contrib.h) against the App's lists: what build/pso_manifest.tsv
    holds leaves the file for its done file; a folder without the lists (the layout before) moves nothing."""

    def run_prune(self, tmp, lists):
        probe = tmp / 'prune'
        (tmp / 'prune.m').write_text(PROBE)
        built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', f'-I{OOT}/third_party/d2r_gfx/gfx/nvn', str(tmp / 'prune.m'),
                                '-framework', 'Foundation', '-o', str(probe)], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr[-600:])
        seed = tmp / 'seed'; seed.mkdir(exist_ok=True); (seed / 'seed-manifest.json').write_text('{}\n')
        contrib = tmp / 'shader-list-contribution-oot.txt'
        contrib.write_text('# header\nR\tabc1\nR\tabc2\nT\tabc2\t1\nR\tabc3\n')
        run = subprocess.run([str(probe), str(contrib), str(seed), str(lists)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        return int(run.stdout), contrib.read_text()

    def test_rows_the_build_lists_hold_leave_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp); build = tmp / 'build'; build.mkdir()
            (build / 'pso_manifest.tsv').write_text('# list\nR\tabc1\nR\tabc2\nT\tabc2\t1\nS\tdef\n')
            (build / 'pso_title.tsv').write_text('# title\n')
            moved, left = self.run_prune(tmp, build)
            self.assertEqual(moved, 3)
            self.assertEqual([l for l in left.splitlines() if not l.startswith('#')], ['R\tabc3'])

    def test_without_the_lists_nothing_leaves(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp); build = tmp / 'build'; build.mkdir()
            moved, _ = self.run_prune(tmp, build)
            self.assertEqual(moved, 0)


if __name__ == '__main__':
    unittest.main()
