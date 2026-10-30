"""Stripped Unity 2017 Shader resources, without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import unityfs


def string(value):
    raw = value.encode()
    return struct.pack('<i', len(raw)) + raw + bytes(-len(raw) % 4)


class Unity2017Tests(unittest.TestCase):
    def test_stripped_shader_has_no_texture_map(self):
        # Empty properties and subshaders, then the parsed shader's strings,
        # dependencies and aligned bool. There is no texture map before baked.
        parsed = (struct.pack('<ii', 0, 0) + string('Legacy/Blit') + string('')
                  + string('') + struct.pack('<i', 0) + b'\1\0\0\0')
        arrays = b''.join(struct.pack('<iI', 1, value) for value in (38, 0, 0, 0))
        payload = (string('Legacy') + parsed + arrays + struct.pack('<i', 0)
                   + struct.pack('<iiq', 1, 0, 42) + b'\1\0\0\0')
        type_hash = bytes.fromhex('266d53113fa30d2b858f2768f92eaa14')
        metadata = (b'2017.4.28f1\0' + struct.pack('<i?iiBh', 38, False, 1, 48, 0, -1)
                    + type_hash + struct.pack('<i', 1))
        metadata += bytes(-(20 + len(metadata)) % 4)
        metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
        data = (struct.pack('>IIII', len(metadata), 0x100 + len(payload), 17, 0x100)
                + bytes(4) + metadata).ljust(0x100, b'\0') + payload
        tree = unityfs.known_tree(48, type_hash)
        objects = list(unityfs.serialized_objects(data, {}, {48}, {(48, type_hash): tree}))
        shader = objects[0][2]
        self.assertEqual(shader['m_ParsedForm']['m_Name'], 'Legacy/Blit')
        self.assertEqual(shader['platforms'], [38])
        self.assertEqual(shader['offsets'], [0])
        self.assertEqual(shader['m_Dependencies'], [{'m_FileID': 0, 'm_PathID': 42}])
        self.assertTrue(shader['m_ShaderIsBaked'])
        reader = unityfs.Reader(payload, True)
        unityfs._read(reader, tree)
        self.assertEqual(reader.p, len(payload))


if __name__ == '__main__':
    unittest.main()
