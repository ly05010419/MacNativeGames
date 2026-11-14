"""shader_split: Gust's chunked zlib .gz files (Atelier Ryza DX's PostEffect/pe_pack.elixir.gz: chunks of a u32 stored length and one zlib stream, ended by a zero length)
are inflated before the scan (gust_gz), and other forms are left alone. No game files needed."""
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100199023B2E000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had


def chunked(data, size=0x4000):
    out = b''
    for o in range(0, len(data), size):
        z = zlib.compress(data[o:o + size]); out += struct.pack('<I', len(z)) + z
    return out + bytes(4)


class GustGz(unittest.TestCase):
    def test_chunks_are_joined(self):
        data = bytes(range(256)) * 200   # 51,200 bytes: three full chunks and a short one
        self.assertEqual(ss.gust_gz(chunked(data)), data)

    def test_other_forms_are_left_alone(self):
        data = b'CRAE' + bytes(60)
        self.assertIsNone(ss.gust_gz(data))                              # a stored CRAE archive
        self.assertIsNone(ss.gust_gz(b'\x1f\x8b\x08\x00' + bytes(60)))   # a gzip file
        self.assertIsNone(ss.gust_gz(chunked(data)[:-4]))                # no zero length at the end
        self.assertIsNone(ss.gust_gz(chunked(data) + b'more'))           # bytes after the zero length
        z = zlib.compress(data); self.assertIsNone(ss.gust_gz(struct.pack('<I', len(z) + 4) + z + b'junk' + bytes(4)))   # a chunk that is not one whole zlib stream

    def test_scan_reads_the_inflated_file(self):
        inner = b'archive ' + b'\x91\x68\x86\x19' + bytes(64)
        seen = []
        real = ss.glslc_shaders
        ss.glslc_shaders = lambda d, label, need=True, skip=(): seen.append(bytes(d)) or []
        try:
            with tempfile.TemporaryDirectory() as t:
                p = os.path.join(t, 'pe_pack.elixir.gz')
                with open(p, 'wb') as f: f.write(chunked(inner))
                ss.scan_file_in((p, 'Data/NX/PostEffect/pe_pack.elixir.gz'))
        finally: ss.glslc_shaders = real
        self.assertEqual(seen, [inner])


if __name__ == '__main__':
    unittest.main()
