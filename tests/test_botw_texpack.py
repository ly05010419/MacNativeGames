"""Breath of the Wild texture pack builder (games/botw/scripts/botw_texpack.py) on small made-up pictures and a fake upscaler: no game data, no real model."""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/botw/scripts'))
import botw_texpack as tp      # noqa: E402

NVN_MTL = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'
LENGTHS = (0, 1, 7, 31, 32, 33, 100, 4096)
HARD_CODED = (0xda812a146f668251, 0x007b59654d24b601, 0x8cbdd07534315025, 0xc3dd7ca350c91fd6,      # the C function's hashes of pattern(n) for LENGTHS
              0x62d5b638c9953066, 0x2320f475120411ca, 0xe93e6c20a9092f4e, 0x125d7bed6f3bdaf1)


def pattern(n):
    return bytes((i*131 + n*7 + 3) & 255 for i in range(n))


def c_function():
    text = NVN_MTL.read_text()
    start = text.index('static uint64_t pkg_hash(const void *p, uint64_t n) {')
    depth, end = 0, start
    for end in range(text.index('{', start), len(text)):
        depth += {'{': 1, '}': -1}.get(text[end], 0)
        if depth == 0:
            break
    return text[start:end + 1]


def png_bytes(width, height, nc, rows):
    """A PNG from already filtered rows (each a filter byte and the data), for the decoder tests."""
    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, tp.COLOUR_TYPE[nc], 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))


def filter_rows(data, width, height, nc, kind):
    """Rows filtered with one PNG filter type, written from the PNG specification (not from the decoder)."""
    stride, rows, prev = width*nc, [], bytes(width*nc)
    for y in range(height):
        line = data[y*stride:(y + 1)*stride]
        out = bytearray([kind])
        for i in range(stride):
            a = line[i - nc] if i >= nc else 0
            b = prev[i]
            c = prev[i - nc] if i >= nc else 0
            if kind == 0: pred = 0
            elif kind == 1: pred = a
            elif kind == 2: pred = b
            elif kind == 3: pred = (a + b)//2
            else:
                p = a + b - c
                pred = min((abs(p - a), 0, a), (abs(p - b), 1, b), (abs(p - c), 2, c))[2]
            out.append((line[i] - pred) & 255)
        rows.append(bytes(out))
        prev = line
    return rows


def fake_upscaler(model, in_path, out_path):
    """Nearest-neighbour 4x, in place of realesrgan-ncnn-vulkan."""
    fake_upscaler.calls.append((model, tp.read_png(in_path).width))
    img = tp.read_png(in_path)
    rows = []
    for y in range(img.height):
        row = b''.join(img.data[(y*img.width + x)*img.channels:(y*img.width + x + 1)*img.channels]*4 for x in range(img.width))
        rows += [row]*4
    tp.write_png(out_path, tp.Image(img.width*4, img.height*4, img.channels, b''.join(rows)))


fake_upscaler.calls = []


def nearest(img, factor):
    rows = []
    for y in range(img.height):
        row = b''.join(img.data[(y*img.width + x)*img.channels:(y*img.width + x + 1)*img.channels]*factor for x in range(img.width))
        rows += [row]*factor
    return b''.join(rows)


class HashTest(unittest.TestCase):
    def test_matches_c_function(self):
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('no C compiler (cc) on this machine: the hash cannot be compared with the C function')
        calls = ''.join(f'{{ static uint8_t b[{max(n, 1)}]; for (int i = 0; i < {n}; i++) b[i] = (uint8_t)((i*131 + {n}*7 + 3) & 255); printf("%016llx\\n", (unsigned long long)pkg_hash(b, {n})); }}\n' for n in LENGTHS)
        source = f'#include <stdint.h>\n#include <string.h>\n#include <stdio.h>\n{c_function()}\nint main(void) {{\n{calls}return 0;}}\n'
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'h.c').write_text(source)
            subprocess.run([cc, '-O1', '-o', str(Path(tmp, 'h')), str(Path(tmp, 'h.c'))], check=True, capture_output=True)
            out = subprocess.run([str(Path(tmp, 'h'))], check=True, capture_output=True, text=True).stdout.split()
        self.assertEqual([int(v, 16) for v in out], [tp.pkg_hash(pattern(n)) for n in LENGTHS])
        self.assertEqual([int(v, 16) for v in out], list(HARD_CODED))      # also keeps the C function from changing unnoticed

    def test_hard_coded_values(self):
        self.assertEqual([tp.pkg_hash(pattern(n)) for n in LENGTHS], list(HARD_CODED))

    def test_key(self):
        self.assertEqual(tp.texpack_key(b'', 48, 72, 0x49), 'da812a146f668251-48x72-49')
        self.assertTrue(re.fullmatch(r'[0-9a-f]{16}-\d+x\d+-[0-9a-f]{2}', tp.texpack_key(pattern(33), 4, 4, 9)))


