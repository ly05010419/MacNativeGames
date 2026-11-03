"""Stripped Unity 2019.4 Shader resources, without game data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import unityfs


def string(value):
    raw = value.encode()
    return struct.pack('<i', len(raw)) + raw + bytes(-len(raw) % 4)


class Unity2019Tests(unittest.TestCase):
    def test_stripped_shader_reads_segmented_blobs(self):
        # Unity 2019.4's parsed form has no keyword tables, and offsets /
        # compressedLengths / decompressedLengths are one list per platform
        # (2019.3+). Empty properties and subshaders (so no pass is read: the pass
        # and subprogram fields are checked against the release tree, not here),
        # then the parsed shader's strings and aligned bool, one platform with one
        # segment.
        parsed = (struct.pack('<i', 0) + struct.pack('<i', 0)
                  + string('Legacy/Blit') + string('') + string('')
                  + struct.pack('<i', 0) + b'\1\0\0\0')
        arrays = (struct.pack('<iI', 1, 38)
                  + struct.pack('<iiI', 1, 1, 0)
                  + struct.pack('<iiI', 1, 1, 4)
                  + struct.pack('<iiI', 1, 1, 4))
        payload = string('Legacy') + parsed + arrays + struct.pack('<i', 4) + b'\x01\x02\x03\x04' + struct.pack('<i', 0) + struct.pack('<i', 0) + b'\1\0\0\0'
        type_hash = bytes.fromhex('ed2a06ce7fb8203f6efb5ab5e0ce20d5')
        metadata = (b'2019.4.21f1\0' + struct.pack('<i?iiBh', 38, False, 1, 48, 0, -1)
                    + type_hash + struct.pack('<i', 1))
        metadata += bytes(-(20 + len(metadata)) % 4)
        metadata += struct.pack('<qIII', 1, 0, len(payload), 0)
        data = (struct.pack('>IIII', len(metadata), 0x100 + len(payload), 21, 0x100)
                + bytes(4) + metadata).ljust(0x100, b'\0') + payload
        tree = unityfs.known_tree(48, type_hash)
        objects = list(unityfs.serialized_objects(data, {}, {48}, {(48, type_hash): tree}))
        shader = objects[0][2]
        self.assertEqual(shader['m_ParsedForm']['m_Name'], 'Legacy/Blit')
        self.assertEqual(shader['platforms'], [38])
        self.assertEqual(shader['offsets'], [[0]])
        self.assertEqual(shader['compressedBlob'], b'\x01\x02\x03\x04')
        self.assertTrue(shader['m_ShaderIsBaked'])
        reader = unityfs.Reader(payload, True)
        unityfs._read(reader, tree)
        self.assertEqual(reader.p, len(payload))


if __name__ == '__main__':
    unittest.main()
