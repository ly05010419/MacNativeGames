"""A recorded nxnative pipeline row must become exactly the key this runtime would make, or be left out with a reason."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import import_nxnative_manifest as imp  # noqa: E402
from shader_list import DEFAULT_SUFFIX  # noqa: E402

VS, FS = 'a5b1bd74e47458d7', '29e3c9dd92b8a571'
VS_FULL, FS_FULL = VS + '0' * 48, FS + '1' * 48


def nx_key(vs=VS + '_b', fs=FS + '_b', a2c=0, atest=0, logic=0, attr_hi=0, adv=0):
    """A 504-byte nxnative pso_skey: names (24 bytes each), 4 header bytes, the 34-byte fz descriptor, 2 padding, pso_key."""
    head = vs.encode().ljust(24, b'\0') + fs.encode().ljust(24, b'\0') + bytes([0, 1, 0, 0]) + bytes(range(34)) + b'\0\0'
    state = bytearray(416)
    state[16:20] = struct.pack('<I', 80)         # cfmt[0]
    state[64:68] = struct.pack('<I', 0xf)        # chmask
    state[68:72] = struct.pack('<I', a2c)
    state[72:80] = struct.pack('<Q', adv << 37)  # blend[0]: nxnative's advanced-blend fields sit above our 37 bits
    state[344:352] = struct.pack('<II', atest, logic)
    state[352:356] = struct.pack('<I', attr_hi)
    return (head + bytes(state)).hex()


class ConvertTests(unittest.TestCase):
    names = {VS: VS_FULL, FS: FS_FULL}

    def test_layout_of_a_converted_key(self):
        key = bytes.fromhex(imp.convert_key(nx_key(), self.names))
        self.assertEqual(len(key), 544)
        self.assertEqual(key[:80].rstrip(b'\0').decode(), f'{VS_FULL}-{DEFAULT_SUFFIX}_b')
        self.assertEqual(key[80:160].rstrip(b'\0').decode(), f'{FS_FULL}-{DEFAULT_SUFFIX}_b')
        self.assertEqual(key[160:198], bytes([0, 1, 0, 0]) + bytes(range(34)))
        self.assertEqual(key[198:200], b'\0\0')
        self.assertEqual(struct.unpack_from('<I', key, 200 + 16)[0], 80)
        self.assertEqual(struct.unpack_from('<I', key, 200 + 64)[0], 0xf)

    def test_no_fragment_shader_stays_empty(self):
        key = bytes.fromhex(imp.convert_key(nx_key(fs=''), self.names))
        self.assertEqual(key[80:160], bytes(80))

    def test_state_this_runtime_cannot_represent_is_left_out(self):
        for field, reason in (({'atest': 1}, 'alpha test or logic operation'), ({'logic': 1}, 'alpha test or logic operation'),
                              ({'attr_hi': 1}, 'vertex attributes above 15'), ({'adv': 1}, 'advanced blending'),
                              ({'a2c': 2}, 'a2c bits')):
            with self.assertRaises(imp.Skip) as caught:
                imp.convert_key(nx_key(**field), self.names)
            self.assertEqual(str(caught.exception), reason)

    def test_unknown_shader_and_wrong_length(self):
        with self.assertRaises(imp.Skip) as caught:
            imp.convert_key(nx_key(vs='ffffffffffffffff_b'), self.names)
        self.assertEqual(str(caught.exception), 'shader not in the seed')
        with self.assertRaises(imp.Skip) as caught:
            imp.convert_key('00' * 500, self.names)
        self.assertEqual(str(caught.exception), 'key length')


class MeshRowTests(unittest.TestCase):
    names = {VS: VS_FULL, FS: FS_FULL}

    def test_task_shader_and_no_task_shader(self):
        self.assertEqual(imp.task_name('-', self.names), '-')
        self.assertEqual(imp.task_name(FS + '_b', self.names), f'{FS_FULL}-{DEFAULT_SUFFIX}_b')

    def test_unknown_or_malformed_task_shader(self):
        for bad in ('ffffffffffffffff_b', 'nonsense'):
            with self.assertRaises(imp.Skip):
                imp.task_name(bad, self.names)

    def test_a_manifest_gives_render_and_mesh_rows(self):
        with tempfile.TemporaryDirectory() as d:
            manifest = Path(d) / 'pso_manifest.tsv'
            manifest.write_text(f'# comment\nS\tx_b\t1\nR\t{nx_key()}\nM\t-\t{nx_key()}\nM\t{FS}_b\t{nx_key()}\nM\t{"f" * 16}_b\t{nx_key()}\n')
            rows, skipped = imp.convert(manifest, self.names)
        key = imp.convert_key(nx_key(), self.names)
        self.assertEqual(rows, [f'R\t{key}', f'M\t-\t{key}', f'M\t{FS_FULL}-{DEFAULT_SUFFIX}_b\t{key}'])
        self.assertEqual(skipped, {'shader not in the seed': 1})


class SeedTests(unittest.TestCase):
    def test_seed_names_need_a_default_variant(self):
        with tempfile.TemporaryDirectory() as d:
            seed = Path(d)
            (seed / f'{VS_FULL}.variants').write_text('[]')
            (seed / f'{VS_FULL}-{DEFAULT_SUFFIX}_b.metal').write_text('')
            (seed / f'{FS_FULL}.variants').write_text('[]')   # no default variant: not usable
            self.assertEqual(imp.seed_names(seed), {VS: VS_FULL})


if __name__ == '__main__':
    unittest.main()
