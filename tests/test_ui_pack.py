"""tools/ui_pack (the UI texture pack builder): the key and the key lists on stdlib only; the shape method and the incremental
build on small made-up pictures (no game data), when numpy, scipy, ContourPy and pillow are installed (tools/deps python ui-pack)."""
import ast
import contextlib
import importlib.util
import io
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from tools.ui_pack import harvest, keylist, manifest
from tools.ui_pack.errors import UiPackError

ROOT = Path(__file__).resolve().parents[1]
NVN_MTL = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'
LENGTHS = (0, 1, 7, 31, 32, 33, 100, 4096)
HARD_CODED = (0xda812a146f668251, 0x007b59654d24b601, 0x8cbdd07534315025, 0xc3dd7ca350c91fd6,      # the C function's hashes of pattern(n) for LENGTHS
              0x62d5b638c9953066, 0x2320f475120411ca, 0xe93e6c20a9092f4e, 0x125d7bed6f3bdaf1)
KEY_FORMAT = r'^[0-9a-f]{16}-\d+x\d+-[0-9a-f]{2}$'
HARVEST_HEADER = 'key\twidth\theight\tnvn_format\tmetal_format\tclass\tsrgb\tfs_key\tbytes\tframe\n'      # as the game writes it (ui_pack.h)
HAVE_METHOD = all(importlib.util.find_spec(name) for name in ('numpy', 'scipy', 'contourpy', 'PIL'))
NEED_METHOD = 'numpy, scipy, ContourPy and pillow are not installed (tools/deps python ui-pack)'


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


def games():
    """GAMES of tools/ui_pack/cli.py, read without importing it (cli.py imports numpy)."""
    for node in ast.parse((ROOT/'tools/ui_pack/cli.py').read_text()).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'GAMES' for t in node.targets):
            return ast.literal_eval(node.value)
    raise AssertionError('GAMES not found in tools/ui_pack/cli.py')


def write_keys(path, entries, reference=1080, default=0.8):
    """A key list file: entries are (key, scale or None, options dict)."""
    lines = [f'# reference-height: {reference}', f'# default-scale: {default}']
    lines += [f'{key}\t{"" if scale is None else scale}\t{json.dumps(options)}\tmade-up' for key, scale, options in entries]
    Path(path).write_text('\n'.join(lines) + '\n')
    return Path(path)


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
        self.assertEqual([int(v, 16) for v in out], [harvest.pkg_hash(pattern(n)) for n in LENGTHS])
        self.assertEqual([int(v, 16) for v in out], list(HARD_CODED))      # also keeps the C function from changing unnoticed

    def test_hard_coded_values(self):
        self.assertEqual([harvest.pkg_hash(pattern(n)) for n in LENGTHS], list(HARD_CODED))

    def test_key(self):
        self.assertEqual(harvest.texpack_key(b'', 48, 72, 0x49), 'da812a146f668251-48x72-49')
        self.assertRegex(harvest.texpack_key(pattern(33), 4, 4, 9), KEY_FORMAT)


class ShippedKeyListTest(unittest.TestCase):
    EXPECTED = {'botw': (47, 1080), 'totk-1.4.3': (81, 1944)}

    def test_games_and_paths(self):
        self.assertEqual(sorted(games()), sorted(self.EXPECTED))
        for game, path in games().items():
            self.assertTrue((ROOT/path).is_file(), f'{game}: {path}')

    def test_lists_parse(self):
        for game, path in games().items():
            with self.subTest(game):
                count, reference = self.EXPECTED[game]
                listed = keylist.read_keylist(ROOT/path)      # raises on a bad line, an unknown option or a repeated key
                self.assertEqual((len(listed.entries), listed.reference_height), (count, reference))
                self.assertGreater(listed.default_scale, 0)
                for key, entry in listed.entries.items():
                    self.assertRegex(key, KEY_FORMAT)
                    self.assertEqual(entry.key, key)
                    self.assertTrue(entry.scale is None or entry.scale > 0, key)
                    self.assertTrue(entry.name, key)
                    for name in entry.options:
                        self.assertTrue(name in keylist.OPTIONS or keylist.LEVELS.match(name), f'{key}: {name}')

    def test_keys_unique_in_the_file(self):
        for game, path in games().items():
            keys = [line.split('\t')[0] for line in (ROOT/path).read_text().splitlines() if line.strip() and not line.lstrip().startswith('#')]
            self.assertEqual(len(keys), len(set(keys)), game)


class KeyListFormatTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name, 'keys.tsv')

    def read(self, text):
        self.path.write_text(text)
        return keylist.read_keylist(self.path)

    def refuse(self, text, *words):
        with self.assertRaises(UiPackError) as caught:
            self.read(text)
        for word in words:
            self.assertIn(word, str(caught.exception))

    HEAD = '# reference-height: 1080\n# default-scale: 1.5\n'

    def test_reads_a_list(self):
        listed = self.read(self.HEAD + '\n# a comment\n0000000000000001-48x48-49\t0.98\t{"F": 4, "levels0": [0, 255]}\tA\n0000000000000002-8x8-49\t\t{}\tB; where\n')
        self.assertEqual((listed.reference_height, listed.default_scale), (1080, 1.5))
        a, b = listed.entries['0000000000000001-48x48-49'], listed.entries['0000000000000002-8x8-49']
        self.assertEqual((a.scale, a.options, a.name), (0.98, {'F': 4, 'levels0': [0, 255]}, 'A'))
        self.assertEqual((b.scale, b.options, b.name), (None, {}, 'B; where'))
        self.assertAlmostEqual(keylist.draw_scale(a, listed, 2160), 1.96)      # screen px per texel at the screen height
        self.assertAlmostEqual(keylist.draw_scale(b, listed, 540), 0.75)      # the default scale

    def test_refuses_a_bad_file(self):
        row = '0000000000000001-48x48-49\t1\t{}\tA\n'
        self.refuse(self.HEAD + 'k\t1\t{}\n', ':3:', 'expected key')      # three cells
        self.refuse(self.HEAD + '\t1\t{}\tA\n', 'expected key')      # no key
        self.refuse(self.HEAD + 'k\tfast\t{}\tA\n', 'bad scale')
        self.refuse(self.HEAD + 'k\t-1\t{}\tA\n', 'positive')
        self.refuse(self.HEAD + 'k\t1\t{nope}\tA\n', 'not JSON')
        self.refuse(self.HEAD + 'k\t1\t[]\tA\n', 'JSON object')
        self.refuse(self.HEAD + row + row, 'twice')
        self.refuse('# default-scale: 1\n' + row, 'reference-height')
        self.refuse('# reference-height: 1080\n' + row, 'default-scale')
        self.refuse('# reference-height: 0\n# default-scale: 1\n' + row, 'reference-height')
        self.refuse('# reference-height: tall\n# default-scale: 1\n' + row, 'bad reference-height')
        with self.assertRaises(UiPackError):
            keylist.read_keylist(Path(self.tmp.name, 'absent.tsv'))

    def test_check_options(self):
        good = {'F': 4, 'levels0': [0, 255], 'levels12': [1, 2], 'accept': {'ink': 'a reason', 'gv': 'b'}, 'skip': 'colour art'}
        self.assertIs(keylist.check_options(good), good)
        self.assertEqual(keylist.check_options({}), {})
        for check in keylist.CHECKS:
            keylist.check_options({'accept': {check: 'x'}})
        for bad in ({'sharpen': 2}, {'level0': 1}, {'levels': 1}, {'accept': {'speed': 'x'}}, {'accept': {'ink': ''}}, {'accept': {'ink': 3}},
                    {'accept': ['ink']}, {'skip': ''}, {'skip': True}, {'skip': 0}, {'F': 16}, {'F': 0}, {'F': 2.5}, {'F': True}):
            with self.subTest(bad), self.assertRaises(UiPackError):
                keylist.check_options(bad)
        with self.assertRaises(UiPackError):
            keylist.check_options(['F'])
        with self.assertRaises(UiPackError) as caught:
            keylist.check_options({'sharpen': 2}, 'keys.tsv:7')
        self.assertIn('keys.tsv:7: unknown option "sharpen"', str(caught.exception))

    def test_a_list_with_an_unknown_option_is_refused(self):
        self.refuse(self.HEAD + 'k\t1\t{"sharpen": 2}\tA\n', ':3:', 'unknown option "sharpen"')
        self.refuse(self.HEAD + 'k\t1\t{"accept": {"ink": ""}}\tA\n', 'accept must map')
        self.refuse(self.HEAD + 'k\t1\t{"skip": ""}\tA\n', 'skip must be')


class HarvestReadTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name, 'harvest.tsv')

    def read(self, *lines, header=HARVEST_HEADER):
        self.path.write_text(header + ''.join(line + '\n' for line in lines))
        return harvest.read_harvest(self.path)

    def refuse(self, *lines, header=HARVEST_HEADER, where=''):
        with self.assertRaises(UiPackError) as caught:
            self.read(*lines, header=header)
        self.assertIn(where, str(caught.exception))

    def test_reads_rows(self):
        rows = self.read('aa-48x48-49\t48\t48\t0x49\t10\tmask\t0\t87d90016\t2304\t1', '', 'bb-72x72-49\t72\t72\t0x49\t70\tcolour\t1\te6a88e12\t20736\t5', 'aa-48x48-49\t48\t48\t0x49\t10\tmask\t0\t87d90016\t2304\t9')
        self.assertEqual(rows, [harvest.Row('aa-48x48-49', 48, 48, 10, 'mask'), harvest.Row('bb-72x72-49', 72, 72, 70, 'colour')])      # a repeated key counts once
        self.assertEqual(self.read(), [])      # a header only

    def test_columns_may_come_in_any_order(self):
        rows = self.read('mask\t10\t4\t6\tk1', header='class\tmetal_format\theight\twidth\tkey\n')
        self.assertEqual(rows, [harvest.Row('k1', 6, 4, 10, 'mask')])

    def test_refuses_a_bad_file(self):
        self.refuse(header='key\twidth\theight\tclass\n', where='the header must have the columns')      # no metal_format
        self.refuse(header='', where='the header must have the columns')
        self.refuse('k\t4\t4\t0x49\t10\tpicture', where=':2: class must be mask or colour')
        self.refuse('k\tfour\t4\t0x49\t10\tmask', where=':2: cannot read this line')
        self.refuse('k\t4\t4\t0x49\t10\tmask', 'k2\t4\t4\t0x49\tfast\tmask', where=':3: cannot read this line')
        self.refuse('k\t4', where=':2: cannot read this line')      # a short line
        with self.assertRaises(FileNotFoundError):
            harvest.read_harvest(Path(self.tmp.name, 'absent.tsv'))


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)

    def test_write_and_read(self):
        rows = {'b': manifest.Row('skipped', 'flat:\tno channel\nvaries'), 'a': manifest.Row('written', '', '8', '0.8', '1080', 'rt=1.00', 'deadbeef')}
        manifest.write(self.out, rows)
        lines = (self.out/manifest.MANIFEST).read_text().splitlines()
        self.assertEqual(lines[0], '\t'.join(manifest.HEADER))
        self.assertEqual([line.split('\t')[0] for line in lines[1:]], ['a', 'b'])      # sorted
        back = manifest.read(self.out)
        self.assertEqual(back['a'], rows['a'])
        self.assertEqual(back['b'].reason, 'flat: no channel varies')      # one cell: no tab or line break
        self.assertEqual(manifest.read(Path(self.tmp.name, 'absent')), {})

    def test_the_old_manifest_gives_legacy_rows_that_the_tool_owns(self):
        (self.out/manifest.OLD_MANIFEST).write_text('key\tmodel\tfactor\tsource\trevision\n' + 'k4\tm\t4\t1\n' + 'k5\tm\t4\t1\tr\n' + 'k7\ta\tb\tc\td\te\tf\n' + 'k6\ta\tb\tc\td\te\n' + 'k3\ta\tb\n')
        rows = manifest.read(self.out)
        self.assertEqual(sorted(rows), ['k4', 'k5', 'k7'])      # 4, 5 and 7 cells; no other length
        for row in rows.values():
            self.assertEqual((row.status, row.digest), ('legacy', ''))
            self.assertIn(row.status, manifest.OWNED)
        manifest.write(self.out, {'k4': manifest.Row('skipped', 'x')})
        self.assertEqual(manifest.read(self.out)['k4'].status, 'skipped')      # manifest.tsv wins over the old file
        manifest.remove_old(self.out)
        self.assertFalse((self.out/manifest.OLD_MANIFEST).exists())
        manifest.remove_old(self.out)      # no file: no error


