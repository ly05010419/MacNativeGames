"""The opening overlay's one-keyframe WebM without ffmpeg (games/oot/scripts/webm_one_frame.py, 2026-11-12).

No game data here: a synthetic movie stands in for the opening (one AV1 track, a Segment far bigger than the bytes read,
a keyframe then delta frames). With the real opening the bytes equal ffmpeg's bit-exact remux; make_opening_overlay.py
checks that against MOVIE_ONE_FRAME_SHA on every build."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'games/oot/scripts'))
import webm_one_frame as w  # noqa: E402


def el(eid, payload, width=None):
    return w._el(eid, payload, width)


def movie(first_flags=0x80, tracks=1, segment_size=50_000_000):
    """A WebM like the opening: EBML header, a Segment whose size runs far past the bytes given, one cluster."""
    header = el(w.EBML_HEADER, el(0x4282, b'webm') + el(0x4287, b'\x02'))
    video = el(w.VIDEO, el(w.PIXEL_WIDTH, (1920).to_bytes(2, 'big')) + el(w.PIXEL_HEIGHT, (1080).to_bytes(2, 'big')))
    entry = (el(w.TRACK_NUMBER, b'\x01') + el(w.TRACK_UID, b'\x4e\x3a\x01\x91\xff\xcd\x1a\x63') + el(w.FLAG_LACING, b'\x00')
             + el(w.LANGUAGE, b'eng') + el(w.CODEC_ID, b'V_AV1') + el(w.TRACK_TYPE, b'\x01')
             + el(w.DEFAULT_DURATION, (16_666_666).to_bytes(3 + 1, 'big')[1:]) + video + el(w.CODEC_PRIVATE, b'\x81\x09\x0c\x00'))
    track_list = el(w.TRACKS, el(w.TRACK_ENTRY, entry, 8) * tracks)
    info = el(w.INFO, el(w.TIMECODE_SCALE, (1_000_000).to_bytes(3, 'big')) + el(w.DURATION, struct.pack('>d', 154250.0)))
    blocks = [el(w.SIMPLE_BLOCK, b'\x81' + struct.pack('>h', 0) + bytes([first_flags]) + b'KEY' * 50)]
    blocks += [el(w.SIMPLE_BLOCK, b'\x81' + struct.pack('>h', 17 * n) + b'\x00' + b'delta' * 40) for n in range(1, 6)]
    cluster = el(w.CLUSTER, el(w.TIMECODE, b'\x00') + b''.join(blocks))
    body = info + track_list + cluster
    return header + w.SEGMENT.to_bytes(4, 'big') + w._size(segment_size, 8) + body   # the rest of the 50 MB is not read


class OneFrame(unittest.TestCase):
    def test_one_keyframe_of_the_original_track(self):
        short = w.one_frame(w.read_original(movie()))
        seen = w.inspect(short)
        self.assertEqual(seen['tracks'], [{'codec': 'V_AV1', 'width': 1920, 'height': 1080}])
        self.assertEqual(len(seen['blocks']), 1)
        self.assertTrue(seen['blocks'][0]['keyframe'])
        self.assertEqual(seen['blocks'][0]['payload'], b'KEY' * 50)   # the packet itself, not re-encoded
        self.assertEqual(seen['duration'], 0.016)   # one frame of 16.67 ms in 1 ms ticks, as ffmpeg wrote it
        self.assertEqual(seen['clusters'], 1)

    def test_ffmpeg_layout(self):
        """Info where ffmpeg reserves it, the track renumbered UID 1 / "und", Seek and Cue positions that point right."""
        short = w.one_frame(w.read_original(movie()))
        top = list(w.elements(short))
        segment_payload = top[1][2]
        self.assertEqual(short[top[1][1] + 4], 0x01)   # an 8-byte Segment size
        children = {eid: (begin - segment_payload) for eid, begin, _, _ in w.elements(short, segment_payload)}
        self.assertEqual(children[w.INFO], w.INFO_AT)
        self.assertIn(b'\x73\xc5\x88' + (1).to_bytes(8, 'big'), short)
        self.assertIn(b'\x22\xb5\x9c\x83und', short)
        self.assertIn(b'DURATION', short)
        seek_head = next(e for e in w.elements(short, segment_payload) if e[0] == w.SEEK_HEAD)
        for _, _, inner, finish in w.elements(short, seek_head[2], seek_head[3]):
            kids = {eid: short[a:b] for eid, _, a, b in w.elements(short, inner, finish)}
            self.assertEqual(children[int.from_bytes(kids[w.SEEK_ID], 'big')], int.from_bytes(kids[w.SEEK_POSITION], 'big'))
        self.assertIn(w._uint(w.CUE_CLUSTER_POSITION, children[w.CLUSTER]), short)

    def test_refuses_what_is_not_the_opening(self):
        with self.assertRaisesRegex(ValueError, 'keyframe'):
            w.one_frame(w.read_original(movie(first_flags=0x00)))
        with self.assertRaisesRegex(ValueError, 'one track'):
            w.read_original(movie(tracks=2))
        with self.assertRaisesRegex(ValueError, 'not a WebM'):
            w.read_original(el(0x4282, b'webm') * 4)
        with self.assertRaisesRegex(ValueError, 'bad EBML id'):
            w.read_original(b'\x00' * 64)

    def test_inspect_refuses_a_cut_file(self):
        short = w.one_frame(w.read_original(movie()))
        with self.assertRaises(ValueError):
            w.inspect(short[:-5])


if __name__ == '__main__':
    unittest.main()
