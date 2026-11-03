"""shader_split.bea_members: NintendoWare Bezel Engine archives in both forms. 1.56 (Big Brain Academy: Brain vs. Brain) has at +0x28 the offset of an array of entry offsets,
each pointing at a 0x30-byte 'ASST' entry; 2.9 (Super Mario Party Jamboree) has the entries themselves there, back to back, 0x48 bytes each (the size at 'ASST' +4), with a type tag
and a hash before the data and name offsets. Synthetic archives, no game files needed."""
import contextlib
import io
import os
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '0100620012D6E000'   # shader_split loads a profile at import; other tests expect no default game
import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

FILES = [('a/one.bfsha', b'BNSH\0\0\0\0' + bytes(range(256)) * 8, True), ('b/two.txt', b'plain entry', False)]


def entry(size, kind, stored, length, data_off, name_off):
    e = b'ASST' + struct.pack('<I', size) + struct.pack('<Q', size) + struct.pack('<HHII', kind, 6, stored, length)
    if size == 0x48: e += b'_G3DSHA\0' + bytes(4) + bytes(range(16))   # the type tag and the hash
    else: e += bytes(4)
    return e + struct.pack('<QQ', data_off, name_off)


def archive(inline):
    size = 0x48 if inline else 0x30
    arr = 0x40
    ents = arr + (0 if inline else 8 * len(FILES))
    names, blobs, recs = bytearray(), bytearray(), []
    names_at = ents + size * len(FILES)
    for path, data, packed in FILES:
        p = path.encode()
        recs.append((names_at + len(names), data, ss.zstd_mod().compress(data) if packed else data))
        names += struct.pack('<H', len(p)) + p
    data_at = names_at + len(names)
    out = bytearray(b'SCNE' + bytes(4) + struct.pack('<I', 0x60000 if inline else 0x10100) + bytes(0x14) + struct.pack('<QQ', len(FILES), arr))
    out += bytes(arr - len(out))
    body = bytearray()
    for k, (name_off, data, stored) in enumerate(recs):
        if not inline: out += struct.pack('<Q', ents + size * k)
        body += entry(size, 2, len(stored), len(data), data_at + len(blobs), name_off)
        blobs += stored
    return bytes(out + body + names + blobs)


def members(d):
    out = io.StringIO()
    with contextlib.redirect_stdout(out): r = list(ss.bea_members(d, '/test.bea'))
    return r, out.getvalue()


class BeaMembers(unittest.TestCase):
    def test_pointer_array(self):   # Bezel Engine 1.56
        self.assertEqual(members(archive(False)), ([(p, d) for p, d, _ in FILES], ''))

    def test_inline_entries(self):  # Bezel Engine 2.9
        self.assertEqual(members(archive(True)), ([(p, d) for p, d, _ in FILES], ''))


if __name__ == '__main__':
    unittest.main()
