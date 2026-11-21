"""Mario Kart 8 Deluxe on the Zelda-line runtime: the pieces this port added that run without the game.

- runtime/mii_host.c (the Mii database the host answers) and runtime/romfs_work_size.h (the add-on content mount
  cache), through tests/mii_probe.c;
- scripts/index_romfs_image.py (the runtime's RomFS index from a merged image, either layout);
- the rewriter rows registered for this game's subsdk0, found by scripts/find_signed_tables.py;
- that the port carries no nxnative code and is built on the Zelda line (games/odyssey's runtime and graphics layer)."""
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MiiAndCache(unittest.TestCase):
    def test_probe(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe, aoc = Path(tmp) / 'mii_probe', Path(tmp) / 'aoc'
            aoc.mkdir()
            (aoc / 'contents.dat').write_bytes(bytes(4096))
            (aoc / 'version.txt').write_bytes(b'10002')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-DMII_HOST_TEST',
                            str(HERE / 'tests/mii_probe.c'), str(HERE / 'runtime/mii_host.c'), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe), str(aoc)], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertEqual(json.loads(out.stdout)['status'], 'pass')


class RomfsIndex(unittest.TestCase):
    index = load(HERE / 'scripts/index_romfs_image.py', 'index_romfs_image')

    def dmromfs(self, folder):
        """root/{a.bin, sub/{b.txt}} in the flat layout: data, then the breadth-first table and the name pool."""
        data = b'AAAA' + b'hello'
        names = b'' ; entries = []
        def name(s):
            nonlocal names
            off = len(names); names += s.encode(); return off, len(s)
        # 0 root (children 1..2), 1 a.bin (file), 2 sub (dir, child 3), 3 b.txt
        n_a, n_sub, n_b = name('a.bin'), name('sub'), name('b.txt')
        entries = [(1, 2, 0, 0, 0), (32, 4, *n_a, 1), (3, 1, *n_sub, 0), (36, 5, *n_b, 1)]
        table = b''.join(struct.pack('<QQIHBB', a, b, no, nl, t, 0) for a, b, no, nl, t in entries) + names
        image = struct.pack('<8sQQQ', b'DMROMFS1', 32 + len(data), len(table), len(entries)) + data + table
        (folder / 'romfs.bin').write_bytes(image)

    def test_dmromfs_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.dmromfs(folder)
            m = self.index.index(folder)
            self.assertEqual(m['layout'], 'DMROMFS1')
            self.assertEqual([(f['path'], f['offset'], f['size']) for f in m['files']], [('a.bin', 32, 4), ('sub/b.txt', 36, 5)])
            raw = (folder / 'romfs.bin').read_bytes()
            self.assertEqual(raw[36:41], b'hello')

    def test_bad_image_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            self.dmromfs(folder)
            raw = bytearray((folder / 'romfs.bin').read_bytes())
            struct.pack_into('<Q', raw, 0x20 + 9 + 24, 1 << 40)   # a.bin's offset far outside the image
            (folder / 'romfs.bin').write_bytes(raw)
            with self.assertRaises(ValueError):
                self.index.index(folder)


class Rewriter(unittest.TestCase):
    def test_subsdk0_tables_registered(self):
        text = (HERE / 'third_party/d2r/patch_nso.py').read_text()
        self.assertIn("'d9de423efc1512328c3c05378c7cb031'", text)   # 4.0.1 subsdk0's build id
        for site in ('0x1ae1f0', '0x1b18ec'):
            self.assertIn(f'{site}: (', text)


class ZeldaLine(unittest.TestCase):
    def test_no_nxnative_code(self):
        self.assertFalse((HERE / 'third_party/nxnative').exists())
        for f in HERE.rglob('*'):
            if f.is_symlink():
                self.assertNotIn('nxnative', str(f.readlink()), f)
                self.assertNotIn('games/mk8d/', str(f.resolve()), f)

    def test_runtime_links_the_shared_files_odyssey_links(self):
        ody = ROOT / 'games/odyssey/runtime'
        for f in ody.iterdir():
            if f.is_symlink() and 'shared/runtime' in str(f.readlink()) and f.name != 'threads.c':
                mine = HERE / 'runtime' / f.name
                self.assertTrue(mine.is_symlink() and mine.resolve() == f.resolve(), f.name)
        self.assertEqual((HERE / 'runtime/offline_host.c').resolve(), (ROOT / 'shared/runtime/offline_host.c').resolve())

    def test_game_switch(self):
        self.assertIn('#define GAME_MK8DX 1', (HERE / 'runtime/game_version.h').read_text())
        self.assertIn('-DGAME_MK8DX=1', (HERE / 'Makefile').read_text())


if __name__ == '__main__':
    unittest.main()
