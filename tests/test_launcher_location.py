"""MNG Launcher step 3: the player chooses where a new game goes, no default (user 2026-11-17: "launcher 打包的时候 第三步
让用户 自己选择位置 不要有默认位置，用户选一个位置 自动生成app目录和build目录 然后打包"); the app goes into
<location>/app, the build's work files into <location>/build/<game>. And the window shows the Launcher's version at its
bottom right (same day: "launcher 右下角 显示 版本号")."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder_jobs import build_job, repo  # noqa: E402

SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'


class NoDefaultLocation(unittest.TestCase):
    def test_the_model_has_no_default_and_derives_app_and_build(self):
        model = (SWIFT / 'Model.swift').read_text()
        self.assertIn('location = defaults.string(forKey: "location") ?? ""', model)
        self.assertNotIn('"/Applications"', model)   # the old default apps folder
        self.assertIn('appendingPathComponent("app")', model)
        self.assertIn('appendingPathComponent("build")', model)
        self.assertIn('+ (plan?.game != nil && needsLocation ? [T.locationNeeded] : [])', model)   # no build without one
        self.assertIn('for folder in [outputDir, workRoot] where !folder.isEmpty', model)   # both made at the start

    def test_step_three_asks_for_it(self):
        content = (SWIFT / 'ContentView.swift').read_text()
        self.assertIn('LocationSection()', content)
        self.assertNotIn('FoldersSection', content)
        self.assertIn('T.updatesInPlace(app)', content)   # an installed game is updated where it is

    def test_a_chosen_location_makes_app_and_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            location = Path(tmp) / 'Games'
            request = {'plan': {'game': 'totk', 'base': '/x/base.nsp', 'keys': '/x/prod.keys'},
                       'output_dir': str(location / 'app'), 'work_root': str(location / 'build'), 'platform': 'macos'}
            script, argv, work, out, kind, game = build_job.command(request)
            self.assertEqual(work, location / 'build' / 'totk')
            self.assertEqual(out, location / 'app' / f'{repo.GAMES["totk"]}.app')
            self.assertEqual(argv[argv.index('--work') + 1], str(work))
            self.assertEqual(argv[argv.index('-o') + 1], str(out))
            self.assertEqual(kind, 'full')


class Version(unittest.TestCase):
    def test_the_window_shows_the_version_bottom_right(self):
        app = (SWIFT / 'App.swift').read_text()
        self.assertIn('.overlay(alignment: .bottomTrailing) { VersionLabel() }', app)
        self.assertIn('info["CFBundleShortVersionString"]', app)
        self.assertIn('"MNG Launcher \\(v)"', (SWIFT / 'Strings.swift').read_text())


if __name__ == '__main__':
    unittest.main()