# ---------------------------------------------------------------------------------------------------------------------------------
# Made-up pictures. Shapes are drawn with unit geometry of a 32 px picture (scaled to n), anti-aliased with 8x8 samples per pixel,
# and mapped to grey levels LO..HI (plateau levels that are not 0 and 255, so that "exact" means something).

LO, HI = 20, 235


def shape(name, n=32):
    import numpy as np
    s = n/32
    g = (np.arange(n*8) + 0.5)/8
    X, Y = np.meshgrid(g, g)
    r2 = (X - 16*s)**2 + (Y - 16*s)**2

    def aa(inside):
        return inside.reshape(n, 8, n, 8).mean(axis=(1, 3))
    if name == 'disc':
        c = aa(r2 <= (11*s)**2)
    elif name == 'ring':
        c = aa((r2 <= (12*s)**2) & (r2 >= (7*s)**2))
    elif name == 'rrect':
        a, b, r = 5*s, 27*s, 5*s
        cx, cy = np.clip(X, a + r, b - r), np.clip(Y, a + r, b - r)
        c = aa((X - cx)**2 + (Y - cy)**2 <= r*r)
    elif name == 'line':      # thin (3 px) and a little tilted: the edges must stay straight
        c = aa((np.abs(Y - (16.3*s + 0.1*(X - 16*s))) <= 1.5*s) & (X >= 4*s) & (X <= 28*s))
    elif name == 'N':
        stems = ((X >= 7*s) & (X <= 10.5*s)) | ((X >= 21.5*s) & (X <= 25*s))
        diagonal = np.abs((X - 7*s) - (Y - 5*s)*(18/22)) <= 3*s
        c = aa((stems | (diagonal & (X >= 7*s) & (X <= 25*s))) & (Y >= 5*s) & (Y <= 27*s))
    elif name == 'glow':      # a soft glow: a pure gradient
        c = np.exp(-((np.arange(n)[None, :] - 15.5*s)**2 + (np.arange(n)[:, None] - 15.5*s)**2)/(2*(7*s)**2))
    elif name == 'glowcore':      # a soft glow with a hard core
        c = np.clip(0.45*np.exp(-((np.arange(n)[None, :] - 15.5*s)**2 + (np.arange(n)[:, None] - 15.5*s)**2)/(2*(7*s)**2)) + 0.55*aa(r2 <= (4*s)**2), 0, 1)
    elif name == 'block':      # pixel-crisp: texel-exact edges, no anti-aliasing
        c = np.zeros((n, n)); c[round(7*s):round(20*s), round(9*s):round(27*s)] = 1
    else:
        raise KeyError(name)
    return np.round(LO + (HI - LO)*c).astype(np.uint8)


def save_png(path, array, mode='L'):
    from PIL import Image
    Image.fromarray(array, mode).save(path)


def load_png(path):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path))


