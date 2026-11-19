"""Animal Crossing: New Horizons in MNG Launcher (user 2026-11-17: "合并脚本 然后2", the merged build script and the full
card): its files are recognised, the build runs games/acnh/build.py with the plan's files, the installed app is found by
its bundle ID, its update goes to the same script, and its pages are those the Swift app shows."""
import json
from pathlib import Path
import plistlib
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder import games  # noqa: E402
from tools.builder_jobs import build_job, classify, installed, repo  # noqa: E402

BASE, UPDATE, DLC = '01006F8002326000', '01006F8002326800', '01006F80023273E8'


class Wiring(unittest.TestCase):
    def test_titles_belong_to_acnh(self):
        for title in (BASE, UPDATE, DLC):
            self.assertEqual(repo.game_of_title(title), 'acnh')
        self.assertEqual(repo.GAMES['acnh'], 'Animal Crossing New Horizons')

    def test_supported_versions_match_the_preparer(self):
        source = (ROOT / 'games/acnh/prepare.py').read_text()
        updates = set(map(int, re.search(r'SUPPORTED_UPDATES = \(([^)]*)\)', source).group(1).replace(' ', '').split(',')))
        self.assertEqual(classify._supported('acnh'), {0} | updates)

    def test_the_build_runs_the_merged_script_with_the_plan_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            plan = {'game': 'acnh', 'base': '/b.nsp', 'keys': '/prod.keys', 'update': '/u.nsp', 'update_version': 2228224,
                    'dlc': ['/d.nsp'], 'fonts': '/fonts', 'blocks': []}
            script, argv, work, out, kind, game = build_job.command(
                {'plan': plan, 'output_dir': tmp, 'work_root': str(Path(tmp) / 'work')})
        self.assertEqual(script, ROOT / 'games/acnh/build.py')
        self.assertEqual((kind, game, out.name, work.name), ('full', 'acnh', 'Animal Crossing New Horizons.app', 'acnh'))
        for flag, value in (('-k', '/prod.keys'), ('--update', '/u.nsp'), ('--dlc', '/d.nsp'), ('--shared-fonts', '/fonts')):
            self.assertEqual(argv[argv.index(flag) + 1], value)

    def test_same_version_compares_the_update(self):
        plan = {'game': 'acnh', 'update': '/u.nsp', 'update_version': 2228224}
        self.assertTrue(build_job.same_version(plan, {'game': 'acnh', 'version': 'v2228224'}))
        self.assertFalse(build_job.same_version(plan, {'game': 'acnh', 'version': 'v2097152'}))

    def test_installed_app_and_pages(self):
        self.assertIn(games.ACNH.bundle_id, installed.bundle_ids())
        packager = (ROOT / 'games/acnh/scripts/package_native_app.py').read_text()
        self.assertIn(f'BUNDLE_ID = "{games.ACNH.bundle_id}"', packager)
        self.assertEqual(games.PAGES['acnh'], ('controls', 'storage', 'saves'))
        swift = (ROOT / 'tools/builder_app/Sources/MNGBuilder/GameStrings.swift').read_text()   # GameCatalog, both Launchers
        self.assertIn('"acnh"', re.search(r'static let all = \[[^\]]*\]', swift).group(0))


class UpdateBlocker(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, str(ROOT / 'games/acnh'))
        import importlib.util
        spec = importlib.util.spec_from_file_location('acnh_build_test', ROOT / 'games/acnh/build.py')
        self.build = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.build)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.app, self.work = root / 'A.app', root / 'work'
        (self.app / 'Contents').mkdir(parents=True)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(
            {'CFBundleIdentifier': 'org.macnativegames.acnh', 'CFBundleShortVersionString': '3.0.3'}))

    def ready(self):
        (self.work / 'prepared').mkdir(parents=True)
        (self.work / 'prepared/preparation.json').write_text(json.dumps(
            {'status': 'prepared_for_host_bringup', 'inputs': {'update': {'cnmt_version_raw': 2228224}}}))
        fonts = self.work / 'fonts'
        fonts.mkdir()
        rows = []
        for kind, name in enumerate(('standard', 'chinese-simplified', 'chinese-simplified-extension',
                                     'chinese-traditional', 'korean', 'nintendo-extension')):
            (fonts / f'{kind}-{name}.bfttf').write_bytes(b'x')
            rows.append({'type': kind, 'name': name, 'bfttf_bytes': 1, 'font_bytes': 0})
        (fonts / 'manifest.json').write_text(json.dumps({'status': 'pass', 'fonts': rows}))
        (self.work / 'venv/bin').mkdir(parents=True)
        (self.work / 'venv/bin/python').write_text('')

    def test_a_complete_work_folder_allows_the_update(self):
        self.ready()
        self.assertEqual(self.build.blocker(self.app, self.work), '')

    def test_each_missing_part_asks_for_a_full_build(self):
        self.assertIn('full build', self.build.blocker(self.app, self.work))
        self.ready()
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps(
            {'CFBundleIdentifier': 'org.macnativegames.acnh', 'CFBundleShortVersionString': '3.0.1'}))
        self.assertIn('3.0.3', self.build.blocker(self.app, self.work))


if __name__ == '__main__':
    unittest.main()
