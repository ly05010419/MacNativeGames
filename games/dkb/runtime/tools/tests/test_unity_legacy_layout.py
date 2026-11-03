"""Unity 5.5 stripped Shader layout, without copyrighted game fixtures."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'dm/gfx/tools'))
import unityfs


class LegacyShaderLayoutTests(unittest.TestCase):
    def setUp(self):
        self.tree = unityfs.known_tree(48, bytes.fromhex('4496e93f2179252104401c8dda0a1751'))

    def test_reference_node_count(self):
        def count(n):
            return 1 + sum(count(k) for k in n.kids)
        self.assertEqual(count(self.tree), 1222)

    def test_legacy_fields(self):
        fields = {n.name: n for n in self.tree.kids}
        self.assertNotIn('m_NonModifiableTextures', fields)
        self.assertEqual(fields['platforms'].kids[0].flags, 0)
        self.assertEqual(fields['compressedBlob'].kids[0].flags, 0x4000)
        parsed = fields['m_ParsedForm']
        sub = next(n for n in parsed.kids if n.name == 'm_SubShaders').kids[0].kids[1]
        pas = next(n for n in sub.kids if n.name == 'm_Passes').kids[0].kids[1]
        self.assertNotIn('m_HasProceduralInstancingVariant', [n.name for n in pas.kids])
        prog = next(n for n in pas.kids if n.name == 'progVertex')
        sp = prog.kids[0].kids[0].kids[1]
        self.assertNotIn('m_ShaderRequirements', [n.name for n in sp.kids])
        channels = next(n for n in sp.kids if n.name == 'm_Channels').kids[0]
        self.assertEqual(channels.flags, 0x4000)
        self.assertEqual(channels.kids[0].flags, 0)
        # One 2-byte channel pair, padded by the vector parent before SourceMap.
        reader = unityfs.Reader(struct.pack('<i', 1) + bytes([2, 3, 0, 0]) + struct.pack('<I', 7), True)
        bind = next(n for n in sp.kids if n.name == 'm_Channels')
        value = unityfs._read(reader, bind)
        assert isinstance(value, dict)
        self.assertEqual(value['m_SourceMap'], 7)
        self.assertEqual(reader.p, 12)


if __name__ == '__main__':
    unittest.main()
