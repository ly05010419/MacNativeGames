"""Version fitting and resolution heaps of optional texture packs, using synthetic resources only."""
import hashlib
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from compression import zstd

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games/totk/scripts'))
from install_texture_pack import prepare, resolution_heap, keep_pack_budgets
from make_mod_manifest import HEAPDEF, heap_sizes
from test_mod_manifest import fake_romfs


def heap(values):
    keys = sorted({'Heaps', 'Size', *values})
    strings = bytearray()
    offsets = []
    for key in keys:
        offsets.append(4 + 4 * (len(keys) + 1) + len(strings))
        strings += key.encode() + b'\0'
    offsets.append(4 + 4 * (len(keys) + 1) + len(strings))
    data = bytearray(b'YB\x07\0' + struct.pack('<III', 16, 0, 0))
    data += struct.pack('<I', len(keys) << 8 | 0xc2) + struct.pack(f'<{len(offsets)}I', *offsets) + strings
    while len(data) % 4:
        data += b'\0'

    def node(fields):
        at = len(data)
        data.extend(struct.pack('<I', len(fields) << 8 | 0xc1))
        for key, kind, value in sorted(fields):
            data.extend(struct.pack('<II', keys.index(key) | kind << 24, value))
        return at

    children = [(key, 0xc1, node([('Size', 0xd1, value)])) for key, value in values.items()]
    heaps = node(children)
    root = node([('Heaps', 0xc1, heaps)])
    struct.pack_into('<I', data, 12, root)
    return bytes(data)


class TexturePackInstall(unittest.TestCase):
    def test_installed_tables_match_game_version_and_both_resolution_heaps(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            app, pack, output = root / 'App.app', root / 'pack', root / 'installed'
            stock = heap(dict(TextureHeap=100, Graphics=200, DynamicTextureAllocatorMem=110, Other=300))
            mod = heap(dict(TextureHeap=1000, Graphics=150, DynamicTextureAllocatorMem=100, Other=100))
            canvas = heap(dict(TextureHeap=100, Graphics=250, DynamicTextureAllocatorMem=160, Other=300))
            name = 'TexToGo/Test.txtg'
            table_name = 'System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs'
            table = b'RESTBL' + struct.pack('<IIII', 1, 160, 1, 0) + struct.pack('<II', zlib.crc32(name.encode()), 200)
            romfs = app / 'Contents/Resources/totk/extracted/v143/romfs'
            fake_romfs(romfs, {HEAPDEF: zstd.compress(stock), name: b'original', table_name: zstd.compress(table)})
            for rel, raw in {HEAPDEF: zstd.compress(mod), name: b'larger texture data'}.items():
                target = pack / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(raw)
            overlay = romfs.parent / 'romfs-patches-out/2560x1440'
            target = overlay / HEAPDEF
            target.parent.mkdir(parents=True)
            raw = zstd.compress(canvas)
            target.write_bytes(raw)
            (overlay / 'manifest.tsv').write_text(f'{HEAPDEF}\t{len(raw)}\t{hashlib.sha256(raw).hexdigest()}\t{hashlib.sha256(zstd.compress(stock)).hexdigest()}\n')
            report = prepare(pack, app, 'v143', output)
            self.assertEqual(report['textures'], 1)
            self.assertEqual(report['resource_entries_grown'], 1)
            fitted = heap_sizes(zstd.decompress((output / 'heap-default' / HEAPDEF).read_bytes()))
            self.assertEqual(fitted, dict(TextureHeap=1000, Graphics=200, DynamicTextureAllocatorMem=110, Other=300))
            canvas_sizes = heap_sizes(zstd.decompress((output / 'romfs-patches-out/2560x1440' / HEAPDEF).read_bytes()))
            self.assertEqual(canvas_sizes, dict(TextureHeap=1000, Graphics=250, DynamicTextureAllocatorMem=160, Other=300))
            adjusted = zstd.decompress((output / table_name).read_bytes())
            self.assertEqual(struct.unpack_from('<I', adjusted, 26)[0], 200 + len(b'larger texture data') - len(b'original'))
            self.assertEqual(zstd.decompress((pack / HEAPDEF).read_bytes()), mod)
            self.assertNotIn(HEAPDEF, (output / 'manifest.tsv').read_text())
            with self.assertRaisesRegex(ValueError, 'already exists'):
                prepare(pack, app, 'v143', output)

    def test_foreign_canvas_changes_are_rejected(self):
        base = heap(dict(TextureHeap=100, Graphics=200, DynamicTextureAllocatorMem=110))
        foreign = heap(dict(TextureHeap=1000, Graphics=250, DynamicTextureAllocatorMem=160))
        with self.assertRaisesRegex(ValueError, 'unrelated heap TextureHeap'):
            resolution_heap(base, base, foreign)

    def test_pack_allocation_budgets_are_preserved_without_importing_other_version_records(self):
        crc = zlib.crc32(b'TexToGo/Test.txtg')
        def table(size, other):
            return b'RESTBL' + struct.pack('<IIII', 1, 160, 2, 0) + struct.pack('<IIII', crc, size, 99, other)
        current, pack = table(200, 50), table(500, 900)
        result, count = keep_pack_budgets(current, [pack], ['TexToGo/Test.txtg'])
        self.assertEqual(count, 1)
        self.assertEqual(struct.unpack_from('<I', result, 26)[0], 500)
        self.assertEqual(struct.unpack_from('<I', result, 34)[0], 50)


if __name__ == '__main__':
    unittest.main()
