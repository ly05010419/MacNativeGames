"""compact_movies.py: the MIX files it writes read back (mixls.parse, the
game's plain-header layout) and the Bink header it keeps is the one
binkw32 reads (TrackIds' layout).

    python3 -I -B -m unittest discover -s windows_games/ra2/tests
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/ios'))
sys.path.insert(0, str(ROOT / 'scripts'))
import compact_movies  # noqa: E402
import mixls  # noqa: E402


def bink_header(revision, tracks):
    h = b'BIK' + revision + struct.pack('<10I', 0, 3580, 0, 0, 640, 480, 15, 1, 0, tracks)
    if revision >= b'k':
        h += b'\0' * 4
    h += b'\0' * 8 * tracks + b''.join(struct.pack('<I', 100 + i) for i in range(tracks))
    return h


class CompactMovies(unittest.TestCase):
    def test_mix_round_trip(self):
        entries = [(0x1E7D27F9, b'movie one'), (0x80000001, b''), (0x7FFFFFFF, b'x' * 1000)]
        with tempfile.TemporaryDirectory() as t:
            path = Path(t) / 'TEST.MIX'
            compact_movies.write_mix(path, entries)
            data = path.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data, 0)[0], 0)  # plain header
            parsed = mixls.parse(data)
            self.assertEqual(list(parsed), [i for i, _ in entries])  # order kept
            for id_, d in entries:
                off, size = parsed[id_]
                self.assertEqual(data[off:off + size], d)

    def test_header_length(self):
        for rev, tracks, want in ((b'i', 0, 44), (b'i', 2, 44 + 16 + 8), (b'k', 1, 44 + 8 + 4 + 4)):
            h = bink_header(rev, tracks)
            self.assertEqual(len(h), want)
            self.assertEqual(compact_movies.bink_header_len(h + b'frames'), want)


if __name__ == '__main__':
    unittest.main()
