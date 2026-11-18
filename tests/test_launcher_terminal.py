"""MNG Launcher's terminal page (tools/builder_app TerminalTab.swift; user 2026-11-13: an AI helper inside the Launcher
that looks at why a build failed, fixes it and goes on): SwiftTerm pinned to one release, its licences carried in the
app (make_app.py licenses), the AI asked to work through tools/mng.py and never about keys, and the ways in (the games
page's Terminal button, a failed build's Ask the AI)."""
import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'tools/builder_app'
SWIFT = APP / 'Sources/MNGBuilder'


def make_app():
    spec = importlib.util.spec_from_file_location('make_app_under_test', APP / 'make_app.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Pin(unittest.TestCase):
    def test_swiftterm_is_one_exact_release(self):
        package = (APP / 'Package.swift').read_text()
        # v1.20.0: the newest the Command Line Tools' Swift 6.2 compiles (v1.99.2 needs Swift 6.3, 2026-11-15)
        self.assertIn('.package(url: "https://github.com/migueldeicaza/SwiftTerm", exact: "1.20.0")', package)
        self.assertIn('.product(name: "SwiftTerm", package: "SwiftTerm")', package)
        resolved = APP / 'Package.resolved'
        self.assertTrue(resolved.is_file(), 'Package.resolved pins SwiftTerm and what it brings')
        self.assertIn('5d14406844143538cd8f8851d2d8a67c1fe443e5', resolved.read_text())   # the v1.20.0 tag's commit

    def test_the_build_runs_inside_this_mac_s_sandbox(self):
        import inspect
        self.assertEqual(inspect.getsource(make_app().binary).count("'--disable-sandbox'"), 2)   # the build and --show-bin-path


class Licences(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.scratch = Path(self.tmp.name) / 'scratch'
        self.resources = Path(self.tmp.name) / 'Resources'

    def tearDown(self):
        self.tmp.cleanup()

    def test_every_checked_out_package_s_licence_goes_into_the_app(self):
        for name, files in (('SwiftTerm', ('LICENSE',)), ('swift-png', ('LICENSE', 'NOTICE.md'))):
            (self.scratch / 'checkouts' / name).mkdir(parents=True)
            for f in files:
                (self.scratch / 'checkouts' / name / f).write_text(f'{name} {f}')
        (self.scratch / 'checkouts/swift-png/Sources').mkdir()
        self.assertEqual(make_app().licenses(self.scratch, self.resources), ['SwiftTerm', 'swift-png'])
        self.assertEqual((self.resources / 'licenses/swift-png/NOTICE.md').read_text(), 'swift-png NOTICE.md')
        self.assertTrue((self.resources / 'licenses/SwiftTerm/LICENSE').is_file())

    def test_a_package_without_a_licence_stops_the_app(self):
        (self.scratch / 'checkouts/unknown').mkdir(parents=True)
        (self.scratch / 'checkouts/unknown/README.md').write_text('no licence')
        with self.assertRaises(SystemExit):
            make_app().licenses(self.scratch, self.resources)

    def test_make_app_puts_them_in(self):
        self.assertIn("licenses(a.scratch.expanduser(), stage / 'Contents/Resources')", (APP / 'make_app.py').read_text())


class Helper(unittest.TestCase):
    def test_the_ai_works_through_the_launcher_s_engine(self):
        text = (SWIFT / 'TerminalTab.swift').read_text()
        prompt = text[text.index('static func prompt'):text.index('static func prPrompt')]
        for words in ('tools/mng.py status', 'tools/mng.py check', 'docs/install.md'):
            self.assertIn(words, prompt)
        self.assertNotIn('密钥', prompt)   # user 2026-11-15: no sentence about keys or game files in the prompt

    def test_the_pull_request_goes_to_the_project_and_is_shown_before_it_is_sent(self):
        # user 2026-11-15: "增加 一个按钮 叫 提交pr给作者 把最近修复的 bug 用pr的方法提交给我"
        text = (SWIFT / 'TerminalTab.swift').read_text()
        pr = text[text.index('static func prPrompt'):text.index('static func command')]
        for words in ('https://github.com/ly05010419/MacNativeGames', 'origin/main', 'gh', '我同意了再发', 'send only when I say so'):
            self.assertIn(words, pr)
        self.assertIn('Button(T.sendPR) { TerminalHost.shared.ask("claude", AIHelp.prPrompt()) }', text)

    def test_an_ai_already_running_is_told_not_restarted(self):
        # the screenshot of 2026-11-15: the button typed a shell command into a running Claude's prompt
        text = (SWIFT / 'TerminalTab.swift').read_text()
        self.assertIn('if runningAI() != nil { type(prompt + "\\r") } else { type(AIHelp.command(tool: tool, prompt: prompt)) }', text)
        self.assertIn("replacingOccurrences(of: \"'\", with: \"'\\\\''\")", text)   # the prompt is one shell word
        self.assertIn('if command -v \\(tool) >/dev/null', text)   # an AI that is not installed is named, not run

    def test_the_ways_in(self):
        self.assertIn('model.page == "terminal" { TerminalPage() }', (SWIFT / 'ContentView.swift').read_text())
        self.assertIn('Button { model.page = "terminal" }', (SWIFT / 'HomePage.swift').read_text())
        action = (SWIFT / 'ActionSection.swift').read_text()
        self.assertIn('Button(T.askAI)', action)
        self.assertIn('TerminalHost.shared.ask("claude", AIHelp.prompt(log: f.step_log ?? f.log))', action)


if __name__ == '__main__':
    unittest.main()
