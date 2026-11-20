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


def fake_card(path, metas):
    """A game card image of HFS0 tables only (no NCA bytes): the root's secure partition lists metas .cnmt.nca files."""
    import struct

    def hfs0(names, sizes):
        strings = b''.join(n.encode() + b'\0' for n in names)
        table, offset = b'', 0
        for i, size in enumerate(sizes):
            table += struct.pack('<QQII8x32s', offset, size, strings.index(names[i].encode() + b'\0'), 0, bytes(32))
            offset += size
        return b'HFS0' + struct.pack('<II4x', len(names), len(strings)) + table + strings

    secure = hfs0([f'{i:032x}.cnmt.nca' for i in range(metas)] + ['f' * 32 + '.nca'], [16] * (metas + 1))
    root = hfs0(['secure'], [len(secure) + 16 * (metas + 1)])
    head = bytearray(0x200)
    head[0x100:0x104] = b'HEAD'
    head[0x130:0x138] = (0x200).to_bytes(8, 'little')
    Path(path).write_bytes(bytes(head) + root + secure + bytes(16 * (metas + 1)))


class CombinedCard(unittest.TestCase):
    """A player's report (2026-11-20): a merged card "[1G+1U+3D]" and the same game as base, four updates and three
    add-ons were both refused (the add-ons "need the 3.0.1 or 3.0.3 update" next to a 3.0.3 update)."""

    def test_update_names_give_their_version(self):
        self.assertEqual(classify._version('[UPD][01006F8002326800][3.0.3][US][22.1.0].nsp', 'acnh'), 2228224)
        self.assertEqual(classify._version('[UPD][01006F8002326800][3.0.1][US][21.2.0].nsp', 'acnh'), 2097152)
        self.assertIsNone(classify._version('[UPD][01006F8002326800][3.0.2][US][22.0.0].nsp', 'acnh'))

    def test_the_newest_supported_update_carries_the_add_ons(self):
        items = [classify.Item('/b.nsp', 'base', 'acnh', BASE, 0),
                 classify.Item('/u300.nsp', 'update', 'acnh', UPDATE, None),
                 classify.Item('/u301.nsp', 'update', 'acnh', UPDATE, 2097152),
                 classify.Item('/u303.nsp', 'update', 'acnh', UPDATE, 2228224),
                 classify.Item('/d.nsp', 'dlc', 'acnh', DLC)]
        with tempfile.TemporaryDirectory() as tmp:
            for i in items:
                Path(tmp + i.path).write_bytes(b'x')
            p = classify._plan([classify.Item(tmp + i.path, *list(classify.asdict(i).values())[1:]) for i in items],
                               open_fonts=True)
        self.assertTrue(p.update.endswith('/u303.nsp'))
        self.assertEqual(p.update_version, 2228224)
        self.assertFalse([b for b in p.blocks if 'add-on' in b or '追加' in b], p.blocks)

    def test_a_combined_card_is_its_own_update(self):
        with tempfile.TemporaryDirectory() as tmp:
            card, plain = Path(tmp) / 'combined.xci', Path(tmp) / 'plain.xci'
            fake_card(card, 5)
            fake_card(plain, 1)
            self.assertEqual((classify.card_titles(card), classify.card_titles(plain)), (5, 1))
            p = classify._plan([classify.Item(str(card), 'base', 'acnh', BASE, 2228224)], open_fonts=True)
            self.assertEqual((p.update, p.update_version), (str(card), 2228224))
            p = classify._plan([classify.Item(str(plain), 'base', 'acnh', BASE, 0)], open_fonts=True)
            self.assertIsNone(p.update)