class PngTest(unittest.TestCase):
    def test_round_trip_layouts(self):
        for nc in (1, 3, 4):
            data = bytes((i*37 + nc) & 255 for i in range(5*3*nc))
            with tempfile.TemporaryDirectory() as tmp:
                tp.write_png(Path(tmp, 'a.png'), tp.Image(5, 3, nc, data))
                self.assertEqual(tp.read_png(Path(tmp, 'a.png')), tp.Image(5, 3, nc, data))

    def test_all_filter_types(self):
        for nc in (1, 2, 3, 4):
            data = random.Random(nc).randbytes(6*5*nc) if nc != 3 else bytes(i % 5 * 3 for i in range(6*5*nc))      # random bytes reach the Paeth ties
            for kind in range(5):
                with self.subTest(channels=nc, filter=kind), tempfile.TemporaryDirectory() as tmp:
                    Path(tmp, 'a.png').write_bytes(png_bytes(6, 5, nc, filter_rows(data, 6, 5, nc, kind)))
                    self.assertEqual(tp.read_png(Path(tmp, 'a.png')), tp.Image(6, 5, nc, data))

    def test_refuses_unsupported(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'a.png').write_bytes(b'not a png')
            with self.assertRaises(tp.TexpackError):
                tp.read_png(Path(tmp, 'a.png'))
            good = png_bytes(2, 2, 1, filter_rows(bytes(4), 2, 2, 1, 0))
            Path(tmp, 'b.png').write_bytes(good[:16] + b'\x10' + good[17:])      # bit depth 16 (and a bad CRC)
            with self.assertRaises(tp.TexpackError):
                tp.read_png(Path(tmp, 'b.png'))


class PlaneTest(unittest.TestCase):
    def test_box_reduce(self):
        plane = bytes([0, 1, 10, 20, 2, 3, 30, 41, 100, 100, 7, 7, 100, 101, 7, 8])      # 4x4
        self.assertEqual(tp.box_reduce(plane, 4, 4), bytes([2, 25, 100, 7]))       # block sums 6, 101, 401, 29 over 4, rounded: 1.5 -> 2, 25.25 -> 25, 100.25, 7.25
        with self.assertRaises(tp.TexpackError):
            tp.box_reduce(bytes(9), 3, 3)

    def test_pad_and_crop(self):
        plane = bytes([1, 2, 3, 4, 5, 6])      # 3x2
        padded = tp.pad_plane(plane, 3, 2, 2)
        self.assertEqual(padded[:7], bytes([1, 1, 1, 2, 3, 3, 3]))
        self.assertEqual(len(padded), 7*6)
        self.assertEqual(padded[-7:], bytes([4, 4, 4, 5, 6, 6, 6]))
        self.assertEqual(tp.crop_plane(padded, 7, 6, 2), plane)

    def test_mean_plane(self):
        self.assertEqual(tp.mean_plane([bytes([0, 10]), bytes([1, 10]), bytes([1, 11])]), bytes([1, 10]))

    def test_local_max_and_cap(self):
        plane = bytes([0, 0, 0, 0, 160, 0, 0, 0, 0, 0, 0, 255])      # 4x3: a grey texel and a white corner
        self.assertEqual(tp.local_max(plane, 4, 3), bytes([160, 160, 0, 0, 160, 160, 255, 255, 160, 160, 255, 255]))
        big = bytes([255]) * (8 * 6)      # the model made everything white, 2x
        capped = tp.cap_tones(big, plane, 4, 3, 2)
        self.assertEqual(capped[:8], bytes([160, 160, 160, 160, 0, 0, 0, 0]))      # grey stays grey, empty stays empty
        self.assertEqual(capped[-2:], bytes([255, 255]))      # white stays white
        self.assertEqual(tp.cap_tones(bytes([10, 20, 30, 40]), bytes([200]), 1, 1, 2), bytes([10, 20, 30, 40]))      # darker than the cap: unchanged

    def test_sharpness(self):
        self.assertIsNone(tp.sharpness(bytes([7]) * 4, 2, 2))
        self.assertEqual(tp.sharpness(bytes([0, 255, 0, 0]), 2, 2), 1.0)
        self.assertAlmostEqual(tp.sharpness(bytes([0, 50, 100, 150, 200, 250]), 6, 1), 0.2)      # a ramp: soft
        self.assertEqual(tp.sharpness(bytes([0, 255]), 1, 2), 1.0)      # a 1-wide strip has no horizontal neighbours
        self.assertAlmostEqual(tp.sharpness(bytes(range(0, 248, 8)), 1, 31), 8 / 240)


