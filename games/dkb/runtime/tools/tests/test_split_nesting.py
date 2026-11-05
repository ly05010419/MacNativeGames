"""shader_split: the budgets of archives in archives (scan_blob, NEST). A Yaz0 member that expands is not overlap (GitHub issue #640: Mario Kart 8 Deluxe's UI/<lang>/race.sarc,
one layout archive that expands 9 times, was left out), while members that overlap and Yaz0 members that expand without end are still stopped early. No game files needed."""
import struct
import sys
import time
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # its builders (SARC, BNSH) and shader_split with a profile
ss = fz.ss


def yaz0(b):
    """Yaz0 that writes each run of one byte as back-references of distance 1 (at most 0x111 bytes each) and the rest as literals"""
    out = bytearray(b'Yaz0' + len(b).to_bytes(4, 'big') + bytes(8)); i = 0
    while i < len(b):
        head = len(out); out.append(0)
        for bit in range(8):
            if i >= len(b): break
            run = 0
            while i and i + run < len(b) and run < 0x111 and b[i + run] == b[i - 1]: run += 1
            if run >= 0x12: out += bytes([0, 0, run - 0x12])
            elif run >= 3: out += bytes([(run - 2) << 4, 0])
            else: out[head] |= 0x80 >> bit; out.append(b[i]); run = 1
            i += run
    return bytes(out)


def fanout(member, n=8, pad=0):
    """A SARC whose n members all span the same bytes, then pad zero bytes"""
    head = 0x14 + 0xc + 16 * n + 8; doff = (head + 0xff) & ~0xff
    d = b'SARC' + struct.pack('<HHII', 0x14, 0xfeff, doff + len(member) + pad, doff) + bytes(4) + b'SFAT' + struct.pack('<HHI', 0xc, n, 0x65)
    d += struct.pack('<IIII', 0, 0, 0, len(member)) * n + b'SFNT' + struct.pack('<HH', 8, 0)
    return d + bytes(doff - len(d)) + member + bytes(pad)


class Nesting(unittest.TestCase):
    def scan(self, d):
        out = []
        with mock.patch.object(ss, 'scan_blob', wraps=ss.scan_blob) as calls: ss.scan_blob(d, '/UI/EUde/race.sarc', out)
        return out, calls.call_count

    def test_compressed_member_is_not_overlap(self):
        # race.sarc's shape: one Yaz0 layout archive that expands about 9 times the file, its members as large as it; they added up to about 18 times the file
        inner = fz.sarc([(b'shader.bnsh', fz.bnsh())] + [(b'lyt%d.bflyt' % i, bytes(0x4000)) for i in range(8)])
        packed = yaz0(inner)
        outer = fz.sarc([(b'rc_L_Rank_00.szs', packed), (b'pad.bin', b'\1' * (len(inner) // 9 - len(packed)))])
        self.assertTrue(8 * len(outer) < len(inner) < 10 * len(outer))
        out, _ = self.scan(outer)
        self.assertEqual({w.split('#')[0] for w, *_ in out}, {'/UI/EUde/race.sarc/rc_L_Rank_00.szs/shader.bnsh'})

    def test_overlapping_members_are_stopped(self):
        d = (REPO / 'dm/tools/fuzz/corpus/shader_split/sarc-fanout.bin').read_bytes()
        with self.assertRaisesRegex(SystemExit, r'members add up to more than 16 times the file \(members that overlap\)'): self.scan(d)

    def test_compressed_overlapping_members_are_stopped(self):
        # every level 8 members spanning one Yaz0 of the next, padded to expand far: 8^6 scans without the budgets
        d = bytes(0x4000)
        for _ in range(6): d = fanout(yaz0(d), pad=0x4000)
        calls = []
        with self.assertRaisesRegex(SystemExit, 'more than 16 times the file'):
            with mock.patch.object(ss, 'scan_blob', wraps=ss.scan_blob) as m:
                try: ss.scan_blob(d, '/fanout', [])
                finally: calls.append(m.call_count)
        self.assertLess(calls[0], 200)

    def test_yaz0_expansion_is_charged_before_it_is_decoded(self):
        # one member, no overlap, that expands about 87 times: the second budget stops it from its header, without decoding it
        d = fz.sarc([(b'bomb.szs', yaz0(bytes(4 << 20)))])
        with mock.patch.object(ss, 'yaz0', wraps=ss.yaz0) as dec, self.assertRaisesRegex(SystemExit, 'Yaz0 archive members expand to more than 16 times the file'):
            ss.scan_blob(d, '/bomb.sarc', [])
        self.assertEqual(dec.call_count, 0)


class MemberNames(unittest.TestCase):
    def test_names_and_time(self):   # each name ends at its own NUL; splitting the rest of the archive per member made Fire Emblem Fortune's Weave's 9.8 MB layout SARC take 6 minutes
        members = [(b'tex%04d.bntx' % i, bytes(0x4000)) for i in range(800)]
        archive = fz.sarc(members)   # built outside the timing: only the member reading is measured (about 1 ms here, 47 s with the per-member split)
        t = time.monotonic(); got = list(ss.sarc_members(archive)); took = time.monotonic() - t
        self.assertEqual(got, [(n.decode(), m) for n, m in members])
        self.assertLess(took, 1.0)


if __name__ == '__main__':
    unittest.main()
