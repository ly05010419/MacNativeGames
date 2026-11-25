"""Ocarina of Time without firmware fonts (user 2026-11-16: "经常有人 安装 oot 缺字体 用这种方式 继续安装"): the build takes
the given fonts, else the work folder's, else the open-source stand-ins of tools/deps/open_fonts.py, and the Launcher no
longer tells the player to find a firmware's fonts first."""
import hashlib
import importlib.util
import json
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import build_app  # noqa: E402
from tools.builder_jobs import classify  # noqa: E402
from tools.deps import open_fonts, pins  # noqa: E402

spec = importlib.util.spec_from_file_location('oot_build_fonts', ROOT / 'games/oot/build.py')
oot_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oot_build)

SLOTS = list(build_app.EXPECTED['shared_fonts_sha256'])   # the slot names build_app.py checks, in type order


def fake_font(tag):
    """A TrueType-looking payload (sfnt version 1.0) of an odd length: write_slots pads it to 32-bit words."""
    return b'\x00\x01\x00\x00' + tag.encode() * 5


def make_fonts(folder, tag='open'):
    open_fonts.write_slots([(i, name, f'{name}.ttf', fake_font(f'{tag}{i}')) for i, name in enumerate(SLOTS)], folder)
    return folder


class WriteSlots(unittest.TestCase):
    def test_the_files_pass_the_build_s_font_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = make_fonts(Path(tmp) / 'fonts')
            build_app.validate_fonts(folder)   # raises when a file or the manifest is wrong
            manifest = json.loads((folder / 'manifest.json').read_text())
            self.assertEqual([r['type'] for r in manifest['fonts']], list(range(6)))
            for row in manifest['fonts']:
                data = (folder / f"{row['type']}-{row['name']}.bfttf").read_bytes()
                self.assertEqual(struct.unpack_from('>II', data), (0x7F9A0218, len(data) - 8))   # key 0
                self.assertEqual(len(data) % 4, 0)
                self.assertEqual(row['font_bytes'], len(data) - 8)
                self.assertEqual(row['sha256'], hashlib.sha256(data).hexdigest())

    def test_a_font_that_is_not_truetype_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            slots = [(i, name, 'x', b'wOFF' + b'\0' * 8) for i, name in enumerate(SLOTS)]
            with self.assertRaises(ValueError):
                open_fonts.write_slots(slots, Path(tmp) / 'fonts')

    def test_the_runtime_reads_the_manifest_the_oot_script_writes(self):
        """games/oot/scripts/prepare_shared_fonts.py accepts the folder: the runtime's shared-fonts.txt names each file."""
        import subprocess
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            (tree / 'scripts').mkdir()
            (tree / 'scripts/prepare_shared_fonts.py').write_bytes((ROOT / 'games/oot/scripts/prepare_shared_fonts.py').read_bytes())
            make_fonts(tree / 'extracted/firmware/shared-fonts')
            subprocess.run([sys.executable, '-I', tree / 'scripts/prepare_shared_fonts.py', '--version', 'oot'], check=True,
                           capture_output=True)
            text = (tree / 'build/game-data/oot/shared-fonts.txt').read_text()
            self.assertIn('FONT5_FILE 5-nintendo-extension.bfttf', text)


class Pins(unittest.TestCase):
    def test_one_pinned_font_per_slot_in_the_build_s_order(self):
        self.assertEqual([(kind, name) for kind, name, _, _ in pins.OPEN_FONTS], list(enumerate(SLOTS)))
        for _, _, path, digest in pins.OPEN_FONTS:
            self.assertRegex(digest, r'^[0-9a-f]{64}$')
            self.assertTrue(path.endswith('.ttf'), path)
        self.assertRegex(pins.OPEN_FONTS_COMMIT, r'^[0-9a-f]{40}$')
        self.assertRegex(pins.FONTTOOLS[2], r'^[0-9a-f]{64}$')
        self.assertIn('py3-none-any', pins.FONTTOOLS[0])

    def test_the_download_url_names_the_pinned_commit(self):
        url = open_fonts._url('notosansjp/NotoSansJP[wght].ttf')
        self.assertEqual(url, f'https://raw.githubusercontent.com/google/fonts/{pins.OPEN_FONTS_COMMIT}/ofl/notosansjp/NotoSansJP%5Bwght%5D.ttf')


