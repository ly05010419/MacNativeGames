"""Identity of a RomFS image (dm/tools/romfs_image.py identity, dm/host/romfs_identity.h): the line's format, Python and C agreeing on it, and the check main.m makes at start
(match, another version, missing, cut off, unreadable identity, no identity). Synthetic images only."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('romfs_image', REPO / 'dm/tools/romfs_image.py')
assert spec is not None and spec.loader is not None
image = importlib.util.module_from_spec(spec); spec.loader.exec_module(image)

CHECK_C = r'''#include "romfs_identity.h"
int main(int argc, char **argv) {
    romfs_id want, got; char line[400], msg[900];
    if (argc == 3 && !strcmp(argv[1], "of")) {   /* of IMAGE: the identity line the C reader computes */
        if (!romfs_id_of_image(argv[2], &got)) return 1;
        romfs_id_text(&got, line, sizeof line); puts(line); return 0;
    }
    if (argc != 4 || strcmp(argv[1], "check")) return 2;   /* check EXPECTED IMAGE: exit code = verdict, then the message */
    int rc = romfs_id_check(argv[2], argv[3], &want, &got);
    if (rc != ROMFS_ID_OK && rc != ROMFS_ID_NONE) { romfs_id_message(rc, &want, &got, "org.example.game", msg, sizeof msg); puts(msg); }
    return rc;
}
'''


class Identity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name); (root / 'check.c').write_text(CHECK_C); cls.check = root / 'check'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/host'), str(root / 'check.c'), '-o', str(cls.check)], check=True)
        cls.data = root / 'data'
        for name, size in (('wanted', 70_000), ('other', 70_001)):   # a first index chunk bigger than nothing, a second not a multiple of 8
            d = root / name; (d / 'sub').mkdir(parents=True)
            (d / 'marker').write_text(name); (d / 'sub' / 'big').write_bytes(bytes(range(256)) * (size // 256) + b'x' * (size % 256))
            image.pack(d, root / f'{name}.img')
        cls.wanted, cls.other = root / 'wanted.img', root / 'other.img'

    def test_line_format_and_parse(self):
        line = image.identity(self.wanted, 'ABC', '1.2.1 beta', 'v327680')
        fields = image.parse_identity(line)
        self.assertEqual((fields['title'], fields['version'], fields['build']), ('ABC', '1.2.1_beta', 'v327680'))
        self.assertEqual(fields['size'], self.wanted.stat().st_size); self.assertRegex(fields['index'], r'^[0-9a-f]{16}$')
        self.assertTrue(line.startswith('DMROMFSID1 title=')); self.assertNotIn('\n', line)
        with self.assertRaises(ValueError): image.parse_identity('something else')
        with self.assertRaises(ValueError): image.parse_identity('DMROMFSID1 title=x')

    def test_same_image_same_identity_other_image_other_identity(self):
        a, b = image.identity(self.wanted, 'A', '1', 'x'), image.identity(self.wanted, 'B', '2', 'y')
        self.assertTrue(image.same_identity(a, b))   # title, version and build only describe the image
        self.assertFalse(image.same_identity(a, image.identity(self.other)))

    def test_python_and_c_compute_the_same_identity(self):
        for img in (self.wanted, self.other):
            c = subprocess.run([str(self.check), 'of', str(img)], capture_output=True, text=True, check=True).stdout.strip()
            self.assertTrue(image.same_identity(c, image.identity(img)), c)

    def test_identity_file_is_written_and_read_back(self):
        with tempfile.TemporaryDirectory() as t:
            f = Path(t) / 'romfs.id'
            r = subprocess.run(['python3', '-I', str(REPO / 'dm/tools/romfs_image.py'), '--identity', str(self.wanted), 'T', '1.0', 'v0'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr); f.write_text(r.stdout)
            self.assertEqual(r.stdout.count('\n'), 1)
            self.assertEqual(subprocess.run([str(self.check), 'check', str(f), str(self.wanted)]).returncode, 0)   # ROMFS_ID_OK
            bad = subprocess.run(['python3', '-I', str(REPO / 'dm/tools/romfs_image.py'), '--identity', str(Path(t) / 'missing.img')], capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0); self.assertIn('[RED]', bad.stderr)

    def test_verdicts(self):
        OK, NONE, BAD, MISSING, UNREADABLE, DIFFERENT = range(6)
        with tempfile.TemporaryDirectory() as t:
            t = Path(t); ident = t / 'romfs.id'; ident.write_text(image.identity(self.wanted, 'T', '1.0', 'v0') + '\n')
            def check(expected, img):
                r = subprocess.run([str(self.check), 'check', str(expected), str(img)], capture_output=True, text=True); return r.returncode, r.stdout
            self.assertEqual(check(ident, self.wanted)[0], OK)
            rc, msg = check(ident, self.other)
            self.assertEqual(rc, DIFFERENT)
            for text in ('Expected: T 1.0', 'build v0', 'Found: %d bytes' % self.other.stat().st_size, 'scripts/push_romfs.sh <device UDID> org.example.game'): self.assertIn(text, msg)
            rc, msg = check(ident, t / 'none.img'); self.assertEqual(rc, MISSING); self.assertIn('no game file image', msg)
            cut = t / 'cut.img'; cut.write_bytes(self.wanted.read_bytes()[:-5000])
            self.assertEqual(check(ident, cut)[0], UNREADABLE)
            junk = t / 'junk.id'; junk.write_text('DMROMFSID1 title=T\n'); self.assertEqual(check(junk, self.wanted)[0], BAD)
            junk.write_text('hello'); self.assertEqual(check(junk, self.wanted)[0], BAD)
            self.assertEqual(check(t / 'absent.id', self.wanted)[0], NONE)   # a build without an identity takes any image


if __name__ == '__main__':
    unittest.main()
