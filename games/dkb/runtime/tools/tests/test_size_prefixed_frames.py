"""shader_split.unsized: HAL's .cmp containers (Kirby and the Forgotten Land), a 4-byte little-endian uncompressed size, then a zstd or an LZ4 frame. The frame
is opened only when it decompresses to exactly the size the prefix gives; anything else is returned as it is, so a file that is not such a container is
scanned as before. No game files needed."""
import contextlib
import io
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

BODY = b'BNSH\0\0\0\0' + bytes(range(256)) * 64


def unsized(d):
    out = io.StringIO()
    with contextlib.redirect_stdout(out): r = ss.unsized(d, '/test.cmp')
    return r, out.getvalue()


class Unsized(unittest.TestCase):
    def test_zstd_frame(self):
        d = struct.pack('<I', len(BODY)) + ss.zstd_mod().compress(BODY)
        self.assertEqual(d[4:8], ss.ZSTD_MAGIC)
        self.assertEqual(unsized(d), (BODY, ''))

    def test_lz4_frame(self):
        import lz4.frame
        d = struct.pack('<I', len(BODY)) + lz4.frame.compress(BODY)
        self.assertEqual(d[4:8], ss.LZ4_FRAME_MAGIC)
        self.assertEqual(unsized(d), (BODY, ''))

    def test_frame_size(self):
        import lz4.frame
        zs = ss.zstd_mod()
        self.assertEqual(ss.frame_size(zs.compress(BODY)), len(BODY))
        self.assertEqual(ss.frame_size(zs.compress(b'x' * 300)), 300)   # the 2-byte field, which counts from 256
        self.assertEqual(ss.frame_size(lz4.frame.compress(BODY, store_size=True)), len(BODY))
        self.assertIsNone(ss.frame_size(lz4.frame.compress(BODY, store_size=False)))

    def test_word_before_a_frame_that_records_another_size(self):
        # Tears of the Kingdom's Cave/…/C.crbin.<hash>/*.quad: the hash, then a zstd frame of its own size; not this container, and nothing to report
        d = bytes.fromhex('eb157a51') + ss.zstd_mod().compress(BODY)
        self.assertEqual(unsized(d), (d, ''))

    def test_frame_longer_than_its_prefix(self):
        # the prefix is the whole length, not a limit: a frame that records no size and holds more is not this container
        import lz4.frame
        d = struct.pack('<I', len(BODY) - 1) + lz4.frame.compress(BODY, store_size=False)
        r, log = unsized(d)
        self.assertIs(r, d)
        self.assertIn('[RED] /test.cmp', log)

    def test_bytes_that_do_not_decompress(self):
        d = struct.pack('<I', 64) + ss.ZSTD_MAGIC + b'\x00' * 60
        r, log = unsized(d)
        self.assertIs(r, d)
        self.assertIn('[RED] /test.cmp', log)


if __name__ == '__main__':
    unittest.main()