class SkipRuleTest(unittest.TestCase):
    def test_small_soft_and_flat_are_skipped_unless_overridden(self):
        with tempfile.TemporaryDirectory() as tmp:
            harvest, out = Path(tmp, 'h'), Path(tmp, 'p'); harvest.mkdir()
            sharp = bytes(255 if 6 <= x < 14 else 0 for y in range(20) for x in range(20))
            soft = bytes(min(255, x * 13) for y in range(20) for x in range(20))
            pics = {'tiny-16x16-49': tp.Image(16, 16, 1, bytes(i & 255 for i in range(256))), 'soft-20x20-49': tp.Image(20, 20, 1, soft),
                    'flat-20x20-49': tp.Image(20, 20, 1, bytes(20 * 20)), 'sharp-20x20-49': tp.Image(20, 20, 1, sharp)}
            lines = [BuildTest.HEADER]
            for key, img in pics.items():
                tp.write_png(harvest/f'{key}.png', img)
                lines.append(f'{key}\t{img.width}\t{img.height}\t0x49\t140\tmask\t0\t87d90016\t100\t1\n')
            (harvest/'harvest.tsv').write_text(''.join(lines))
            log = []
            with mock.patch.object(tp, 'run_upscaler', fake_upscaler):
                tp.build(harvest, out, log=log.append)
                self.assertEqual(sorted(p.name for p in out.glob('*.png')), ['sharp-20x20-49.png'])
                self.assertTrue(any('tiny-16x16-49: skip (small' in line for line in log))
                self.assertTrue(any('soft-20x20-49: skip (soft, sharpness 0.05' in line for line in log))
                self.assertTrue(any('flat-20x20-49: skip (flat' in line for line in log))
                (harvest/'overrides.tsv').write_text('tiny-16x16-49\t4x\nsoft-20x20-49\trealesrgan-x4plus-anime\n')
                tp.build(harvest, out, log=log.append)
            self.assertTrue((out/'tiny-16x16-49.png').is_file() and (out/'soft-20x20-49.png').is_file())


class ChoiceTest(unittest.TestCase):
    def row(self, cls, w=48, h=48, metal=10):
        return tp.Row('k', w, h, metal, cls)

    def test_model_by_class(self):
        self.assertEqual(tp.choose(self.row('mask'))[:2], ('grey', ('realesrgan-x4plus-anime',)))
        for metal in (30, 142):
            self.assertEqual(tp.choose(self.row('mask', metal=metal))[:2], ('rg', ('realesrgan-x4plus-anime',)))
        self.assertEqual(tp.choose(self.row('mask', metal=140))[0], 'grey')
        for metal in (70, 71, 130, 135, 152, 153):
            self.assertEqual(tp.choose(self.row('colour', metal=metal))[:2], ('rgba', ('4x-UltraSharp', 'realesrgan-x4plus-anime')))

    def test_scale_rule(self):
        self.assertEqual(tp.choose(self.row('mask', 128, 8))[2], 4)
        self.assertEqual(tp.choose(self.row('mask', 8, 128))[2], 4)
        self.assertEqual(tp.choose(self.row('mask', 129, 8))[2], 2)
        self.assertEqual(tp.choose(self.row('colour', 8, 512))[2], 2)

    def test_override_parsing(self):
        self.assertEqual(tp.parse_override('skip'), tp.Override(skip=True))
        self.assertEqual(tp.parse_override('2x'), tp.Override(factor=2))
        self.assertEqual(tp.parse_override('realesrgan-x4plus'), tp.Override(model='realesrgan-x4plus'))
        self.assertEqual(tp.parse_override('realesrgan-x4plus,4x'), tp.Override(model='realesrgan-x4plus', factor=4))
        for bad in ('2x,4x', 'a,b', 'a,,2x', ''):
            with self.subTest(bad), self.assertRaises(ValueError):
                tp.parse_override(bad)

    def test_override_file_and_precedence(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, 'o.tsv')
            path.write_text('# comment\n\nk1\tskip\nk2\t2x\nk3\tmy-model,4x\n')
            table = tp.read_overrides(path)
            self.assertEqual(sorted(table), ['k1', 'k2', 'k3'])
            self.assertTrue(table['k1'].skip)
            self.assertEqual(tp.choose(self.row('mask', 48, 48), table['k2'])[2], 2)        # a small picture made 2x
            self.assertEqual(tp.choose(self.row('mask', 300, 300), table['k3']), ('grey', ('my-model',), 4))
            self.assertEqual(tp.choose(self.row('colour'), table['k3']), ('rgba', ('my-model', 'realesrgan-x4plus-anime'), 4))      # the alpha keeps the mask model
            path.write_text('k1 skip\n')
            with self.assertRaises(tp.TexpackError):
                tp.read_overrides(path)


