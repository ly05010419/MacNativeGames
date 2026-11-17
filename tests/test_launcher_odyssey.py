"""Super Mario Odyssey in MNG Launcher (user 2026-11-17: "继续", after Animal Crossing and Echoes of Wisdom): its card or
NSPs are recognised, the build runs games/odyssey/build.py (scripts/build_odyssey.py behind it) with the app path the
launcher chose, the installed app is found by its bundle ID and updated by the same script, and its pages are those the
Swift app shows."""
import plistlib
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder import games  # noqa: E402
from tools.builder.gui_controls_table import NAMES  # noqa: E402
from tools.builder_jobs import build_job, classify, installed, repo  # noqa: E402


def load_build():
    import importlib.util
    spec = importlib.util.spec_from_file_location('odyssey_build_test', ROOT / 'games/odyssey/build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Wiring(unittest.TestCase):
    def test_titles_and_the_build_command(self):
        for title in ('0100000000010000', '0100000000010800'):
            self.assertEqual(repo.game_of_title(title), 'odyssey')
        self.assertIsNone(classify._supported('odyssey'))   # 1.3.0 / 1.4.1 are checked by hash in the build
        with tempfile.TemporaryDirectory() as tmp:
            plan = {'game': 'odyssey', 'base': '/g.xci', 'keys': '/prod.keys', 'firmware': '/fw', 'blocks': []}
            script, argv, work, out, kind, game = build_job.command({'plan': plan, 'output_dir': tmp, 'work_root': tmp})
        self.assertEqual(script, ROOT / 'games/odyssey/build.py')
        self.assertEqual((kind, game, out.name, argv[0]), ('full', 'odyssey', 'Super Mario Odyssey.app', '/g.xci'))
        self.assertEqual(argv[argv.index('--firmware') + 1], '/fw')

    def test_app_identity_pages_and_buttons(self):
        build = load_build()
        self.assertEqual(build.APP_NAME, repo.GAMES['odyssey'])
        self.assertEqual(build.BUNDLE_ID, games.ODYSSEY.bundle_id)
        self.assertIn(games.ODYSSEY.bundle_id, installed.bundle_ids())
        packager = (ROOT / 'games/odyssey/scripts/package_native_app.py').read_text()
        self.assertIn(f'BUNDLE_ID = "{games.ODYSSEY.bundle_id}"', packager)
        self.assertIn('MacNativeGames/Odyssey', packager)
        self.assertIn('"Odyssey")', (ROOT / 'games/odyssey/scripts/odyssey_app_launcher.c').read_text())
        self.assertEqual(games.PAGES['odyssey'], ('controls', 'storage', 'saves'))
        self.assertEqual(NAMES['odyssey']['a'], 'Jump / Confirm')
        swift = (ROOT / 'tools/builder_app/Sources/MNGBuilder/HomePage.swift').read_text()
        self.assertIn('"odyssey"', re.search(r'static let games = \[[^\]]*\]', swift).group(0))

    def test_update_blocker(self):
        build = load_build()
        with tempfile.TemporaryDirectory() as tmp:
            app, work = Path(tmp) / 'S.app', Path(tmp) / 'work'
            (app / 'Contents').mkdir(parents=True)
            (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'local.projectidea.odyssey'}))
            self.assertIn('before MNG Launcher', build.blocker(app, work))
            (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': build.BUNDLE_ID}))
            self.assertIn('full build', build.blocker(app, work))
            files = [work / 'venv/bin/python', work / 'extracted/local/romfs/index.json',
                     work / 'extracted/extraction-manifest.json', work / 'app-assets/OdysseyIcon.jpg',
                     *(work / f'rewritten/{m}.patched.bin' for m in build.MODULES),
                     *(work / f'extracted/firmware/shared-fonts/{i}.bfttf' for i in range(6))]
            for path in files:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('')
            self.assertEqual(build.blocker(app, work), '')


if __name__ == '__main__':
    unittest.main()