@unittest.skipUnless(HAVE_METHOD, NEED_METHOD)
class MethodTest(unittest.TestCase):
    SD = 0.8      # screen px per texel: the shapes are drawn smaller than their texels, as the UI draws them

    @classmethod
    def setUpClass(cls):
        import numpy as np
        from tools.ui_pack import gate, gameview, pack, recon
        cls.np, cls.gate, cls.gameview, cls.pack, cls.recon = np, gate, gameview, pack, recon
        cls.fits = {name: cls.fit(name) for name in ('disc', 'ring', 'rrect', 'line', 'N', 'block', 'glowcore')}

    @classmethod
    def fit(cls, name):
        """The pack picture of a made-up shape: the stages of build.make, with the intermediate values."""
        src = cls.np.asarray(shape(name), float)[..., None]
        res = cls.recon.fit_key(src, {})
        F = cls.pack.factor(src)
        img, dn, _ = cls.pack.render(src, res, F, cls.SD, {})
        return dict(src=src, res=res, F=F, img=img, dn=dn, m=cls.gate.measure(src, res, img, dn, F, cls.SD))

    def crossing(self, line, mid):
        """Sub-texel index where a profile first rises through mid (None: never)."""
        np = self.np
        i = np.nonzero((line[1:] >= mid) & (line[:-1] < mid))[0]
        return None if not len(i) else i[0] + (mid - line[i[0]])/(line[i[0] + 1] - line[i[0]])

    def test_plateau_levels_are_exact(self):
        for name in ('disc', 'ring', 'rrect', 'line', 'N', 'block'):
            with self.subTest(name):
                f = self.fits[name]
                src, img = f['src'][..., 0], f['img'][..., 0]
                self.assertEqual((src.min(), src.max()), (LO, HI))
                self.assertEqual((img.min(), img.max()), (LO, HI))      # no overshoot and no lost plateau
                self.assertEqual(self.gate.ends(img), self.gate.ends(src))
                self.assertEqual(img.shape, (src.shape[0]*f['F'], src.shape[1]*f['F']))
                self.assertEqual(self.gate.verdict(f['m']), {})      # the gate passes a faithful pack
                self.assertGreater((img == HI).mean(), 0.3*(src == HI).mean())      # the plateau is a plateau, not a spike

    def test_a_disc_stays_round(self):
        np = self.np
        from scipy import ndimage
        f = self.fits['disc']; F = f['F']; img = f['img'][..., 0].astype(float)
        mid = (LO + HI)/2
        radii = []
        for angle in np.linspace(0, 2*np.pi, 72, endpoint=False):
            r = np.linspace(8, 14, 601)      # source px along the ray from the centre (16, 16); texel index = x*F - 0.5
            line = ndimage.map_coordinates(img, [(16 + r*np.sin(angle))*F - 0.5, (16 + r*np.cos(angle))*F - 0.5], order=1)
            radii.append(8 + 6*self.crossing(-line, -mid)/600)      # the falling edge (the line is bright inside)
        radii = np.array(radii)
        self.assertLess(abs(radii.mean() - 11), 0.1)      # the radius of the drawn disc
        self.assertLess(radii.max() - radii.min(), 0.1)      # a straight-edged blob or a wobble would spread this

    def test_a_straight_edge_stays_straight(self):
        np = self.np
        f = self.fits['line']; F = f['F']; img = f['img'][..., 0].astype(float)
        mid = (LO + HI)/2
        xs, top, bottom = [], [], []
        for j in range(int(8*F), int(24*F), 3):      # the middle of the line, away from its ends
            col = img[:, j]
            top.append(self.crossing(col, mid)); bottom.append(self.crossing(col[::-1], mid))
            xs.append((j + 0.5)/F)
        xs, top = np.array(xs), (np.array(top) + 0.5)/F
        bottom = (len(img) - (np.array(bottom) + 0.5))/F
        for edge, offset in ((top, -1.5), (bottom, 1.5)):
            slope, intercept = np.polyfit(xs, edge, 1)
            self.assertLess(np.abs(edge - (slope*xs + intercept)).max(), 0.05)      # straight to a twentieth of a source px
            self.assertAlmostEqual(slope, 0.1, delta=0.01)      # and at the drawn angle
            self.assertAlmostEqual(slope*16 + intercept, 16.3 + offset, delta=0.1)      # and where it was drawn

    def test_prefiltered_pack_beats_an_unfiltered_one(self):
        np = self.np
        for name in ('disc', 'rrect', 'N', 'line'):
            with self.subTest(name):
                f = self.fits[name]; src, res, F = f['src'], f['res'], f['F']
                unfiltered = np.clip(np.round(self.recon.render(res, src, F, 1.0)), 0, 255)      # box width 1 texel: no pre-filter for the screen
                ch = self.pack.used_channels(src)
                filtered_view = self.gate.gv_rmse(f['img'].astype(float), F, res, src, self.SD, ch)
                self.assertLess(filtered_view, self.gate.gv_rmse(unfiltered, F, res, src, self.SD, ch) - 0.5)
                self.assertLess(filtered_view, self.gate.gv_rmse(src, 1, res, src, self.SD, ch))      # and beats the original texture
                ideal = self.gate.ideal(res, src, self.SD, (0.37, 0.61))      # the same through the game view of the two textures
                a = self.gameview.view(f['img'].astype(float), F, self.SD, (0.37, 0.61)) - ideal
                b = self.gameview.view(unfiltered, F, self.SD, (0.37, 0.61)) - ideal
                self.assertLess(np.sqrt((a**2).mean()), np.sqrt((b**2).mean()))

    def test_the_gate_fails_a_broken_shape_and_an_ink_drift(self):
        np = self.np
        from scipy import ndimage
        f = self.fits['disc']; src, res, F, dn = f['src'], f['res'], f['F'], f['dn']
        self.assertEqual(self.gate.verdict(f['m']), {})
        moved = np.roll(f['img'], 2*F, axis=1)      # the disc drawn 2 source px too far right
        bad = self.gate.verdict(self.gate.measure(src, res, moved, dn, F, self.SD))
        self.assertIn('gv', bad)
        self.assertNotIn('levels', bad)      # same plateaus: only the shape is wrong
        fat = ndimage.grey_dilation(f['img'], size=(3, 3, 1))      # every edge a texel out: more ink, plateaus unchanged
        bad = self.gate.verdict(self.gate.measure(src, res, fat, dn, F, self.SD))
        self.assertIn('ink', bad)
        self.assertNotIn('levels', bad)
        dim = np.round(LO + (f['img'].astype(float) - LO)*0.8).astype(np.uint8)      # the bright plateau too low
        bad = self.gate.verdict(self.gate.measure(src, res, dim, dn, F, self.SD))
        self.assertIn('levels', bad)
        value, text = bad['levels']
        self.assertGreater(value, 2)
        self.assertIn('plateau ends', text)

    def test_the_gate_fails_a_glow_with_a_hard_core(self):
        bad = self.gate.verdict(self.fits['glowcore']['m'])
        self.assertTrue({'ink', 'levels', 'rt'} & set(bad), bad)      # a hard core on a soft glow is not a shape: the game keeps its own texture

    def test_a_pixel_crisp_block_stays_a_block(self):
        f = self.fits['block']; F = f['F']; img = f['img'][..., 0]
        mask = f['src'][..., 0] > (LO + HI)/2
        self.assertTrue((f['dn'][..., 0].round() == f['src'][..., 0]).all())      # exact texel geometry: the traced block redraws the source
        from scipy import ndimage
        inner = img.reshape(32, F, 32, F)[:, F//2, :, F//2]      # the middle of each texel's block of the pack
        self.assertTrue((inner[ndimage.binary_erosion(mask, iterations=2)] == HI).all())      # plateau texels deep inside and outside
        self.assertTrue((inner[~ndimage.binary_dilation(mask, iterations=2)] == LO).all())
        row = img[13*F + F//2].astype(float)      # the block's middle row: the edges stay where the texel edges are (x = 9 and 27)
        self.assertAlmostEqual((self.crossing(row, (LO + HI)/2) + 0.5)/F, 9, delta=0.05)
        self.assertAlmostEqual((self.crossing(-row, -(LO + HI)/2) + 0.5)/F, 27, delta=0.05)


# ---------------------------------------------------------------------------------------------------------------------------------

A, B, C = 'aaaaaaaaaaaaaaaa-24x24-49', 'bbbbbbbbbbbbbbbb-24x24-49', 'cccccccccccccccc-24x24-49'
D, E, G = 'dddddddddddddddd-24x24-49', 'eeeeeeeeeeeeeeee-24x24-49', 'ffffffffffffffff-24x24-49'


class Folders(unittest.TestCase):
    """A harvest folder, a key list and a pack folder in a temporary directory."""
    N = 24

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.harvest, self.out = Path(self.tmp.name, 'harvest'), Path(self.tmp.name, 'pack')
        self.harvest.mkdir()
        self.rows = {}
        self.log = []

    def add(self, key, array, cls='mask', mode='L', picture=True):
        if picture:
            save_png(self.harvest/f'{key}.png', array, mode)
        self.rows[key] = f'{key}\t{self.N}\t{self.N}\t0x49\t10\t{cls}\t0\t87d90016\t{self.N*self.N}\t1\n'
        (self.harvest/'harvest.tsv').write_text(HARVEST_HEADER + ''.join(self.rows.values()))

    def keys(self, *entries, name='keys.tsv'):
        """entries: key or (key, scale, options)."""
        return write_keys(Path(self.tmp.name, name), [(e, None, {}) if isinstance(e, str) else e for e in entries])

    def build(self, *entries, **kw):
        from tools.ui_pack import build
        kw.setdefault('jobs', 1)
        return build.build(self.harvest, self.out, keylist.read_keylist(self.keys(*entries)), kw.pop('screen_height', 1080), log=self.log.append, **kw)

    def manifest(self):
        return manifest.read(self.out)

    def snapshot(self):
        return {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in sorted(self.out.iterdir())} if self.out.is_dir() else None

    def logged(self, *words):
        return [line for line in self.log if all(w in line for w in words)]


@unittest.skipUnless(HAVE_METHOD, NEED_METHOD)
class BuildOutcomeTest(Folders):
    def test_writes_skips_and_fails_as_specified(self):
        self.add(A, shape('disc', 24))
        self.add(B, shape('glowcore', 24))      # the gate fails it
        self.add(C, load_rgba(self.N), 'colour', 'RGBA')
        self.add(D, flat(self.N))
        self.add(E, shape('glow', 24))      # a pure gradient
        self.add(G, shape('ring', 24))      # listed with a skip option
        self.add('1111111111111111-24x24-49', shape('disc', 24), 'colour', 'L')      # harvested, not in the list
        failed = self.build(A, B, C, D, E, (G, None, {'skip': 'title art'}), '2222222222222222-24x24-49')
        self.assertEqual(failed, 1)
        self.assertEqual(sorted(p.name for p in self.out.glob('*.png')), [f'{A}.png'])      # only the disc is written
        picture = load_png(self.out/f'{A}.png')
        self.assertEqual(picture.shape, (24*8, 24*8))      # 8x up to 128 texels
        source = load_png(self.harvest/f'{A}.png')
        self.assertEqual((picture.min(), picture.max()), (source.min(), source.max()))
        rows = self.manifest()
        self.assertEqual({k: r.status for k, r in rows.items()}, {A: 'written', B: 'failed', C: 'skipped', D: 'skipped', E: 'skipped', G: 'skipped'})
        self.assertEqual((rows[A].F, rows[A].screen_height, rows[A].s_d), ('8', '1080', '0.8'))
        self.assertIn('rt=', rows[A].gate)
        self.assertTrue(rows[B].reason.startswith('gate: '), rows[B].reason)
        self.assertIn('colour art', rows[C].reason)
        self.assertIn('flat', rows[D].reason)
        self.assertIn('pure gradient', rows[E].reason)
        self.assertEqual(rows[G].reason, 'title art')
        self.assertTrue(self.logged(B, 'failed'))      # the summary names the failed key
        self.assertTrue(self.logged('failed 1'))
        self.assertTrue(self.logged('not built (--all builds them'))      # the unlisted harvested texture is named, not built
        self.assertTrue(self.logged('2222222222222222-24x24-49'))      # the listed texture the harvest lacks is named

    def test_all_builds_the_unlisted_keys_with_the_default_options(self):
        self.add(A, shape('disc', 24))
        self.add(B, load_rgba(self.N), 'colour', 'RGBA')
        self.build(A, all_keys=True)
        self.assertEqual(sorted(self.manifest()), [A, B])
        self.assertEqual(self.manifest()[B].status, 'skipped')

    def test_accepted_checks_let_a_key_through(self):
        self.add(A, shape('glowcore', 24))
        self.assertEqual(self.build(A), 1)
        self.assertEqual(self.manifest()[A].status, 'failed')
        failing = set(re.findall(r'(?:^gate: |; )(ink|levels|rt|gv):', self.manifest()[A].reason))
        self.assertTrue(failing)
        self.assertEqual(self.build((A, None, {'accept': {c: f'{c} is a glow' for c in failing}})), 0)
        row = self.manifest()[A]
        self.assertEqual(row.status, 'written')
        self.assertIn('accepted', row.reason)
        self.assertTrue((self.out/f'{A}.png').is_file())
        if len(failing) > 1:
            self.build((A, None, {'accept': {c: 'x' for c in list(failing)[:1]}}))
            self.assertEqual(self.manifest()[A].status, 'failed')      # one accepted check is not all of them

    def test_a_missing_picture_is_reported_failed(self):
        self.add(A, shape('disc', 24), picture=False)
        self.add(B, flat(self.N))
        self.assertEqual(self.build(A, B), 1)
        self.assertTrue(self.logged(A, 'FAILED', 'not found'))
        self.assertEqual(sorted(self.manifest()), [B])

    def test_a_pool_of_workers_builds_the_same(self):
        self.add(A, shape('disc', 24)); self.add(B, shape('line', 24))
        self.assertEqual(self.build(A, B, jobs=2), 0)
        self.assertEqual({k: r.status for k, r in self.manifest().items()}, {A: 'written', B: 'written'})
        self.assertEqual(sorted(p.name for p in self.out.glob('*.png')), [f'{A}.png', f'{B}.png'])
        pooled = {p.name: p.read_bytes() for p in self.out.iterdir()}
        self.assertEqual(self.build(A, B, jobs=1, force=True), 0)
        self.assertEqual({p.name: p.read_bytes() for p in self.out.iterdir()}, pooled)      # pictures and manifest, byte for byte

    def test_no_stale_picture_of_a_key_that_turns_skipped_or_failed(self):
        self.add(A, shape('disc', 24))
        self.build(A)
        self.assertTrue((self.out/f'{A}.png').is_file())
        self.build((A, None, {'skip': 'now a title'}))      # skipped: the old picture goes
        self.assertFalse((self.out/f'{A}.png').exists())
        self.assertEqual((self.manifest()[A].status, self.manifest()[A].reason), ('skipped', 'now a title'))
        self.build(A)
        self.assertTrue((self.out/f'{A}.png').is_file())
        self.add(A, shape('glowcore', 24))      # the texture changes into something the gate fails
        self.assertEqual(self.build(A), 1)
        self.assertFalse((self.out/f'{A}.png').exists())
        self.assertEqual(self.manifest()[A].status, 'failed')
        self.assertFalse(list(self.out.glob('*.tmp')))


@unittest.skipUnless(HAVE_METHOD, NEED_METHOD)
class CliTest(Folders):
    def main(self, *args):
        from tools.ui_pack import cli
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(['build', '--harvest', str(self.harvest), '--out', str(self.out), '--jobs', '1', *args])
        return code, out.getvalue(), err.getvalue()

    def test_exits_0_with_a_failing_key_and_leaves_it_out(self):
        self.add(A, shape('glowcore', 24))
        code, out, err = self.main('--keys', str(self.keys(A)))
        self.assertEqual((code, err), (0, ''))
        self.assertIn('failed 1', out)
        self.assertEqual(self.manifest()[A].status, 'failed')
        self.assertFalse((self.out/f'{A}.png').exists())

    def test_exits_2_for_a_bad_list_harvest_or_argument(self):
        self.add(A, flat(self.N))
        keys = self.keys(A)
        self.assertEqual(self.main('--keys', str(keys), '--screen-height', '0')[0], 2)
        self.assertEqual(self.main('--keys', str(keys), '--jobs', '0')[0], 2)
        self.assertEqual(self.main('--keys', str(keys), '--only', 'nope')[0], 2)
        self.assertEqual(self.main('--keys', str(self.tmp.name + '/absent.tsv'))[0], 2)
        Path(self.tmp.name, 'bad.tsv').write_text('# reference-height: 1080\n')
        code, _, err = self.main('--keys', str(Path(self.tmp.name, 'bad.tsv')))
        self.assertEqual(code, 2)
        self.assertIn('ui_pack: ', err)
        (self.harvest/'harvest.tsv').write_text('key\twidth\n')
        self.assertEqual(self.main('--keys', str(keys))[0], 2)
        (self.harvest/'harvest.tsv').unlink()
        self.assertEqual(self.main('--keys', str(keys))[0], 2)      # no harvest.tsv
        self.assertFalse(self.out.exists())      # nothing written on a refused run

    def test_game_key_lists_are_found(self):
        from tools.ui_pack import cli
        self.assertEqual(cli.GAMES, games())
        for path in cli.GAMES.values():
            self.assertTrue((cli.REPO_ROOT/path).is_file())
        self.add(A, flat(self.N))
        code, out, _ = self.main('--game', 'botw')      # a harvest that holds none of the 47 listed textures
        self.assertEqual(code, 0)
        self.assertIn('note: 47 of the 47 listed textures are not in the harvest', out)


def flat(n):
    import numpy as np
    return np.full((n, n), 20, np.uint8)


def load_rgba(n):
    import numpy as np
    y, x = np.mgrid[:n, :n]
    return np.stack([x*10 % 256, y*10 % 256, (x + y)*5 % 256, np.full((n, n), 255)], axis=-1).astype(np.uint8)


@unittest.skipUnless(HAVE_METHOD, NEED_METHOD)
class IncrementalBuildTest(Folders):
    """Two built keys (A a disc, B a line) are the starting point; every test works on its own copy of them."""

    @classmethod
    def setUpClass(cls):
        cls.template = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.template.cleanup)
        cls.template_harvest, cls.template_out = Path(cls.template.name, 'harvest'), Path(cls.template.name, 'pack')
        cls.template_harvest.mkdir()
        save_png(cls.template_harvest/f'{A}.png', shape('disc', 24)); save_png(cls.template_harvest/f'{B}.png', shape('line', 24))
        (cls.template_harvest/'harvest.tsv').write_text(HARVEST_HEADER + ''.join(f'{k}\t24\t24\t0x49\t10\tmask\t0\t87d90016\t576\t1\n' for k in (A, B)))
        from tools.ui_pack import build
        keys = write_keys(Path(cls.template.name, 'keys.tsv'), [(A, None, {}), (B, None, {})])
        cls.failed = build.build(cls.template_harvest, cls.template_out, keylist.read_keylist(keys), 1080, jobs=1, log=lambda line: None)

    def setUp(self):
        super().setUp()
        shutil.rmtree(self.harvest); shutil.copytree(self.template_harvest, self.harvest); shutil.copytree(self.template_out, self.out)
        self.rows = {k: f'{k}\t24\t24\t0x49\t10\tmask\t0\t87d90016\t576\t1\n' for k in (A, B)}

    def rebuilt(self, key):
        return bool(self.logged(f'{key}: written'))

    def test_the_starting_point(self):
        self.assertEqual(self.failed, 0)
        self.assertEqual({k: r.status for k, r in self.manifest().items()}, {A: 'written', B: 'written'})

    def test_a_current_entry_is_reused(self):
        before = self.snapshot()
        self.assertEqual(self.build(A, B), 0)
        for name in (f'{A}.png', f'{B}.png'):
            self.assertEqual(self.snapshot()[name], before[name])      # the same bytes at the same mtime
        self.assertTrue(self.logged(A, 'unchanged') and self.logged(B, 'unchanged'))
        self.assertTrue(self.logged('written 2 (0 built, 2 unchanged)'))

    def test_a_changed_screen_height_rebuilds(self):
        before = self.snapshot()
        self.build(A, B, screen_height=1440)
        self.assertTrue(self.rebuilt(A) and self.rebuilt(B))
        after = self.snapshot()
        self.assertNotEqual(after[f'{A}.png'][0], before[f'{A}.png'][0])      # another pre-filter
        row = self.manifest()[A]
        self.assertEqual((row.screen_height, row.s_d), ('1440', f'{0.8*1440/1080:.4g}'))

    def test_a_changed_option_or_scale_rebuilds_only_that_key(self):
        self.build((A, None, {'F': 4}), B)
        self.assertTrue(self.rebuilt(A))
        self.assertFalse(self.rebuilt(B))
        self.assertEqual(load_png(self.out/f'{A}.png').shape, (96, 96))      # F 4 instead of 8
        self.assertEqual(self.manifest()[A].F, '4')
        self.log.clear()
        self.build((A, None, {'F': 4}), (B, 0.9, {}))
        self.assertTrue(self.rebuilt(B))
        self.assertFalse(self.rebuilt(A))
        self.assertEqual(self.manifest()[B].s_d, '0.9')

    def test_a_changed_source_rebuilds_only_that_key(self):
        before = self.snapshot()
        save_png(self.harvest/f'{B}.png', shape('rrect', 24))
        self.build(A, B)
        self.assertTrue(self.rebuilt(B))
        self.assertFalse(self.rebuilt(A))
        after = self.snapshot()
        self.assertEqual(after[f'{A}.png'], before[f'{A}.png'])
        self.assertNotEqual(after[f'{B}.png'][0], before[f'{B}.png'][0])

    def test_force_rebuilds_a_current_entry(self):
        self.build(A, B, force=True)
        self.assertTrue(self.rebuilt(A) and self.rebuilt(B))
        self.assertTrue(self.logged('written 2 (2 built, 0 unchanged)'))

    def test_only_limits_the_build_and_refuses_an_unknown_key(self):
        save_png(self.harvest/f'{A}.png', shape('ring', 24)); save_png(self.harvest/f'{B}.png', shape('rrect', 24))      # both sources changed
        self.build(A, only=[A])
        self.assertTrue(self.rebuilt(A))
        self.assertFalse(self.logged(B))      # not looked at
        before = self.snapshot()
        with self.assertRaises(UiPackError) as caught:
            self.build(A, B, only=[B, 'nope'])
        self.assertIn('nope', str(caught.exception))
        self.assertEqual(self.snapshot(), before)      # nothing written by the refused run
        with self.assertRaises(UiPackError):
            self.build(A, only=[B])      # harvested, but not in the key list
        self.build(A, B, only=[B])
        self.assertTrue(self.rebuilt(B))
        self.build(A, only=[A])      # B left the list, but a run with --only deletes nothing
        self.assertTrue((self.out/f'{B}.png').is_file())

    def test_dry_run_writes_nothing(self):
        save_png(self.harvest/f'{A}.png', shape('ring', 24))
        self.add(C, shape('disc', 24))
        before = self.snapshot()
        self.assertEqual(self.build(A, C, dry_run=True), 0)      # B left the list
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(sorted(self.manifest()), [A, B])
        self.assertTrue(self.logged(A, 'would build') and self.logged(C, 'would build'))
        self.assertTrue(self.logged(B, 'would delete'))
        self.assertTrue(self.logged('dry run: 2 to build'))
        self.out = Path(self.tmp.name, 'fresh')
        self.build(A, C, dry_run=True)
        self.assertFalse(self.out.exists())      # not even the folder

    def test_a_picture_the_manifest_does_not_own_is_never_touched(self):
        other = Path(self.tmp.name, 'other'); other.mkdir()
        self.out = other
        (other/f'{A}.png').write_bytes(b'a vector tool made this')
        for kw in ({}, {'force': True}):
            self.log.clear()
            self.build(A, B, **kw)
            self.assertEqual((other/f'{A}.png').read_bytes(), b'a vector tool made this')
            self.assertTrue(self.logged(A, 'left alone'))
            self.assertNotIn(A, self.manifest())
            self.assertTrue((other/f'{B}.png').is_file())      # the other key is built
        self.build((A, None, {'skip': 'x'}), B)      # not even when the key turns into a skipped one
        self.assertEqual((other/f'{A}.png').read_bytes(), b'a vector tool made this')
        self.build(B)      # nor when the key leaves the list
        self.assertEqual((other/f'{A}.png').read_bytes(), b'a vector tool made this')

    def test_an_old_manifest_is_migrated(self):
        (self.out/manifest.MANIFEST).unlink()      # the pack of the earlier builder: pictures and ai-manifest.tsv only
        (self.out/f'{A}.png').write_bytes(b'upscaled once'); (self.out/f'{C}.png').write_bytes(b'upscaled once')
        (self.out/f'{B}.png').write_bytes(b'upscaled once')
        (self.out/manifest.OLD_MANIFEST).write_text('key\tmodel\tfactor\tmtime\trevision\n'
                                                    f'{A}\trealesrgan-x4plus-anime\t4\t1\tr1\n'
                                                    f'{B}\trealesrgan-x4plus-anime\t4\t1\n'
                                                    f'{C}\ta\tb\tc\td\te\tf\n')
        self.assertEqual(self.build(A, B), 0)
        self.assertFalse((self.out/manifest.OLD_MANIFEST).exists())      # taken over and removed
        self.assertEqual({k: r.status for k, r in self.manifest().items()}, {A: 'written', B: 'written'})
        self.assertTrue(self.rebuilt(A) and self.rebuilt(B))      # the pictures are made again by this method
        self.assertEqual(load_png(self.out/f'{A}.png').shape, (192, 192))
        self.assertFalse((self.out/f'{C}.png').exists())      # not a target any more: removed
        self.assertTrue(self.logged(C, 'deleted'))

    def test_a_key_that_leaves_the_list_has_its_picture_deleted(self):
        self.build(A)
        self.assertFalse((self.out/f'{B}.png').exists())
        self.assertEqual(sorted(self.manifest()), [A])
        self.assertTrue(self.logged(B, 'deleted'))
        self.assertTrue((self.out/f'{A}.png').is_file())
        self.assertTrue(self.logged('1 stale pictures deleted'))

    def test_a_listed_key_the_harvest_lacks_keeps_its_picture(self):
        self.rows.pop(B)
        (self.harvest/'harvest.tsv').write_text(HARVEST_HEADER + ''.join(self.rows.values()))
        self.build(A, B)
        self.assertTrue((self.out/f'{B}.png').is_file())      # a smaller harvest does not shrink the pack
        self.assertIn(B, self.manifest())
        self.assertTrue(self.logged('not in the harvest', B))


if __name__ == '__main__':
    unittest.main()
