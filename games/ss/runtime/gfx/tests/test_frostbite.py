"""frostbite.py on a synthetic Frostbite RomFS: layout.toc's install chunks, a superbundle toc whose one bundle has a CompiledBytecode resource and a texture, the bundle manifest in a cas file,
the item's blocks (stored and through a decoder) and the VNVN record's code area and control section."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import frostbite as fb

CHUNK = 0xA3A00DE1   # installpackage_00's persistentIndex in EA SPORTS FC 27


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7f; n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n: return bytes(out)


def dbo(kind, name, body):   # one DbObject value: a type byte with bit 7 for an unnamed one, its name, its body
    return bytes([kind | (0x80 if name is None else 0)]) + (b'' if name is None else name.encode() + b'\0') + body


def dbo_obj(name, items, kind=2): body = b''.join(items) + b'\0'; return dbo(kind, name, leb(len(body)) + body)
def dbo_str(name, s): b = s.encode() + b'\0'; return dbo(7, name, leb(len(b)) + b)
def dbo_i32(name, v): return dbo(8, name, struct.pack('<i', v))


def toc_file(body): return fb.TOC_MAGIC + bytes(fb.TOC_BODY - 4) + body


def block(data, codec=0, stored=None):   # an item block: stored bytes as they are, or what a decoder is given
    s = data if stored is None else stored
    return struct.pack('>IBBH', len(data), codec, 0x70 | len(s) >> 16, len(s) & 0xffff) + s


def vnvn(code, ctl):   # a CompiledBytecode resource: a 0x20-byte header, then the VNVN record (version 9, one register mapping in the first list)
    head = b'VNVN' + struct.pack('<IHHI', 0, 9, 0, 0) + b'\0' + struct.pack('<H', 1) + bytes(4) + struct.pack('<HHHH', 0, 0, 0, 0)
    co = len(head) + 24; ko = co + len(code)
    rec = head + struct.pack('<4IQ', co, len(code), ko, len(ctl), 0) + code + ctl
    return bytes([5, 0, 8, 0]) + bytes(0x1c) + rec


def manifest(res):   # res: [(name, size, type)] → the manifest as the cas file stores it
    names = b''; offs = []
    for n, _, _ in res: offs.append(len(names)); names += n.encode() + b'\0'
    tot = len(res); body_head = 0x20 + 20 * tot + 8 * tot + 4 * tot + 24 * tot
    hdr = struct.pack('<8I', fb.BUNDLE_MAGIC, tot, 0, tot, 0, body_head, 0, 0) + bytes(20 * tot)
    hdr += b''.join(struct.pack('<II', o, s) for o, (_, s, _) in zip(offs, res)) + b''.join(struct.pack('<I', t) for _, _, t in res) + bytes(24 * tot) + names
    return struct.pack('>I', len(hdr)) + hdr


class FrostbiteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); r = Path(self.tmp.name)
        self.code = struct.pack('<I', 0x12345678) + bytes(0x60)
        self.ctl = bytes.fromhex('34127698') + bytes(0x87c); self.ctl = self.ctl[:0x704] + struct.pack('<I', len(self.code)) + self.ctl[0x708:]
        self.shader = vnvn(self.code, self.ctl); tex = b'texture data'
        res = [('shaders/bytecode/0001', len(self.shader), fb.COMPILED_BYTECODE), ('textures/a', len(tex), fb.name_hash('Texture'))]
        man = manifest(res); items = [block(self.shader), block(tex)]
        cas = man + b''.join(items)
        (r / 'Data/NX/superbundlelayout/installpackage_00').mkdir(parents=True)
        (r / 'Data/NX/superbundlelayout/installpackage_00/cas_01.cas').write_bytes(cas)
        chunks = dbo_obj('installChunks', [dbo_obj(None, [dbo_str('name', 'superbundlelayout/installpackage_00'), dbo_str('installBundle', ''),
                                                         dbo_i32('persistentIndex', CHUNK - (1 << 32))])], kind=1)
        (r / 'Data/layout.toc').write_bytes(toc_file(dbo_obj(None, [dbo_obj('installManifest', [chunks])])))
        cid = bytes([0, 0]) + struct.pack('>IH', CHUNK, 1)
        ents = [(len(man), 0)] + [(len(i), len(man) + sum(len(x) for x in items[:k])) for k, i in enumerate(items)]
        n = len(ents); data = struct.pack('>9I', 0, 0, 0x24 + 8 + 8 * n, n, 0x24, 0x24, 0x24, 0, n) + cid
        for size, off in ents: data += struct.pack('>II', off, size)
        data += bytes([0x84] + [0] * (n - 1))
        body = struct.pack('>24I', 0, 0x60, 1, *([0] * 21)) + struct.pack('>IIQ', 0, 0x40000000 | len(data), 0x70) + data
        (r / 'Data/NX/test.toc').write_bytes(toc_file(body))
        self.root, self.items, self.man = str(r), items, man

    def tearDown(self): self.tmp.cleanup()

    def test_name_hash(self):
        self.assertEqual(fb.COMPILED_BYTECODE, 0x0e2b02f7)
        self.assertEqual(fb.name_hash('Texture'), 0x6bde20ba)
        self.assertEqual(fb.name_hash('ShaderBlockDepotResource'), 0x73312045)

    def test_resources_of_a_type(self):
        self.assertTrue(fb.is_frostbite(self.root))
        res = fb.resources(self.root, fb.COMPILED_BYTECODE)
        self.assertEqual(list(res.values()), [(len(self.shader), 'shaders/bytecode/0001')])
        (cas, off, stored), = res
        self.assertTrue(cas.endswith('installpackage_00/cas_01.cas'))
        self.assertEqual((off, stored), (len(self.man), len(self.items[0])))

    def test_read_and_nvn_shader(self):
        (cas, off, stored), (size, _) = next(iter(fb.resources(self.root, fb.COMPILED_BYTECODE).items()))
        d = Path(cas).read_bytes()
        b = fb.read(d, off, stored, size, None)
        self.assertEqual(b, self.shader)
        self.assertEqual(fb.nvn_shader(b), (self.code, self.ctl))

    def test_decoded_blocks(self):   # codec 0x19 goes to the decoder with the block's raw size
        data = b'0123456789' * 10
        d = block(data[:60], 0x19, b'A' * 7) + block(data[60:])
        seen = []
        def decode(comp, n): seen.append((comp, n)); return data[:n]
        self.assertEqual(fb.read(d, 0, len(d), len(data), decode), data)
        self.assertEqual(seen, [(b'A' * 7, 60)])

    def test_damaged_items(self):
        d = block(b'abc')
        with self.assertRaises(ValueError): fb.read(d, 0, len(d), 4, None)   # decodes to 3, not 4
        bad = d[:5] + b'\x60' + d[6:]   # the 0x70 marker broken
        with self.assertRaises(ValueError): fb.read(bad, 0, len(bad), 3, None)
        with self.assertRaises(ValueError): fb.read(block(b'abc', 0x22), 0, 11, 3, None)   # an unknown codec
        self.assertIsNone(fb.nvn_shader(b'not a shader' * 4))


if __name__ == '__main__':
    unittest.main()
