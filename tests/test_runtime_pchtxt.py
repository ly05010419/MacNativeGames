"""TOTK_PCHTXT (runtime/pchtxt.h, the reader game_patches.c and Breath of the Wild's game_patches_botw.c for 1.9.0
and 1.6.0 share) reads a pchtxt the way the build's --mod does (games/totk/scripts/mod_exefs.py): @disabled sections are not
applied, @flag print_values is accepted, and "@flag offset_shift 0x100" is required before the first patch line; a line outside main or with a byte that is not
two hex digits stops the start."""
from pathlib import Path
import build_app
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD_ID = bytes(range(0x10, 0x24))       # 20 bytes at 0x40 of the extracted main
IMAGE = 0x3000000                          # the probe's main (tests/pchtxt_probe.c)
HEADER = f'@nsobid-{BUILD_ID.hex().upper()}\n@flag print_values\n@flag offset_shift 0x100\n'


class PchtxtCases:
    DEFINES = []          # compiler options that select the game's runtime source (tests/pchtxt_probe.c)
    GAME_DIR = 'v0'       # extracted/<this>/exefs/main: where the runtime reads main's build id

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
                                '-Wno-deprecated-declarations', *cls.DEFINES, '-I' + str(gfx / 'include'),
                                '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                                str(ROOT / 'tests/pchtxt_probe.c'), '-o', str(cls.probe)],
                               capture_output=True, text=True, timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)
        main = cls.dir / 'extracted' / cls.GAME_DIR / 'exefs/main'
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

    def test_bounds(self):
        for line, ok in ((f'{IMAGE - 1:X} aa', True), (f'{IMAGE:X} aa', False), (f'{IMAGE - 1:X} aabb', False),
                         ('FFFFFFFFFFFFFFFF aa', False), ('FFFFFFFFFFFFFFFF aabb', False)):
            code, out = self.run_patch(HEADER + f'@enabled\n{line}\n@stop\n')
            self.assertEqual(code, 0 if ok else 3, (line, out))
            if not ok:
                self.assertIn('malformed or outside main', out)

    def test_bytes_that_are_not_hex_stop(self):
        for value in ('AQ', 'GG', 'aa0x', '+1'):
            code, out = self.run_patch(HEADER + f'@enabled\n40 {value}\n@stop\n')
            self.assertEqual(code, 3, (value, out))
            self.assertIn('not hex digits', out)


    def test_packaged_app_reads_main_build_id(self):
        app = self.dir / 'app'
        build_id = app / f'extracted/{self.GAME_DIR}/exefs/main.build-id'
        build_id.parent.mkdir(parents=True)
        build_id.write_text(BUILD_ID.hex().upper() + '\n')
        code, out = self.run_patch(HEADER + '@enabled\n40 aabbccdd\n@stop\n', app)
        self.assertEqual(code, 0, out)
        self.assertTrue(out.splitlines()[-1].startswith('aabbccdd'), out)

        build_id.write_text('not a build id\n')
        code, out = self.run_patch(HEADER + '@enabled\n40 aabbccdd\n@stop\n', app)
        self.assertEqual(code, 3, out)
        self.assertIn('main.build-id', out)


@unittest.skipUnless(sys.platform == 'darwin', 'the runtime builds on macOS only')
class RuntimePchtxt(PchtxtCases, unittest.TestCase):
    pass


class BotwChecks:
    """Breath of the Wild passes the reader the sites the app patches or checks itself, and a line on one stops the
    start (the build's --mod refuses it as well)."""
    RESERVED = 0       # the first byte of a reserved site (main's ShowKeyboard GOT slot, 8 bytes)

    def test_reserved_site_stops(self):
        for start in (self.RESERVED, self.RESERVED + 4, self.RESERVED - 1):
            code, out = self.run_patch(HEADER + f'@enabled\n{start:X} 0000\n@stop\n')
            self.assertEqual(code, 3, (start, out))
            self.assertIn('touches a site the app patches or checks itself', out)

    def test_upstreams_sites_stop(self):
        # The camera blend, the 1.9.0 sun shadow moves, field of view and native-size block (upstream's patches, listed in
        # patch_sites.BOTW_UPSTREAM): a line on the first or the last byte of each stops the start.
        for first, size in self.UPSTREAM:
            for at in (first, first + size - 1):
                code, out = self.run_patch(HEADER + f'@enabled\n{at:X} 00\n@stop\n')
                self.assertEqual(code, 3, (hex(at), out))
                self.assertIn('touches a site the app patches or checks itself', out)

    def test_next_to_a_reserved_site_is_applied(self):
        code, out = self.run_patch(HEADER + f'@enabled\n{self.RESERVED + 8:X} 00\n{self.RESERVED - 2:X} 0000\n40 aa\n@stop\n')
        self.assertEqual(code, 0, out)

    def test_reserved_table_matches_patch_sites(self):
        sys.path.insert(0, str(ROOT / 'games/totk/scripts'))
        import patch_sites
        text = patch_sites.compiled_text((ROOT / 'games/totk/runtime/botw_pchtxt_sites.h').read_text(),
                                         patch_sites.BOTW_DEFINED[self.VERSION])
        table = re.search(r'g_pchtxt_reserved\[\] = \{(.*?)\n\};', text, re.S).group(1)
        runtime = {o + i for o, n in ((int(a, 16), int(b, 16)) for a, b in re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+)\}', table))
                   for i in range(n)}
        build = {s.offset + i for s in patch_sites.sites(self.VERSION) for i in range(s.size)}
        got = set(range(self.RESERVED, self.RESERVED + 8))      # the build reserves the slot's first word; the runtime all 8 bytes
        self.assertEqual(runtime - got, build - got)


@unittest.skipUnless(sys.platform == 'darwin', 'the runtime builds on macOS only')
class RuntimePchtxtBotw190(BotwChecks, PchtxtCases, unittest.TestCase):
    """Breath of the Wild 1.9.0 has its own copy of the reader (game_patches_botw.c): the same reading, its own folder."""
    DEFINES = ['-DGAME_BOTW190']
    GAME_DIR = VERSION = 'botw190'
    RESERVED = 0x1d78198
    UPSTREAM = [(0xc33678, 12), (0xc33730, 8), (0x124eef8, 16), (0x10edad0, 16), (0xeb48ac, 4), (0x71c, 4), (0x12660d0, 8), (0x12660bc, 0x50)]


@unittest.skipUnless(sys.platform == 'darwin', 'the runtime builds on macOS only')
class RuntimePchtxtBotw160(BotwChecks, PchtxtCases, unittest.TestCase):
    """1.6.0 compiles the same reader (game_patches_botw.c) and reads main's build id from extracted/botw160."""
    DEFINES = ['-DGAME_BOTW160']
    GAME_DIR = VERSION = 'botw160'
    RESERVED = 0x2c7ef90
    UPSTREAM = [(0xdddfe4, 4), (0xdde044, 16), (0xdde090, 4)]


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