class RunUpscalerTest(unittest.TestCase):
    def test_command_line_and_missing_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            models = Path(tmp, 'models'); models.mkdir()
            tp.TOOL.update(upscaler=Path('/x/realesrgan-ncnn-vulkan'), models=models)
            with self.assertRaises(tp.TexpackError) as caught:
                tp.run_upscaler('4x-UltraSharp', Path(tmp, 'i.png'), Path(tmp, 'o.png'))
            self.assertIn('4x-UltraSharp', str(caught.exception))
            (models/'m.param').write_text(''); (models/'m.bin').write_text('')
            Path(tmp, 'o.png').write_text('')
            with mock.patch('subprocess.run', return_value=mock.Mock(returncode=0, stderr='')) as run:
                tp.run_upscaler('m', Path(tmp, 'i.png'), Path(tmp, 'o.png'))
            self.assertEqual(run.call_args.args[0], ['/x/realesrgan-ncnn-vulkan', '-i', str(Path(tmp, 'i.png')), '-o', str(Path(tmp, 'o.png')), '-n', 'm', '-s', '4', '-m', str(models)])


class BuildTest(unittest.TestCase):
    HEADER = 'key\twidth\theight\tnvn_format\tmetal_format\tclass\tsrgb\tfs_key\tbytes\tframe\n'

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.harvest, self.out = Path(self.tmp.name, 'harvest'), Path(self.tmp.name, 'pack')
        self.harvest.mkdir()
        self.images = {
            'aaaa-6x5-0a': (tp.Image(6, 5, 1, bytes((i*9) & 255 for i in range(30))), 'mask', 10, 1),
            'bbbb-5x4-0b': (tp.Image(5, 4, 3, bytes((i*5 + 1) & 255 if i % 3 != 2 else 0 for i in range(60))), 'mask', 30, 3),
            'cccc-4x4-0c': (tp.Image(4, 4, 4, bytes((i*11 + 3) & 255 for i in range(64))), 'colour', 70, 4),
            'dddd-130x3-0a': (tp.Image(130, 3, 1, bytes((i*3) & 255 for i in range(390))), 'mask', 10, 1),
        }
        lines = [self.HEADER]
        for key, (img, cls, metal, _) in self.images.items():
            tp.write_png(self.harvest/f'{key}.png', img)
            lines.append(f'{key}\t{img.width}\t{img.height}\t0x49\t{metal}\t{cls}\t0\t87d90016\t100\t1\n')
        (self.harvest/'harvest.tsv').write_text(''.join(lines))
        fake_upscaler.calls.clear()
        patcher = mock.patch.object(tp, 'run_upscaler', fake_upscaler); patcher.start(); self.addCleanup(patcher.stop)
        for name, value in (('TINY', 0), ('SOFT', 0)):      # the made-up pictures are tiny; SkipRuleTest covers the skips
            patcher = mock.patch.object(tp, name, value); patcher.start(); self.addCleanup(patcher.stop)
        self.log = []

    def run_build(self, **kw):
        return tp.build(self.harvest, self.out, log=self.log.append, **kw)

    def test_full_build(self):
        self.assertEqual(self.run_build(), 0)
        for key, (img, cls, metal, nc) in self.images.items():
            factor = 2 if img.width > 128 else 4
            result = tp.read_png(self.out/f'{key}.png')
            self.assertEqual((result.width, result.height, result.channels), (img.width*factor, img.height*factor, img.channels), key)
            self.assertEqual(result.data, nearest(img, factor), key)      # the fake is nearest-neighbour, so padding, crop, averaging and reduction must give it back
        self.assertEqual({m for m, _ in fake_upscaler.calls}, {'realesrgan-x4plus-anime', '4x-UltraSharp'})
        self.assertTrue(all(w == 6 + 8 or w == 5 + 8 or w == 4 + 8 or w == 130 + 8 for _, w in fake_upscaler.calls))      # padded by PAD on both sides
        manifest = tp.read_manifest(self.out/tp.MANIFEST)
        self.assertEqual(sorted(manifest), sorted(self.images))
        self.assertEqual(manifest['cccc-4x4-0c'][:2], ('4x-UltraSharp+realesrgan-x4plus-anime', '4'))
        self.assertEqual(manifest['dddd-130x3-0a'][:2], ('realesrgan-x4plus-anime', '2'))
        self.assertEqual(manifest['aaaa-6x5-0a'][2], str((self.harvest/'aaaa-6x5-0a.png').stat().st_mtime_ns))

    def test_current_entries_are_skipped_and_force_rebuilds(self):
        self.run_build()
        calls = len(fake_upscaler.calls)
        self.run_build()
        self.assertEqual(len(fake_upscaler.calls), calls)
        self.assertIn('4 current', self.log[-1])
        self.run_build(force=True)
        self.assertGreater(len(fake_upscaler.calls), calls)
        calls = len(fake_upscaler.calls)
        source = self.harvest/'aaaa-6x5-0a.png'
        os.utime(source, ns=(1, 1))      # a source that changed
        self.run_build()
        self.assertGreater(len(fake_upscaler.calls), calls)
        self.assertIn('1 built, 3 current', self.log[-1])

    def test_an_older_manifest_row_is_rebuilt(self):
        self.run_build()
        path = self.out/tp.MANIFEST
        rows = path.read_text().splitlines()
        path.write_text('\n'.join([rows[0]] + ['\t'.join(r.split('\t')[:4]) for r in rows[1:]]) + '\n')      # rows written before REVISION
        calls = len(fake_upscaler.calls)
        self.run_build()
        self.assertGreater(len(fake_upscaler.calls), calls)
        self.assertIn('4 built', self.log[-1])
        self.assertTrue(all(entry[3] == tp.REVISION for entry in tp.read_manifest(path).values()))

    def test_never_overwrites_a_file_the_manifest_does_not_list(self):
        self.out.mkdir()
        (self.out/'aaaa-6x5-0a.png').write_bytes(b'vector tool')
        self.run_build(force=True)
        self.assertEqual((self.out/'aaaa-6x5-0a.png').read_bytes(), b'vector tool')
        self.assertNotIn('aaaa-6x5-0a', tp.read_manifest(self.out/tp.MANIFEST))
        self.assertTrue((self.out/'bbbb-5x4-0b.png').is_file())
        self.assertTrue(any('not in ai-manifest.tsv' in line for line in self.log))

    def test_overrides_only_and_dry_run(self):
        (self.harvest/'overrides.tsv').write_text('# x\naaaa-6x5-0a\tskip\nbbbb-5x4-0b\t2x\ncccc-4x4-0c\tother-model,2x\n')
        self.run_build(only=['aaaa-6x5-0a', 'bbbb-5x4-0b', 'cccc-4x4-0c'], dry_run=True)
        self.assertFalse(self.out.exists())
        self.assertEqual(fake_upscaler.calls, [])
        self.assertIn('2 to build', self.log[-1])
        self.run_build(only=['aaaa-6x5-0a', 'bbbb-5x4-0b', 'cccc-4x4-0c'])
        self.assertFalse((self.out/'aaaa-6x5-0a.png').exists())
        self.assertEqual(tp.read_png(self.out/'bbbb-5x4-0b.png').width, 10)
        self.assertEqual(tp.read_manifest(self.out/tp.MANIFEST)['cccc-4x4-0c'][:2], ('other-model+realesrgan-x4plus-anime', '2'))
        self.assertFalse((self.out/'dddd-130x3-0a.png').exists())
        with self.assertRaises(tp.TexpackError):
            self.run_build(only=['nope'])

    def test_wrong_layout_is_reported(self):
        tp.write_png(self.harvest/'cccc-4x4-0c.png', tp.Image(4, 4, 3, bytes(48)))      # RGB where the tsv says colour
        self.assertEqual(self.run_build(), 1)
        self.assertFalse((self.out/'cccc-4x4-0c.png').exists())
        self.assertTrue(any('cccc-4x4-0c: FAILED' in line for line in self.log))
        self.assertTrue((self.out/'aaaa-6x5-0a.png').is_file())      # the others still get built


if __name__ == '__main__':
    unittest.main()
