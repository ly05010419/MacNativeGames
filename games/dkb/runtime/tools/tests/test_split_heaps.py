"""shader_split: Heaps PAK archives are listed by their own header, and an archive of another engine with the same magic is left alone (heaps_pak_files); the GLSLC outputs of a
shaders.cache.nx are read out of the //BIN= comments (heaps_shaders). No game files needed."""
import base64
import io
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '010029B0118E8000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def name(s, flags): return bytes([len(s)]) + s.encode() + bytes([flags])
def file_entry(s, pos, size, wide=False): return name(s, 2 if wide else 0) + (struct.pack('<d', pos) if wide else struct.pack('<i', pos)) + struct.pack('<iI', size, 0)
def folder(s, children): return name(s, 1) + struct.pack('<i', len(children)) + b''.join(children)


def pak(root, data, version=1):
    """'PAK', the version, the header size (which counts everything up to the data), the data size, version 1's 64-character hash, the entries, then 'DATA'"""
    body = (b'0' * 64 if version else b'') + root + b'DATA'
    return b'PAK' + bytes([version]) + struct.pack('<ii', 12 + len(body), len(data)) + body + data


class HeapsPak(unittest.TestCase):
    def test_files_are_listed_at_their_archive_offsets(self):
        for version in 0, 1:
            data = b'firstsecond!'
            d = pak(folder('', [file_entry('a.png', 0, 5), folder('sub', [file_entry('shaders.cache.nx', 5, 7, wide=True)])]), data, version)
            got = ss.heaps_pak_files(io.BytesIO(d))
            assert got is not None, 'a Heaps archive was not read as one'
            self.assertEqual([(n, s) for n, _, s in got], [('/a.png', 5), ('/sub/shaders.cache.nx', 7)])
            self.assertEqual(d[got[1][1]:got[1][1] + got[1][2]], b'second!')

    def test_other_files_are_not_heaps_archives(self):
        for d in (b'PAK', b'PAK\x09' + bytes(40), b'PACK' + bytes(40), b'PAK\0' + struct.pack('<ii', 0x7fffffff, 0) + bytes(40)):
            self.assertIsNone(ss.heaps_pak_files(io.BytesIO(d)))
        no_data = pak(folder('', [file_entry('a', 0, 1)]), b'x').replace(b'DATA', b'NOPE')
        self.assertIsNone(ss.heaps_pak_files(io.BytesIO(no_data)))
        too_many = pak(name('', 1) + struct.pack('<i', 100) + file_entry('a', 0, 1), b'x')   # a folder counting more children than the header holds
        self.assertIsNone(ss.heaps_pak_files(io.BytesIO(too_many)))


class HeapsCache(unittest.TestCase):
    def test_each_comment_is_decoded_and_scanned(self):
        seen = []
        real, ss.glslc_shaders = ss.glslc_shaders, lambda d, label, need=True, skip=(): seen.append((d, label, need)) or [(label, 'ps', b'c', d)]
        try:
            d = b'#version 140\nvoid main() {}\n//BIN=' + base64.b64encode(b'output one') + b'#\n\n\xff\0\0\0\0#version 140\n//BIN=' + base64.b64encode(b'output two!') + b'#\n\n'
            found = ss.heaps_shaders(d, '/res.pak/shaders.cache.nx')
        finally: ss.glslc_shaders = real
        self.assertEqual([s[0] for s in seen], [b'output one', b'output two!'])
        self.assertTrue(all(not s[2] for s in seen))
        self.assertEqual(len(found), 2)

    def test_no_output_is_named(self):
        with self.assertRaises(SystemExit) as e: ss.heaps_shaders(b'#version 140\nvoid main() {}\n', '/x.pak/shaders.cache.nx')
        self.assertIn('//BIN=', str(e.exception.code))


if __name__ == '__main__':
    unittest.main()
