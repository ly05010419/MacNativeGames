"""make_mod_manifest.py: the manifest of a player's RomFS mod, checked against a small fake RomFS (no game data)."""
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'games/totk/scripts/make_mod_manifest.py'
FILES = {'Pack/A.pack.zs': b'original a', 'Sound/B.bwav': b'original b' * 100, 'UI/C.bfres': b'c'}


def fake_romfs(folder):
    """romfs.bin and runtime-index.bin in the layout scripts/prepare_romfs_backend.py writes."""
    folder.mkdir(parents=True)
    image, rows, strings = bytearray(), bytearray(), bytearray()
    for path in sorted(FILES):
        name = path.encode() + b'\0'
        rows += struct.pack('<QQII', len(image), len(FILES[path]), len(strings), len(name))
        image += FILES[path]
        strings += name
    (folder/'romfs.bin').write_bytes(image)
    (folder/'runtime-index.bin').write_bytes(struct.pack('<8sQQ', b'TOTKRFS1', len(FILES), 24 + len(rows)) + rows + strings)
    return folder


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


class ModManifest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.romfs = fake_romfs(self.root/'romfs')
        self.mod = self.root/'mods/v143'

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, path, data):
        (self.mod/path).parent.mkdir(parents=True, exist_ok=True)
        (self.mod/path).write_bytes(data)

    def test_lists_replaced_files_with_both_hashes(self):
        self.put('Sound/B.bwav', b'dubbed')
        self.put('UI/C.bfres', b'c')   # same as the original: left out
        self.put('.DS_Store', b'finder')
        result = run(self.mod, '--romfs', self.romfs)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [l for l in (self.mod/'manifest.tsv').read_text().splitlines() if not l.startswith('#')]
        self.assertEqual(lines, ['\t'.join(['Sound/B.bwav', '6', hashlib.sha256(b'dubbed').hexdigest(),
                                            hashlib.sha256(FILES['Sound/B.bwav']).hexdigest()])])
        self.assertIn('(same as the original): UI/C.bfres', result.stdout)

    def test_misspelt_path_writes_nothing(self):
        self.put('Sound/b.bwav', b'wrong case')
        result = run(self.mod, '--romfs', self.romfs)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not a file of this RomFS: Sound/b.bwav (it has Sound/B.bwav', result.stderr)
        self.assertFalse((self.mod/'manifest.tsv').exists())

    def test_new_file_is_added(self):   # issue #48
        self.put('Sound/B.bwav', b'dubbed')
        self.put('UI/Tex/New/Gold.bntx.zs', b'new texture')
        result = run(self.mod, '--romfs', self.romfs)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = sorted(l for l in (self.mod/'manifest.tsv').read_text().splitlines() if not l.startswith('#'))
        self.assertEqual(lines[1], '\t'.join(['UI/Tex/New/Gold.bntx.zs', '11', hashlib.sha256(b'new texture').hexdigest(), '-']))
        self.assertIn('(1 replaced, 1 added)', result.stdout)

    def test_app_path_takes_the_version_from_the_folder(self):
        app = self.root/'Tears of the Kingdom.app'
        fake_romfs(app/'Contents/Resources/totk/extracted/v143/romfs')
        self.put('Pack/A.pack.zs', b'modded a')
        result = run(self.mod, '--app', app)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('1 files', result.stdout)


if __name__ == '__main__':
    unittest.main()
