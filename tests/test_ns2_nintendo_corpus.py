"""NVN2 programs in Nintendo resources must survive packing without changing their runtime keys."""
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / 'games/totk/scripts'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('ns2_corpus', SCRIPTS / 'ns2_shader_corpus.py')
corpus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(corpus)


def bnsh(stage=1):
    data = bytearray(0x600)
    data[:8] = b'BNSH\0\0\0\0'
    struct.pack_into('<I', data, 0x1c, len(data))
    data[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', data, 0x7c, 1, 0x100)
    struct.pack_into('<Q', data, 0x110, 0x140)
    struct.pack_into('<Q', data, 0x148, 0x180)
    struct.pack_into('<QQII', data, 0x188, 0x200, 0x400, 0x100, 0xd8)
    data[0x200:0x208] = corpus.CTL_MAGIC + b'\xc9\0\0\0'
    struct.pack_into('<I', data, 0x230, 0xd8)
    struct.pack_into('<I', data, 0x244, 0x100)
    struct.pack_into('<I', data, 0x400, (stage << 10) | 1)
    return data


class NintendoCorpus(unittest.TestCase):
    def test_embedded_bnsh_offsets_are_relative_to_the_bnsh(self):
        data = bnsh()
        found = corpus.nintendo_shaders(b'prefix' * 5 + data, 'main')
        self.assertEqual(len(found), 1)
        key, stage, ctl, code, _ = found[0]
        self.assertEqual(stage, 0)
        self.assertEqual(code, data[0x400:0x500])
        self.assertEqual(key, corpus.shader_key(ctl, code))

    def test_geometry_stage_is_preserved(self):
        self.assertEqual(corpus.nintendo_shaders(bnsh(4), 'terrain')[0][1], 2)

    def test_a_truncated_resource_is_rejected(self):
        with self.assertRaises(ValueError):
            corpus.nintendo_shaders(bnsh()[:-1], 'broken')

    def test_code_cannot_point_outside_its_bnsh_into_the_next_resource(self):
        data = bnsh()
        struct.pack_into('<Q', data, 0x190, 0x590)
        with self.assertRaises(ValueError):
            corpus.nintendo_shaders(data + bytes(0x100), 'broken')


if __name__ == '__main__':
    unittest.main()
