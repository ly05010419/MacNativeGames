"""Stripped Unity 2021.2 Shader resources, without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import unityfs


def string(value):
    raw = value.encode()
    return struct.pack('<i', len(raw)) + raw + bytes(-len(raw) % 4)


class Unity2021Tests(unittest.TestCase):
    def test_stripped_shader_reads_keyword_tables_and_segments(self):
        # Unity 2021.2's parsed form adds the keyword names and flags. Empty
        # subshaders and properties (so no pass is read: the passes' keyword state
        # masks are checked against the release tree, not here), then the parsed
        # shader's strings, arrays and aligned bool, one platform with one segment.
        parsed = (struct.pack('<i', 0) + struct.pack('<i', 0)
                  + struct.pack('<i', 0) + struct.pack('<i', 0)
                  + string('Legacy/Blit') + string('') + string('')
                  + struct.pack('<i', 0) + struct.pack('<i', 0) + b'\1\0\0\0')
        arrays = (struct.pack('<iI', 1, 38)
                  + struct.pack('<iiI', 1, 1, 0) * 3
                  + struct.pack('<i', 0) + struct.pack('<i', 0) + struct.pack('<i', 0))
        payload = string('Legacy') + parsed + arrays + b'\1\0\0\0'
        type_hash = bytes.fromhex('d8236c9bd87d85edb203acf352348124')
        metadata = (b'2021.2.13f1\0' + struct.pack('<i?iiBh', 38, False, 1, 48, 0, -1)
                    + type_hash + struct.pack('<i', 1))
        metadata += bytes(-(20 + len(metadata)) % 4)
        metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
        data = (struct.pack('>IIII', len(metadata), 0x100 + len(payload), 17, 0x100)
                + bytes(4) + metadata).ljust(0x100, b'\0') + payload
        tree = unityfs.known_tree(48, type_hash)
        objects = list(unityfs.serialized_objects(data, {}, {48}, {(48, type_hash): tree}))
        shader = objects[0][2]
        self.assertEqual(shader['m_ParsedForm']['m_Name'], 'Legacy/Blit')
        self.assertEqual(shader['m_ParsedForm']['m_KeywordNames'], [])
        self.assertEqual(shader['m_ParsedForm']['m_KeywordFlags'], b'')
        self.assertEqual(shader['platforms'], [38])
        self.assertEqual(shader['offsets'], [[0]])
        self.assertTrue(shader['m_ShaderIsBaked'])
        reader = unityfs.Reader(payload, True)
        unityfs._read(reader, tree)
        self.assertEqual(reader.p, len(payload))


if __name__ == '__main__':
    unittest.main()
