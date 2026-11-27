"""games/oot/scripts/make_vr_icons.py and its ASTC decoder (astc_ldr.py) on made-up data (no game data): the decoder against
blocks built by hand with known output and against Metal's own ASTC decoder (its answers for six made-up blocks, read on an
M-series Mac 2026-11-27, written here as 8-bit values), the Switch 2 block-linear order, the BNTX header, the shrinking, the
PNG, and a made-up RomFS."""
import compression.zstd as zstd
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / 'games/oot/scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('make_vr_icons', SCRIPTS / 'make_vr_icons.py')
icons = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icons)
astc = icons.astc_ldr

# Metal's ASTC 4x4 LDR decode of made-up blocks, rounded to 8 bits (scratch/runs/2026-11/oot-vr2/icons-a, astcref).
METAL = {
    'ddeb7af44ad079f905c7585d9b259e14': [  # two partitions
        (41, 51, 84, 169), (48, 58, 96, 163), (73, 89, 147, 134), (121, 148, 242, 80), (96, 117, 191, 109), (65, 42, 88, 139),
        (70, 45, 96, 139), (104, 128, 209, 99), (59, 73, 120, 149), (59, 73, 120, 149), (59, 73, 120, 149), (59, 73, 120, 149),
        (114, 139, 227, 88), (89, 110, 179, 115), (76, 93, 153, 131), (67, 82, 135, 141)],
    'dd555b32adf6755613d05134b52a8f7b': [  # three partitions
        (164, 136, 182, 255), (164, 131, 182, 255), (166, 131, 182, 255), (170, 136, 182, 255), (196, 182, 164, 255),
        (205, 175, 173, 255), (205, 175, 173, 255), (27, 128, 109, 255), (164, 129, 182, 255), (169, 124, 181, 255),
        (169, 126, 181, 255), (164, 136, 182, 255), (196, 172, 178, 255), (205, 179, 168, 255), (205, 176, 172, 255),
        (27, 137, 127, 255)],
    '8ec72c48c48a4999a6aa013e9cd653b1': [  # dual plane
        (21, 35, 88, 255), (20, 34, 92, 255), (20, 33, 91, 255), (20, 34, 84, 255), (21, 34, 85, 255), (20, 34, 89, 255),
        (20, 33, 90, 255), (19, 32, 85, 255), (20, 33, 82, 255), (20, 34, 86, 255), (20, 33, 87, 255), (17, 29, 86, 255),
        (20, 32, 79, 255), (20, 34, 84, 255), (19, 32, 86, 255), (16, 27, 88, 255)],
    '42009115c828f43d00000000ff55aa00': [(20, 25, 30, 255)] * 4 + [(87, 74, 102, 255)] * 4 + [(158, 126, 178, 255)] * 4 + [(225, 175, 250, 255)] * 4,
}


def rgb_block(values, weights):
    """Block mode 0x42 (a 4x4 weight grid, weight range 4), one partition, CEM 8 (RGB direct), six 8-bit colour values (range
    256: 48 bits fit in the 79 left), sixteen 2-bit weights stored bit-reversed from the top."""
    v = 0x42 | (8 << 13)
    for i, c in enumerate(values):
        v |= c << (17 + 8 * i)
    for i, w in enumerate(weights):
        for j in range(2):
            v |= ((w >> j) & 1) << (127 - (2 * i + j))
    return v.to_bytes(16, 'little')


def void_block(r, g, b, a):
    return (0x1fc | (3 << 10) | (((1 << 52) - 1) << 12) | (r << 64) | (g << 80) | (b << 96) | (a << 112)).to_bytes(16, 'little')


