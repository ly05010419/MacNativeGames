"""The Launcher's version label (user 2026-11-17: "版本号 不对？？？"): make_app.py's --version defaulted to 1.0.0, so every
Launcher built from a checkout said "MNG Launcher 1.0.0", a release that is not it. Only a release (--portable) has a
version, and it must name it; a checkout's build says 开发版 and its commit."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools.builder_app import make_app  # noqa: E402

SOURCES = ROOT / 'tools/builder_app/Sources/MNGBuilder'


class LauncherVersion(unittest.TestCase):
    def test_no_made_up_default(self):
        text = (ROOT / 'tools/builder_app/make_app.py').read_text()
        self.assertNotIn("default='1.0.0'", text)

    def test_a_release_must_name_its_version(self):
        with self.assertRaisesRegex(SystemExit, '--portable needs --version'):
            make_app.main(['--portable', '-o', '/tmp/never/MNG Launcher.app'])

    def test_a_checkout_build_says_development_build(self):
        app = (SOURCES / 'App.swift').read_text()
        self.assertIn('Paths.isRelease ? T.version(version) : T.devVersion(commit)', app)
        self.assertIn('开发版', (SOURCES / 'Strings.swift').read_text())


if __name__ == '__main__':
    unittest.main()
