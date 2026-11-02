"""TOTK_PCHTXT (runtime/game_patches.c patch_pchtxt) reads a pchtxt the way the build's --mod does
(games/totk/scripts/mod_exefs.py): @disabled sections are not applied, @flag print_values is accepted, and
"@flag offset_shift 0x100" is required before the first patch line."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import build_app

ROOT = Path(__file__).resolve().parents[1]
BUILD_ID = bytes(range(0x10, 0x24))       # 20 bytes at 0x40 of the extracted main
HEADER = f'@nsobid-{BUILD_ID.hex().upper()}\n@flag print_values\n@flag offset_shift 0x100\n'


@unittest.skipUnless(sys.platform == 'darwin', 'the runtime builds on macOS only')
class RuntimePchtxt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('clang is required')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.probe = cls.dir / 'pchtxt-probe'
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                                '-Wno-deprecated-declarations', '-I' + str(gfx / 'include'),
                                '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                                str(ROOT / 'tests/pchtxt_probe.c'), '-o', str(cls.probe)],
                               capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)
        main = cls.dir / 'extracted/v0/exefs/main'
        main.parent.mkdir(parents=True)
        main.write_bytes(b'NSO0' + bytes(0x3c) + BUILD_ID + bytes(0x100))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_patch(self, text, folder=None):
        folder = folder or self.dir
        patch = folder / 'mod.pchtxt'
        patch.write_text(text)
        env = dict(os.environ, TOTK_PCHTXT=str(patch))
        run = subprocess.run([str(self.probe)], cwd=folder, env=env, capture_output=True, text=True, timeout=30)
        return run.returncode, run.stdout.strip()

    def test_disabled_section_is_not_applied(self):
        code, out = self.run_patch(HEADER + '@enabled\n40 aabbccdd\n@disabled\n44 eeff\n@enabled\n48 0102\n@stop\n4c 99\n')
        self.assertEqual(code, 0, out)
        self.assertEqual(out.splitlines()[-1], 'aabbccdd' + '00000000' + '0102' + '0000' + '00000000')

    def test_community_header_with_print_values_and_crlf(self):
        code, out = self.run_patch((HEADER + '\n// a section\n@enabled\n40 c0000014\n@stop\n').replace('\n', '\r\n'))
        self.assertEqual(code, 0, out)
        self.assertTrue(out.splitlines()[-1].startswith('c0000014'), out)

    def test_missing_offset_shift_stops(self):
        code, out = self.run_patch(f'@nsobid-{BUILD_ID.hex()}\n@flag print_values\n@enabled\n40 aabbccdd\n@stop\n')
        self.assertEqual(code, 3, out)
        self.assertIn('no @flag offset_shift 0x100 before the first patch line', out)

    def test_another_offset_shift_stops(self):
        code, out = self.run_patch(f'@nsobid-{BUILD_ID.hex()}\n@flag offset_shift 0x0\n@enabled\n40 aabbccdd\n@stop\n')
        self.assertEqual(code, 3, out)
        self.assertIn('offset_shift must be 0x100', out)

    def test_unknown_flag_stops(self):
        code, out = self.run_patch(HEADER + '@flag something 1\n@enabled\n40 aabbccdd\n@stop\n')
        self.assertEqual(code, 3, out)
        self.assertIn('only @flag offset_shift 0x100 and print_values', out)

    def test_another_build_stops(self):
        code, out = self.run_patch('@nsobid-' + '00' * 20 + '\n@flag offset_shift 0x100\n@enabled\n40 aa\n@stop\n')
        self.assertEqual(code, 3, out)
        self.assertIn('another build of main', out)

    def test_packaged_app_reads_main_build_id(self):
        app = self.dir / 'app'
        build_id = app / 'extracted/v0/exefs/main.build-id'
        build_id.parent.mkdir(parents=True)
        build_id.write_text(BUILD_ID.hex().upper() + '\n')
        code, out = self.run_patch(HEADER + '@enabled\n40 aabbccdd\n@stop\n', app)
        self.assertEqual(code, 0, out)
        self.assertTrue(out.splitlines()[-1].startswith('aabbccdd'), out)

        build_id.write_text('not a build id\n')
        code, out = self.run_patch(HEADER + '@enabled\n40 aabbccdd\n@stop\n', app)
        self.assertEqual(code, 3, out)
        self.assertIn('main.build-id', out)


class MainBuildIdFile(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)

    def test_the_build_writes_mains_build_id(self):
        main = self.dir / 'tree/extracted/v121/exefs/main'
        main.parent.mkdir(parents=True)
        main.write_bytes(b'NSO0' + bytes(0x3c) + BUILD_ID + bytes(0x100))
        target = self.dir / 'app/extracted/v121/exefs/main.build-id'
        build_app.write_main_build_id(main, target)
        self.assertEqual(target.read_text(), BUILD_ID.hex().upper() + '\n')

    def test_nothing_is_written_without_an_extracted_main(self):
        target = self.dir / 'app/extracted/v121/exefs/main.build-id'
        build_app.write_main_build_id(self.dir / 'tree/extracted/v121/exefs/main', target)
        self.assertFalse(target.exists())


if __name__ == '__main__':
    unittest.main()