class PrepareFonts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.fonts = self.dir / 'tree/extracted/firmware/shared-fonts'
        self.made = []

    def tearDown(self):
        self.tmp.cleanup()

    def make(self):
        self.made.append(1)
        return make_fonts(self.dir / 'open', 'open')

    def tags(self):
        return (self.fonts / '0-standard.bfttf').read_bytes()[12:17]

    def test_no_fonts_anywhere_takes_the_open_ones(self):
        said = oot_build.prepare_fonts(self.fonts, None, self.make)
        self.assertEqual(self.made, [1])
        self.assertIn('open-source', said)
        build_app.validate_fonts(self.fonts)
        self.assertEqual(self.tags(), b'open0')

    def test_the_work_folder_s_fonts_are_kept(self):
        make_fonts(self.fonts, 'firm')
        oot_build.prepare_fonts(self.fonts, None, self.make)
        self.assertEqual(self.made, [])
        self.assertEqual(self.tags(), b'firm0')

    def test_given_fonts_replace_the_open_ones(self):
        oot_build.prepare_fonts(self.fonts, None, self.make)
        (self.fonts / 'OFL.txt').write_text('licence of the open fonts')
        given = make_fonts(self.dir / 'given', 'give')
        said = oot_build.prepare_fonts(self.fonts, given, self.make)
        self.assertIn('given', said)
        self.assertEqual(self.tags(), b'give0')
        self.assertFalse((self.fonts / 'OFL.txt').exists())   # nothing of the open fonts is left with the firmware ones

    def test_a_given_folder_that_is_not_fonts_stops_the_build(self):
        bad = self.dir / 'bad'
        bad.mkdir()
        with self.assertRaisesRegex(ValueError, '--shared-fonts'):
            oot_build.prepare_fonts(self.fonts, bad, self.make)
        self.assertEqual(self.made, [])


class SwitchTwoFonts(unittest.TestCase):
    """Bananza and Pokopia (build_app.py main_ns2 → ns2_shared_fonts): the same order as Ocarina of Time."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.fonts = self.dir / 'totk/extracted/firmware/shared-fonts'
        self.made = []

    def tearDown(self):
        self.tmp.cleanup()

    def make(self):
        self.made.append(1)
        return make_fonts(self.dir / 'open', 'open')

    def tag(self):
        return (self.fonts / '0-standard.bfttf').read_bytes()[12:17]

    def test_no_fonts_takes_the_open_ones_then_keeps_them(self):
        self.assertIn('open-source', build_app.ns2_shared_fonts(self.fonts, None, self.make))
        self.assertIn('kept', build_app.ns2_shared_fonts(self.fonts, None, self.make))
        self.assertEqual((self.made, self.tag()), ([1], b'open0'))

    def test_given_fonts_replace_the_open_ones(self):
        build_app.ns2_shared_fonts(self.fonts, None, self.make)
        given = make_fonts(self.dir / 'given', 'give')
        build_app.ns2_shared_fonts(self.fonts, given, self.make)
        self.assertEqual(self.tag(), b'give0')
        self.assertFalse((self.fonts / 'OFL.txt').exists())

    def test_the_old_refusal_is_gone(self):
        source = (ROOT / 'build_app.py').read_text()
        self.assertNotIn('a Switch 2 dump holds no system shared fonts: pass --shared-fonts', source)


class LauncherPlan(unittest.TestCase):
    def test_a_bananza_dump_without_fonts_is_not_blocked(self):
        with tempfile.TemporaryDirectory() as tmp:
            exefs = Path(tmp) / 'dump/updates/458752/exefs'
            exefs.mkdir(parents=True)
            npdm = bytearray(0x200)
            npdm[:4] = b'META'
            npdm[0x70:0x78] = (0x100).to_bytes(4, 'little') + (0x40).to_bytes(4, 'little')
            npdm[0x100:0x104] = b'ACI0'
            npdm[0x110:0x118] = int('040074A01BF12000', 16).to_bytes(8, 'little')
            (exefs / 'main.npdm').write_bytes(bytes(npdm))
            p = classify.plan(classify.classify([Path(tmp) / 'dump']))
            self.assertEqual((p.game, p.blocks), ('dkb', []))
            self.assertTrue(any('Noto' in n for n in p.notes), p.notes)

    def test_a_nup_without_fonts_is_not_blocked_and_says_what_it_uses(self):
        with tempfile.TemporaryDirectory() as tmp:
            sys.path.insert(0, str(ROOT / 'tests'))
            from nup_packages import OOT, fmt, make_nup   # a .nup is told by its package table (#2333)
            nup = make_nup(Path(tmp) / 'Ocarina of Time.nup', (OOT + 0x800, fmt.KIND_UPDATE, 131072))
            p = classify.plan(classify.classify([nup]))
            self.assertEqual(p.game, 'oot')
            self.assertEqual(p.blocks, [])
            self.assertTrue(any('Noto' in n for n in p.notes), p.notes)

    def test_the_build_no_longer_asks_for_firmware_fonts(self):
        source = (ROOT / 'games/oot/build.py').read_text()
        self.assertIsNone(re.search(r'pass --shared-fonts with the six verified firmware fonts', source))


if __name__ == '__main__':
    unittest.main()
