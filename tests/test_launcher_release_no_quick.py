"""The players' MNG Launcher has no 快速启动 (user 2026-11-17: "launcher 里 用户版本 去掉 快速启动，包括 打包安装的 快速启动"):
neither the Build page's switch nor the installed card's button, and no build it starts asks for a quick app. A Launcher
made from a checkout keeps it for development and tests (CLAUDE.md: tests start quick), as tools/mng.py does."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SWIFT = ROOT / 'tools/builder_app/Sources/MNGBuilder'


class ReleaseHasNoQuickStart(unittest.TestCase):
    def test_the_release_is_told_by_its_source_archive(self):
        jobs = (SWIFT / 'Jobs.swift').read_text()
        self.assertIn('static var isRelease: Bool { Bundle.main.object(forInfoDictionaryKey: "MNGSourceArchiveSHA256") is String }', jobs)
        make_app = (ROOT / 'tools/builder_app/make_app.py').read_text()
        self.assertIn("plist['MNGSourceArchiveSHA256'] = source_archive(", make_app)   # only a --portable build writes it

    def test_no_quick_game_in_the_release(self):
        model = (SWIFT / 'Model.swift').read_text()
        self.assertIn('static var quickGames: Set<String> { Paths.isRelease ? [] : ["oot"] }', model)
        self.assertIn('let quick = quickStart && Model.quickGames.contains(plan.game ?? "")', model)   # the Build page
        self.assertRegex(model, r'func quickStart\(_ app: InstalledReport\.App\) \{\n\s+guard [^\n]*Model\.quickGames\.contains')

    def test_no_diagnostic_mode_in_the_release(self):
        """User 2026-11-17: "正式下载版 也去掉 诊断模式". Shown in a release only while it is still on (set by an earlier
        Launcher), so it can be turned off and then stays gone."""
        home = (SWIFT / 'HomePage.swift').read_text()
        self.assertIn('if let diagnostics, diagnostics.available, !Paths.isRelease || diagnostics.on {', home)
        self.assertEqual(home.count('Toggle(T.diagnostics'), 1)   # the only place that shows it

    def test_both_places_show_it_only_for_a_quick_game(self):
        """Every place that shows 快速启动 sits under a Model.quickGames check."""
        for name in ('ActionSection.swift', 'HomePage.swift'):
            text = (SWIFT / name).read_text()
            uses = [m.start() for m in re.finditer(r'T\.quickStart\b', text)]
            self.assertTrue(uses, name)
            for at in uses:
                before = text[max(0, at - 400):at]
                self.assertIn('Model.quickGames.contains(', before, f'{name}: 快速启动 shown without the check')


if __name__ == '__main__':
    unittest.main()
