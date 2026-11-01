"""make_botw_button_icons.py: Breath of the Wild's prompt icons follow the layout by pointing code points of
External_00.bffnt's CMAP at other glyphs, in every Font/Font_*.sbfarc, written back as stored Yaz0 with the original's
alignment word. A synthetic font in a fake RomFS stands in for the game's."""
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
import make_botw_button_icons as icons      # noqa: E402
from prepare_shader_seed import yaz0         # noqa: E402
from test_mod_manifest import fake_romfs     # noqa: E402

SCRIPT = ROOT/'games/totk/scripts/make_botw_button_icons.py'
LO, HI = 0xE040, 0xE047


def bffnt():
    """FFNT header, a FINF block and one CMAP (method 1, a glyph index table for U+E040-E047)."""
    finf = b'FINF' + struct.pack('<I', 0x20) + bytes(0x18)
    table = struct.pack(f'<{HI - LO + 1}H', *range(HI - LO + 1))
    cmap = b'CMAP' + struct.pack('<IIIHHI', 24 + len(table), LO, HI, 1, 0, 0) + table
    return b'FFNT\xfe\xff' + struct.pack('<HIIHH', 0x14, 0x04010000, 0x14 + len(finf) + len(cmap), 2, 0) + finf + cmap


def sarc(members):
    """A little-endian SARC holding members (name -> bytes), each 4-aligned."""
    names, data, nodes = bytearray(), bytearray(), bytearray()
    for name, body in members.items():
        nodes += struct.pack('<IIII', 0, 0x01000000 | len(names) // 4, len(data), len(data) + len(body))
        names += name.encode() + b'\0'; names += bytes(-len(names) % 4)
        data += body; data += bytes(-len(data) % 4)
    head = b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + nodes + b'SFNT' + struct.pack('<HH', 8, 0) + names
    start = 0x14 + len(head); start += -start % 0x10
    out = b'SARC' + struct.pack('<H', 0x14) + b'\xff\xfe' + struct.pack('<IIHH', start + len(data), start, 0x100, 0)
    return out + head + bytes(start - 0x14 - len(head)) + bytes(data)


ARCHIVE = sarc({'Ancient_00.bffnt': b'ancient', icons.MEMBER: bffnt()})


def flyt():
    """A PaDoCommandA_00 in miniature: the diamond's backing, three letters and the A button (a window and the action's
    name) under N_Loop_00, and the name's copy N_CaptureA_00 under the root, with the part's two textures."""
    def pane(magic, name, t=(0.0, 0.0), base=0, text=None):
        body = bytes((1, base, 255, 0)) + name.encode().ljust(24, b'\0') + bytes(8) + struct.pack('<10f', *t, 0, 0, 0, 0, 1, 1,
                                                                                                    30, 30)
        if text is not None:
            string = text.encode('utf-16-le') + b'\0\0'
            body += struct.pack('<HHHHBBHfI', len(string), len(string), 0, 0, base, 0, 0, 0, 0x54 + 0x18) + bytes(4) + string
        return magic.encode() + struct.pack('<I', 8 + len(body)) + body
    names = [b'Nt_KeyBtnSubBaseAL_00^s\0', b'Nt_KeyTexA_00^d\0']
    txl = struct.pack('<HH', 2, 0) + struct.pack('<II', 8, 8 + len(names[0])) + b''.join(names)
    pas, pae = b'pas1' + struct.pack('<I', 8), b'pae1' + struct.pack('<I', 8)
    sections = [b'txl1' + struct.pack('<I', 8 + len(txl)) + txl, pane('pan1', 'RootPane'), pas,
                pane('pan1', 'N_Loop_00'), pas, pane('pic1', 'P_Base_00'), pane('txt1', 'T_B_00', (1, -26), text='B'),
                pane('txt1', 'T_Y_00', (-25.5, -0.2), text='Y'), pane('txt1', 'T_X_00', (0.8, 26), text='X'),
                pane('pan1', 'N_BtnA_00', (26, 0)), pas, pane('wnd1', 'W_BaseA_00', (-33, 0), base=1),
                pane('txt1', 'T_GuideA_01', (21, 0), base=1, text='Apri'), pae, pae,
                pane('pan1', 'N_CaptureA_00', (86, 4)), pae]
    body = b''.join(sections)
    return b'FLYT\xff\xfe' + struct.pack('<HIIHH', 0x14, 0x08000000, 0x14 + len(body), len(sections), 0) + body


def lz(data):
    """A plain greedy Yaz0 encoder (back references up to 4 KiB), to give yaz0_patch a stream with copies in it."""
    out, i = bytearray(b'Yaz0' + struct.pack('>II', len(data), 0) + bytes(4)), 0
    while i < len(data):
        code, group = 0, bytearray()
        for bit in range(8):
            if i >= len(data):
                break
            best = max(((d, next((n for n in range(min(0x111, len(data) - i)) if data[i + n - d] != data[i + n]),
                                 min(0x111, len(data) - i))) for d in range(1, min(i, 4096) + 1)), key=lambda m: m[1],
                       default=(0, 0))
            if best[1] >= 3:
                d, n = best[0] - 1, best[1]
                group += bytes(((n - 2) << 4 | d >> 8, d & 0xff)) if n < 18 else bytes((d >> 8, d & 0xff, n - 0x12))
                i += n
            else:
                code |= 0x80 >> bit; group.append(data[i]); i += 1
        out.append(code); out += group
    return bytes(out)


def at(flyt_bytes, name):
    """(translation x, y, rotation z, scale x, origin bits, text bits, text) of a pane."""
    [k] = [k for _, k, n, _ in icons.flyt_panes(flyt_bytes) if n == name]
    text = None
    if flyt_bytes[k:k+4] == b'txt1':
        t = k + struct.unpack_from('<I', flyt_bytes, k + 0x64)[0]
        text = flyt_bytes[t:t + struct.unpack_from('<H', flyt_bytes, k + 0x56)[0] - 2].decode('utf-16-le')
    x, y = struct.unpack_from('<2f', flyt_bytes, k + 0x2c)
    return (round(x, 1), round(y, 1), struct.unpack_from('<f', flyt_bytes, k + 0x40)[0],
            struct.unpack_from('<f', flyt_bytes, k + 0x44)[0], flyt_bytes[k + 9] & 3,
            flyt_bytes[k + 0x5c] & 3 if text is not None else None, text)


def textures(flyt_bytes):
    i = flyt_bytes.index(b'Nt_KeyBtnSubBase'); j = flyt_bytes.index(b'Nt_KeyTex')
    return chr(flyt_bytes[i + 16]), chr(flyt_bytes[j + 9])


class Guide(unittest.TestCase):
    """The action guide's diamond: the Xbox letters in their places, the driving button lit, mirrored when it moves to or
    from the left, its backing open where it is lit."""

    def test_the_lit_button_moves_and_the_letters_take_the_xbox_places(self):
        out = icons.diamond(flyt(), 'right', 'bottom')
        self.assertEqual((len(out), at(out, 'N_BtnA_00')[:2], at(out, 'N_CaptureA_00')[:2]),
                         (len(flyt()), (0.0, -26.0), (60.0, -22.0)))
        self.assertEqual([at(out, n)[:2] + at(out, n)[-1:] for n in ('T_B_00', 'T_Y_00', 'T_X_00')],
                         [(26.2, 0.8, 'B'), (0.8, 26.0, 'Y'), (-25.5, -0.2, 'X')])
        self.assertEqual((textures(out), at(out, 'P_Base_00')[2:4]), (('B', 'A'), (0.0, 1.0)))
        self.assertEqual(at(out, 'W_BaseA_00')[:4], (-33.0, 0.0, 0.0, 1.0))           # not mirrored

    def test_to_the_left_it_is_mirrored(self):
        out = icons.diamond(flyt(), 'right', 'left')
        self.assertEqual((at(out, 'N_BtnA_00')[:2], at(out, 'W_BaseA_00')[:4], at(out, 'T_GuideA_01')[:1] +
                          at(out, 'T_GuideA_01')[4:]), ((-26.0, 0.0), (33.0, 0.0, 0.0, -1.0), (-21.0, 2, 2, 'Apri')))
        self.assertEqual((textures(out), at(out, 'P_Base_00')[2:4]), (('A', 'X'), (180.0, 1.0)))
        back = icons.diamond(out, 'left', 'right')
        self.assertEqual(at(back, 'T_GuideA_01')[:1] + at(back, 'T_GuideA_01')[4:], (21.0, 1, 1, 'Apri'))

    def test_at_home_only_the_letters_change(self):
        out = icons.diamond(flyt(), 'right', 'right')
        self.assertEqual((at(out, 'N_BtnA_00')[:2], textures(out)), ((26.0, 0.0), ('A', 'B')))
        self.assertEqual([at(out, n)[-1] for n in ('T_B_00', 'T_Y_00', 'T_X_00')], ['A', 'Y', 'X'])

    def test_a_patched_stream_decodes_to_the_new_bytes_and_hardly_grows(self):
        old = b'abcdefgh' * 40 + bytes(range(48)) + b'abcdefgh' * 20
        packed = lz(old)
        self.assertEqual(yaz0(packed), old)
        new = bytearray(old); new[17] = ord('Z'); new[330] = 0xEE; new = bytes(new)
        patched = icons.yaz0_patch(packed, old, new)
        self.assertEqual((yaz0(patched), patched[:16]), (new, packed[:16]))
        self.assertLess(len(patched), len(icons.yaz0_stored(new, 0)))                 # copies far from the change stay
        self.assertEqual(icons.yaz0_patch(packed, old, old), packed)                    # nothing changed: the same stream

    def test_a_member_grows_and_the_ones_after_it_keep_their_alignment(self):
        pack = sarc({'a': b'x' * 5, 'b': b'y' * 8, 'c': b'z' * 4})
        out = icons.sarc_put(pack, 'b', b'Y' * 21)
        self.assertEqual([out[slice(*icons.sarc_member(out, n))] for n in 'abc'], [b'x' * 5, b'Y' * 21, b'z' * 4])
        self.assertEqual((icons.sarc_member(out, 'c')[0] % 4, struct.unpack_from('<I', out, 8)[0]), (0, len(out)))


def run(*args):
    return subprocess.run([sys.executable, '-I', str(SCRIPT), *map(str, args)], capture_output=True, text=True)


class Remap(unittest.TestCase):
    def test_only_the_named_table_entries_change(self):
        font = bffnt()
        table, lo, hi = icons.cmap_table(font)
        out = icons.remap(font, {0xE042: 0xE043, 0xE043: 0xE042, 0xE040: 0xE047})
        self.assertEqual((lo, hi, len(out)), (LO, HI, len(font)))
        self.assertEqual(struct.unpack_from('<8H', out, table), (7, 1, 3, 2, 4, 5, 6, 7))
        self.assertEqual([i for i in range(len(font)) if font[i] != out[i]], [table, table + 4, table + 6])
        self.assertRaises(AssertionError, icons.remap, font, {0xE048: 0xE040})   # outside the table

    def test_stored_yaz0_round_trips_and_keeps_the_alignment(self):
        for data in (b'', b'x', bytes(range(256)) * 3 + b'tail'):
            packed = icons.yaz0_stored(data, 0x1000)
            self.assertEqual((yaz0(packed), packed[8:12]), (data, b'\0\0\x10\0'))

    def test_the_member_is_found_by_name(self):
        a, b = icons.sarc_member(ARCHIVE, icons.MEMBER)
        self.assertEqual(ARCHIVE[a:b], bffnt())
        self.assertRaises(KeyError, icons.sarc_member, ARCHIVE, 'Normal_00.bffnt')


class Script(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.romfs, self.mod, self.glyphs = root/'romfs', root/'mod', root/'glyphs.json'
        packed = icons.yaz0_stored(ARCHIVE, 0x2000)
        fake_romfs(self.romfs, {'Font/Font_EU.sbfarc': packed, 'Font/Font_CN.sbfarc': packed, 'Pack/A.pack': b'a'})

    def make(self, glyphs, *more):
        self.glyphs.write_text(json.dumps({'glyphs': glyphs, 'diamond': {}}))
        return run(self.mod, '--romfs', self.romfs, '--plan', self.glyphs, *more)

    def test_every_region_archive_is_written_with_its_manifest(self):
        done = self.make([[0xE042, 0xE043], [0xE043, 0xE042]])
        self.assertEqual(done.returncode, 0, done.stderr)
        for region in ('EU', 'CN'):
            packed = (self.mod/f'Font/Font_{region}.sbfarc').read_bytes()
            sarc_out = yaz0(packed)
            a, b = icons.sarc_member(sarc_out, icons.MEMBER)
            table, _, _ = icons.cmap_table(sarc_out[a:b])
            self.assertEqual((packed[8:12], struct.unpack_from('<4H', sarc_out, a + table)), (b'\0\0\x20\0', (0, 1, 3, 2)))
            self.assertEqual(sarc_out[:a] + sarc_out[b:], ARCHIVE[:a] + ARCHIVE[b:])
        lines = (self.mod/'manifest.tsv').read_text().splitlines()[1:]
        self.assertEqual(sorted(line.split('\t')[0] for line in lines), ['Font/Font_CN.sbfarc', 'Font/Font_EU.sbfarc'])

    def test_a_layout_that_moves_nothing_writes_no_archive(self):
        done = self.make([])
        self.assertEqual((done.returncode, list((self.mod/'Font').glob('*')) if (self.mod/'Font').exists() else []),
                         (0, []))

    def test_its_own_archives_are_replaced_and_removed_another_mods_stop_it(self):
        self.assertEqual(self.make([[0xE040, 0xE041]]).returncode, 0)
        self.assertEqual(self.make([[0xE040, 0xE042]]).returncode, 0)          # an earlier run's: replaced
        self.assertEqual(self.make([]).returncode, 0)
        self.assertFalse((self.mod/'Font/Font_EU.sbfarc').exists())            # back to the game's own icons
        (self.mod/'Font').mkdir(exist_ok=True)
        (self.mod/'Font/Font_EU.sbfarc').write_bytes(b'a translation')
        refused = self.make([[0xE040, 0xE041]])
        self.assertIn("another mod's font archive", refused.stderr)
        self.assertEqual(((self.mod/'Font/Font_EU.sbfarc').read_bytes(), (self.mod/'Font/Font_CN.sbfarc').exists()),
                         (b'a translation', False))                            # nothing written

    def test_a_misspelt_path_stops_it_before_anything_is_written(self):
        (self.mod/'pack').mkdir(parents=True)
        (self.mod/'pack/A.pack').write_bytes(b'b')                              # the RomFS has Pack/A.pack
        refused = self.make([[0xE040, 0xE041]])
        self.assertIn('pack/A.pack (the RomFS has Pack/A.pack)', refused.stderr)
        self.assertFalse((self.mod/'Font').exists() or (self.mod/'manifest.tsv').exists())

    def test_write_all_writes_archives_that_keep_their_icons(self):
        done = self.make([], '--write-all')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(yaz0((self.mod/'Font/Font_EU.sbfarc').read_bytes()), ARCHIVE)


if __name__ == '__main__':
    unittest.main()