def bntx(width, height, blocks_of, fmt=0x2d06, layout=0):
    """A one-texture BNTX: blocks_of(bx, by) -> 16 bytes, laid out block-linear (block height 1 << layout GOBs)."""
    bw, bh, gobs = (width + 3) // 4, (height + 3) // 4, 1 << layout
    size = icons.padded_size(bw, bh, gobs)
    data = bytearray(size)
    for by in range(bh):
        for bx in range(bw):
            at = icons.gob_offset(bx * 16, by, (bw * 16 + 63) // 64, gobs)
            data[at:at + 16] = blocks_of(bx, by)
    out = bytearray(0x200)
    out[:4] = b'BNTX'
    brti = 0x40
    out[brti:brti + 4] = b'BRTI'
    struct.pack_into('<BHHHIIIIIIII', out, brti + 0x11, 2, 0, 0, 1, 1, fmt, 0, width, height, 1, 1, layout)
    struct.pack_into('<I', out, brti + 0x50, size)
    struct.pack_into('<Q', out, brti + 0x70, 0x100)
    struct.pack_into('<Q', out, 0x100, 0x200)
    return bytes(out + data)


def read_png(data):
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    width, height, depth, kind = struct.unpack('>IIBB', data[16:26])
    at, body = 8, b''
    while at < len(data):
        n = struct.unpack('>I', data[at:at + 4])[0]
        if data[at + 4:at + 8] == b'IDAT':
            body += data[at + 8:at + 8 + n]
        at += 12 + n
    raw = zlib.decompress(body)
    return width, height, depth, kind, b''.join(raw[y * (width * 4 + 1) + 1:(y + 1) * (width * 4 + 1)] for y in range(height))


class Decoder(unittest.TestCase):
    def test_trits_and_quints_cover_every_digit_group(self):
        self.assertEqual(len(set(astc.TRITS)), 3 ** 5)
        self.assertEqual(len(set(astc.QUINTS)), 5 ** 3)
        self.assertTrue(all(0 <= d <= 2 for t in astc.TRITS for d in t) and all(0 <= d <= 4 for q in astc.QUINTS for d in q))

    def test_unquantized_ranges(self):
        for level in range(4, len(astc.LEVELS)):
            table = astc.COLOUR[level]
            self.assertEqual((len(table), min(table), max(table)), (astc.LEVELS[level][0], 0, 255))
        for level in range(12):
            table = astc.WEIGHT[level]
            self.assertEqual((len(table), min(table), max(table)), (astc.LEVELS[level][0], 0, 64))
        self.assertEqual(astc.WEIGHT[2], [0, 21, 43, 64])

    def test_a_block_built_by_hand(self):
        """Endpoints (0, 255, 10) and (255, 0, 250), weights 0..3 across: the specification's interpolation, worked out here."""
        texels = astc.decode_block(rgb_block((0, 255, 255, 0, 10, 250), [i % 4 for i in range(16)]))
        for i, texel in enumerate(texels):
            w = (0, 21, 43, 64)[i % 4]
            want = [((a << 8 | a) * (64 - w) + (b << 8 | b) * w + 32) >> 6 for a, b in ((0, 255), (255, 0), (10, 250), (255, 255))]
            self.assertEqual(list(texel), want)
        self.assertEqual(round(texels[1][0] / 257), 84)   # Metal: (84, 171, 89)

    def test_against_metal(self):
        for hexed, want in METAL.items():
            texels = astc.decode_block(bytes.fromhex(hexed))
            for got, metal in zip(texels, want):
                self.assertTrue(all(abs(round(c / 257) - m) <= 1 for c, m in zip(got, metal)), (hexed, got, metal))

    def test_blue_contraction(self):
        """Second endpoint's sum below the first's: swapped and blue-contracted ((r + b) / 2, (g + b) / 2, b)."""
        texels = astc.decode_block(rgb_block((200, 10, 100, 20, 250, 30), [(i // 4) % 4 for i in range(16)]))
        self.assertEqual([c >> 8 for c in texels[0][:3]], [20, 25, 30])
        self.assertEqual([c >> 8 for c in texels[15][:3]], [225, 175, 250])

    def test_void_extent_and_errors(self):
        self.assertEqual(astc.decode_block(void_block(0x1234, 0xabcd, 0x8000, 0xffff)), [(0x1234, 0xabcd, 0x8000, 0xffff)] * 16)
        self.assertEqual(astc.decode_block(bytes(16)), [astc.ERROR] * 16)   # block mode 0 is reserved
        hdr = (0x1fc | (1 << 9) | (3 << 10)).to_bytes(16, 'little')          # an HDR void extent in the LDR profile
        self.assertEqual(astc.decode_block(hdr), [astc.ERROR] * 16)

    def test_srgb_endpoints(self):
        """An sRGB texture expands an 8-bit endpoint with 0x80 below it: the top byte is still the endpoint."""
        texels = astc.decode_block(rgb_block((0, 255, 255, 0, 10, 250), [0] * 16), srgb=True)
        self.assertEqual(texels[0], (0x0080, 0xff80, 0x0a80, 0xffff))


class Script(unittest.TestCase):
    def test_the_switch_2_gob_order(self):
        """16-byte pieces x5 y2 x4 y1 y0 (games/oot/third_party/d2r_gfx/gfx/tex/nvn_deswizzle.c, GAME_NS2)."""
        self.assertEqual([icons.gob_offset(x, y, 1, 1) for x, y in ((0, 1), (0, 2), (16, 0), (0, 4), (32, 0))], [16, 32, 64, 128, 256])
        self.assertEqual(sorted(icons.gob_offset(x, y, 1, 1) for x in range(0, 64, 16) for y in range(8)), list(range(0, 512, 16)))
        self.assertEqual(icons.gob_offset(64, 0, 2, 2), 1024)     # the next GOB across skips a whole block (2 GOBs high)
        self.assertEqual(icons.gob_offset(0, 8, 2, 2), 512)       # the next GOB down stays inside the block

    def test_texture_and_blocks(self):
        marker = lambda bx, by: bytes([bx, by]) * 8
        data = bntx(20, 12, marker, layout=1)
        width, height, srgb, block_height, offset, size = icons.texture(data)
        self.assertEqual((width, height, srgb, block_height, offset), (20, 12, True, 2, 0x200))
        self.assertEqual(icons.linear_blocks(data, offset, 5, 3, block_height), [marker(bx, by) for by in range(3) for bx in range(5)])
        with self.assertRaisesRegex(ValueError, 'not a 2D ASTC'):
            icons.texture(bntx(8, 8, marker, fmt=0x2001))   # BC7
        with self.assertRaisesRegex(ValueError, 'not a BNTX'):
            icons.texture(b'FRES' + data[4:])
        with self.assertRaisesRegex(ValueError, 'smaller than its layout'):
            icons.texture(data[:-16])

    def test_decode_crops_and_shrink_weights_by_alpha(self):
        data = bntx(5, 5, lambda bx, by: void_block(0xff00, 0, 0, 0xffff) if bx == 0 else void_block(0, 0xff00, 0, 0))
        width, height, srgb, bh, offset, _ = icons.texture(data)
        rgba = icons.decode(width, height, icons.linear_blocks(data, offset, 2, 2, bh), srgb)
        self.assertEqual(len(rgba), 5 * 5 * 4)
        self.assertEqual(rgba[:4], bytes((255, 0, 0, 255)))
        self.assertEqual(rgba[16:20], bytes((0, 255, 0, 0)))   # x 4: the second block column, transparent green
        # two pixels into one: red opaque, green transparent -> red at half alpha (no green bleeding in)
        self.assertEqual(icons.shrink(bytes((255, 0, 0, 255, 0, 255, 0, 0)), 2, 1, 1), bytearray((255, 0, 0, 128)))
        self.assertEqual(icons.shrink(bytes(8), 2, 1, 1), bytearray(4))
        for source, size in ((341, 128), (1440, 128), (100, 128)):
            spans = icons.taps(source, size)
            self.assertAlmostEqual(sum(share for t in spans for _, share in t), source)
            self.assertTrue(all(0 <= i < source for t in spans for i, _ in t))

    def test_png(self):
        rgba = bytes(range(2 * 3 * 4))
        self.assertEqual(read_png(icons.png(2, 3, rgba)), (2, 3, 8, 6, rgba))

    def make_romfs(self, folder, files):
        index, image = [], b''
        for path, raw in [(icons.make_vr_rig.DICTIONARIES, zstd.compress(b'SARC' + struct.pack('<HHII', 0x14, 0xfeff, 0x28, 0x28)
                                                                          + struct.pack('<HH', 0x100, 0) + b'SFAT' + struct.pack('<HHI', 12, 0, 0x65)
                                                                          + b'SFNT' + struct.pack('<HH', 8, 0)))] + list(files.items()):
            index.append({'path': path, 'offset': len(image), 'size': len(raw)})
            image += raw
        (folder / 'romfs.bin').write_bytes(image)
        (folder / 'index.json').write_text(json.dumps({'files': index}))

    def test_generate_from_a_made_up_romfs(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            blue = bntx(8, 8, lambda bx, by: void_block(0, 0, 0xff00, 0xffff))
            self.make_romfs(tmp, {icons.icon_path('Item_Bomb'): zstd.compress(blue),
                                  icons.icon_path('Item_Hammer'): zstd.compress(b'BNTX' + bytes(60))})
            out = tmp / 'vr-icons'
            written, missing = icons.generate(tmp, out, names=('Item_Bomb', 'Item_Hammer', 'Item_Bombchu'), workers=1)
            self.assertEqual(written, 1)
            self.assertEqual(missing[0], 'Item_Bombchu')
            self.assertTrue(missing[1].startswith('Item_Hammer ('))
            width, height, _, _, pixels = read_png((out / 'Item_Bomb.png').read_bytes())
            self.assertEqual((width, height, pixels[:4]), (icons.SIZE, icons.SIZE, bytes((0, 0, 255, 255))))
            self.assertEqual(icons.generate(tmp, out, names=('Item_Bomb',), workers=1), (0, []))   # unchanged: not rewritten

    def test_stays_out_of_the_checkout(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'outside the source checkout'):
                icons.generate(tmp, ROOT / 'games/oot/vr-icons')


if __name__ == '__main__':
    unittest.main()
