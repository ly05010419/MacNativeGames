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
        self.assertLess(shader_map, body.index("clone_file(manifest,data/'build/pso_manifest.tsv')"))
        self.assertIn("clone_file(title,data/'build/pso_title.tsv')", body)
        resolve = function(self.source, 'def resolve_lists')
        self.assertIn("'resolve',seed,shared,manifest,title", resolve)   # shader_list.py resolve order

    def test_the_pipeline_archive_takes_the_apps_lists(self):
        # 2026-11-11: a fast update kept the seed's lists of 11-09 (2,957 render rows) for the archive while the App's
        # build/ lists, resolved from the current shared list, had 7,516: 4,559 listed pipelines were built at first start.
        body = function(self.source, 'def package_app')
        resolved = body.index("lists=resolve_lists(tree,tree/'build/warm-lists',")
        self.assertLess(resolved, body.index('compute_pipeline_archive(work,tree,lists)'))
        archive = function(self.source, 'def compute_pipeline_archive')
        self.assertNotIn("seed/'pso_manifest.tsv'", archive)
        self.assertNotIn("seed/'pso_title.tsv'", archive)

    def test_the_fast_update_takes_the_same_path(self):
        self.assertIn('package_app(work,tree,app,', function(self.source, 'def update_runtime'))

    def test_the_app_has_the_games_icon(self):
        # build/AppIcon.icns was made but never put in the app (2026-11-10: the desktop app had no icon)
        body = function(self.source, 'def package_app')
        self.assertIn("clone_file(icon,res/'AppIcon.icns')", body)
        self.assertIn("info['CFBundleIconFile']='AppIcon'", body)

    def test_contribution_folder_as_build_app(self):
        self.assertIn("info['MNGContributionFolder']=str(contrib_folder())", self.source)

    def test_ios_build_takes_the_old_device_reminder_off(self):
        # the devices contribute over HTTPS on their own since 2026-11-10; the agent an older build installed goes
        self.assertIn("DEVICE_REMINDER_LABEL='org.macnativegames.oot.ios-shader-contrib'", self.source)
        self.assertIn('remove_device_reminder()', function(self.source, 'def build(args'))
        self.assertNotIn('shader_contrib_watch.py\',\'install', self.source)


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

    def test_uploads_under_its_own_name(self):
        upload = (OOT / 'third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h').read_text()
        self.assertIn('#define GAME_INBOX_ID "oot"', upload)
        self.assertFalse((OOT / 'ios/tools/shader_contrib_watch.py').exists())


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
