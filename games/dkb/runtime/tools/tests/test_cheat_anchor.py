"""scripts/cheat_anchor.py: Atmosphère cheats anchored on one build and compiled for another, on synthetic NSO files built
from real AArch64 encodings (no game files). Relocated code and data, a changed callee, a resized function, pointer
slots (RELA and RELR) and code caves are carried over; changed or relocated words, ambiguous or disagreeing evidence and
outputs this tool did not write are refused, and the emitted words are checked against the VM's encodings. The rules on
what may be written over a relocated or PC-relative word hold for patches and gated code templates alike; data anchors
stay in their target's segment; malformed files and undecodable cheats are never shortened or dropped silently.
"""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import tomllib
import unittest
import warnings

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
from nso_asm import ADRP, ADDI, B, NOP

spec = importlib.util.spec_from_file_location('cheat_anchor', REPO / 'scripts/cheat_anchor.py')
assert spec and spec.loader   # a path that exists: narrows the Optional for the type checker
ca = importlib.util.module_from_spec(spec); spec.loader.exec_module(ca)

RET = 0xd65f03c0
TEXT_END, RO, DATA, SIZE, BSS = 0x1c00, 0x2000, 0x4000, 0x6000, 0x1000   # the layout every synthetic build uses


def LDRH(rt, rn, off):
    """ldrh wt, [xn, #off]: a u16 field load, the kind of instruction the stat cheats read."""
    return 0x79400000 | (off // 2) << 10 | rn << 5 | rt


def LDRW(rt, rn, off):
    """ldr wt, [xn, #off]: a u32 load, here used as an adrp's user."""
    return 0xb9400000 | (off // 4) << 10 | rn << 5 | rt


def LDRX(rt, rn, off):
    """ldr xt, [xn, #off]: a pointer load from a .data slot."""
    return 0xf9400000 | (off // 8) << 10 | rn << 5 | rt


def MOVZ(rd, imm):
    """mov wd, #imm: distinct filler instructions around a code site."""
    return 0x52800000 | imm << 5 | rd


def BL(frm, to):
    """bl from frm to to (nso_asm.B with the link bit)."""
    return B(frm, to) | 0x80000000


def BCOND(cond, frm, to):
    """b.<cond> from frm to to (cond 0 eq, 1 ne): the branch a "flip b.ne to b.eq" cheat writes."""
    return 0x54000000 | (((to - frm) // 4) & 0x7ffff) << 5 | cond


def reader(start, data, field=0x1e, extra=()):
    """A function that loads a u16 field of the object at data: adrp x8; add x8, x8, lo; [extra]; ldrh w0, [x8, #field]; ret."""
    return [ADRP(8, start, data), ADDI(8, 8, data & 0xfff), *extra, LDRH(0, 8, field), RET]


def slot_reader(start, slot):
    """A function that loads the pointer in a .data slot, then a field of what it points to."""
    return [ADRP(9, start, slot), LDRX(9, 9, slot & 0xfff), LDRH(0, 9, 0x38), RET]


def image(functions, relocs=(), relr=()):
    """The memory image: functions {start: words} with an .eh_frame_hdr describing them, MOD0, and a dynamic section
    with the relative relocations, as DT_RELA entries [(slot, pointer)] and as DT_RELR slots [(slot, pointer)] (the
    pointer then lies in the slot itself, RELR's implicit addend)."""
    img = bytearray(SIZE)
    struct.pack_into('<I', img, 4, 0x20)
    img[0x20:0x24] = b'MOD0'
    struct.pack_into('<i', img, 0x24, 0x2100 - 0x20)                               # dynamic section
    struct.pack_into('<ii', img, 0x30, 0x3000 - 0x20, 0x300c + len(functions) * 8 - 0x20)
    tags = [(7, 0x2200), (8, 24 * len(relocs)), (9, 24)] + ([(36, 0x2400), (35, 8 * len(relr))] if relr else []) + [(0, 0)]
    for i, (tag, value) in enumerate(tags): struct.pack_into('<qQ', img, 0x2100 + i * 16, tag, value)
    for i, (slot, ptr) in enumerate(relocs): struct.pack_into('<QQq', img, 0x2200 + i * 24, slot, ca.R_AARCH64_RELATIVE, ptr)
    for i, (slot, ptr) in enumerate(relr):
        struct.pack_into('<Q', img, 0x2400 + i * 8, slot); struct.pack_into('<Q', img, slot, ptr)
    img[0x3000:0x3004] = bytes((1, 0x1b, 3, 0x3b))                                 # eh_frame_hdr (test_nso_match's)
    struct.pack_into('<iI', img, 0x3004, 0x3200 - 0x3004, len(functions))
    struct.pack_into('<II', img, 0x3200, 16, 0)
    img[0x3208:0x3211] = b'\x01zR\0\x04\x78\x1e\x01\x1b'
    for i, (start, words) in enumerate(sorted(functions.items())):
        struct.pack_into('<' + 'I' * len(words), img, start, *words)
        fde = 0x3240 + i * 24
        struct.pack_into('<ii', img, 0x300c + i * 8, start - 0x3000, fde - 0x3000)
        struct.pack_into('<IIiI', img, fde, 16, fde + 4 - 0x3200, start - (fde + 8), len(words) * 4)
    return img


def nso(path, functions, relocs=(), relr=(), build=b'\x11' * 8, text_end=TEXT_END, data=DATA):
    """Write an uncompressed NSO of image() with the usual three segments plus bss, and load it as a Build. data moves
    the start of .data (a build whose .rodata grew); the image's own tables stay below it."""
    img = image(functions, relocs, relr)
    hdr = bytearray(0x100); hdr[:4] = b'NSO0'
    segs = ((0, text_end), (RO, data - RO), (data, SIZE - data))
    fo = 0x100
    for i, (mo, sz) in enumerate(segs):
        struct.pack_into('<III', hdr, 0x10 + i * 0x10, fo, mo, sz); fo += sz
    struct.pack_into('<I', hdr, 0x3c, BSS)
    hdr[0x40:0x48] = build
    struct.pack_into('<III', hdr, 0x60, *(sz for _, sz in segs))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(bytes(hdr) + b''.join(bytes(img[mo:mo + sz]) for mo, sz in segs))
    return ca.Build(path)


A, B2 = b'\x11' * 8, b'\x22' * 8   # the two build IDs most tests use: 1111111111111111 and 2222222222222222


class Anchors(unittest.TestCase):
    """Import on one synthetic build, compile on another, and check the words the VM will run."""
    def setUp(self):
        """A temporary folder per test; nso_syms.load reads the NSO through an unclosed open(), its warning is noise."""
        warnings.simplefilter('ignore', ResourceWarning)
        self.tmp = tempfile.TemporaryDirectory(); self.dir = Path(self.tmp.name)

    def tearDown(self):
        """Remove the temporary folder."""
        self.tmp.cleanup()

    def build(self, name, functions, **kw):
        """A synthetic build in the temporary folder."""
        return nso(self.dir / name, functions, **kw)

    def txt(self, text, name='cheats.txt'):
        """An Atmosphère file in the temporary folder."""
        p = self.dir / name; p.write_text(text); return p

    def roundtrip(self, old, new, text, name='cheats.txt', **kw):
        """Import text on old, compile on new: (entries by name, report lines, skipped count)."""
        src, _, _ = ca.import_file(self.txt(text, name), old, **kw)
        ca.write_toml(src, self.dir / 'g.cheats.toml', name)
        _, entries, report, skipped = ca.compile_source(self.dir / 'g.cheats.toml', new)
        return {e[0]: e for e in entries}, report, skipped

    def source(self, text, name='s.cheats.toml'):
        """A hand-written source in the temporary folder."""
        p = self.dir / name; p.write_text(text); return p

    # ---- data anchors ----

    def test_data_field_follows_the_object_the_code_references(self):
        """A stat field keeps its offset from the object the code references, wherever the object moves."""
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        new = self.build('b', {0x300: reader(0x300, 0x4180), 0x500: reader(0x500, 0x4180, 0x20)}, build=B2)
        got, report, skipped = self.roundtrip(old, new, '[999 strength]\n02000000 0000411E 000003E7\n')
        self.assertEqual(skipped, 0, report)
        self.assertEqual(got['999 strength'][2], [0x02000000, 0x419e, 0x3e7])   # 0x4180 + the same field offset 0x1e
        self.assertIn('2/2 refs (2 exact)', got['999 strength'][3]['how'])

    def test_the_nearest_referenced_address_wins_over_a_busier_one(self):
        """The review's silent-misplacement case: a busier neighbour must not become the base of an unrelated field."""
        # A at 0x4100 is read by two functions, the target B at 0x4200 by one; only B moves in the new build
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20), 0x300: reader(0x300, 0x4200)})
        new = self.build('b', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20), 0x300: reader(0x300, 0x4300)}, build=B2)
        got, report, skipped = self.roundtrip(old, new, '[B]\n02000000 00004200 00000001\n')
        self.assertEqual(skipped, 0, report)
        self.assertEqual(got['B'][2][1], 0x4300); self.assertIn('weak: one reference', got['B'][3]['how'])
        self.assertTrue(any('weakly' in r for r in report))

    def test_disagreeing_or_missing_references_are_refused(self):
        """References that disagree, or too few of them, refuse the cheat instead of guessing."""
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        split = self.build('b', {0x300: reader(0x300, 0x4180), 0x500: reader(0x500, 0x4240, 0x20)}, build=B2)
        got, report, skipped = self.roundtrip(old, split, '[x]\n02000000 0000411E 000003E7\n')
        self.assertEqual(skipped, 1); self.assertIn('disagree', report[0])
        one = self.build('c', {0x300: reader(0x300, 0x4180)}, build=b'\x33' * 8)
        _, report, skipped = self.roundtrip(old, one, '[x]\n02000000 0000411E 000003E7\n')
        self.assertEqual(skipped, 1); self.assertIn('only 1 of its 2', report[0])

    def test_pointer_slot_is_followed_through_its_relocation(self):
        """An object reached only through a relocated .data slot (FFCC's Mog) is found through the slot's relocation."""
        # the only code reference is to a .data slot that holds a relocated pointer to the object
        old = self.build('a', {0x100: slot_reader(0x100, 0x4008), 0x200: slot_reader(0x200, 0x4008)}, relocs=[(0x4008, 0x5000)])
        new = self.build('b', {0x400: slot_reader(0x400, 0x4010), 0x600: slot_reader(0x600, 0x4010)}, relocs=[(0x4010, 0x5400)], build=B2)
        got, report, skipped = self.roundtrip(old, new, '[Mog never tired]\n04000000 00005038 00000000\n')
        self.assertEqual(skipped, 0, report)
        self.assertEqual(got['Mog never tired'][2][:2], [0x04000000, 0x5438])
        self.assertIn('through the slot 0x4010', got['Mog never tired'][3]['how'])

    def test_two_slots_of_one_object_agree_on_the_pointer_and_relr_is_read(self):
        """Two slots of one object agree on its pointer, not on the slots, and DT_RELR relocations are read too."""
        old = self.build('a', {0x100: slot_reader(0x100, 0x4008), 0x200: slot_reader(0x200, 0x4010)}, relocs=[(0x4008, 0x5000), (0x4010, 0x5000)])
        got, report, skipped = self.roundtrip(old, old, '[Mog]\n04000000 00005038 00000000\n')
        self.assertEqual(skipped, 0, report); self.assertEqual(got['Mog'][2][1], 0x5038)
        self.assertIn('through the slots 0x4008, 0x4010', got['Mog'][3]['how'])
        new = self.build('b', {0x400: slot_reader(0x400, 0x4020), 0x600: slot_reader(0x600, 0x4028)}, relr=[(0x4020, 0x5200), (0x4028, 0x5200)], build=B2)
        _, entries, report, skipped = ca.compile_source(self.dir / 'g.cheats.toml', new)
        self.assertEqual(skipped, 0, report); self.assertEqual(entries[0][2][1], 0x5238)

    # ---- code anchors ----

    def test_code_patch_relocates_its_branch_to_the_cave_and_keeps_both_names(self):
        """An ASM cheat: guarded words, the BL to its cave re-encoded at the moved site, both on / off names kept."""
        site_words = lambda s: [0xaa1703e0, NOP, LDRH(0, 0, 0x34), RET]
        old = self.build('a', {0x100: site_words(0x100)})
        new = self.build('b', {0x900: site_words(0x900)}, build=B2, text_end=0x1800)
        cave = RO - 0x100   # the cheat's code in the padding, 0x100 before the read-only data in both builds
        on = f'[Hack - on]\n04000000 {cave:08X} D2800201\n04000000 {cave + 4:08X} D65F03C0\n04000000 00000108 {BL(0x108, cave):08X}\n'
        off = f'[Hack - off]\n04000000 00000108 {LDRH(0, 0, 0x34):08X}\n'
        got, report, skipped = self.roundtrip(old, new, on + off)
        self.assertEqual(skipped, 0, report)
        prog = got['Hack - on'][2]
        guard = [0x14050000, 0x908, LDRH(0, 0, 0x34)]   # "if it still holds the original", at the new site
        self.assertEqual(prog[-7:], guard + [0x04000000, 0x908, BL(0x908, cave), 0x20000000])
        self.assertEqual(prog[:7], [0x14050000, cave, 0, 0x04000000, cave, 0xd2800201, 0x20000000])   # cave words: "if still 0"
        self.assertEqual(got['Hack - off'][2], [0x14050000, 0x908, BL(0x908, cave), 0x04000000, 0x908, LDRH(0, 0, 0x34), 0x20000000])
        self.assertEqual(got['Hack - on'][3]['pair'], 'Hack - off')
        _, entries, _, _ = ca.compile_source(self.dir / 'g.cheats.toml', new, plain=True)
        self.assertEqual(entries[0][2][-3:], [0x04000000, 0x908, BL(0x908, cave)])   # --plain: no guards

    def test_pairs_keep_their_order_and_a_pair_that_restores_elsewhere_is_not_merged(self):
        """Merged pairs keep the file's entry order (it decides which runs last); an off entry that restores another word stays separate."""
        f = [0xaa1703e0, NOP, LDRH(0, 0, 0x34), 0x1e2e1000, RET]
        old = self.build('a', {0x100: f})
        text = '[Y - off]\n04000000 0000010C 1E2E1000\n[Y - on]\n04000000 0000010C 1E2F1000\n'
        text += '[X - on]\n04000000 0000010C 1E2F1000\n[X - off]\n04000000 00000104 D503201F\n'   # restores another word
        src, _, _ = ca.import_file(self.txt(text), old)
        self.assertEqual([c['name'] for c in src['cheats']], ['Y', 'X - on', 'X - off'])
        ca.write_toml(src, self.dir / 'g.cheats.toml', 'cheats.txt')
        _, entries, _, _ = ca.compile_source(self.dir / 'g.cheats.toml', old)
        self.assertEqual([e[0] for e in entries[:2]], ['Y - off', 'Y - on'])   # off came first in the file: it still does

    def test_changed_callee_falls_back_to_the_body_and_resized_function_to_the_window(self):
        """The two fallbacks: a changed callee is found by the body digest, a resized function by the window."""
        # the site (0x1e2e1000, an fmul) sits in the middle of the function, four distinct instructions on each side
        around = [MOVZ(k, k) for k in range(1, 5)], [MOVZ(k, k) for k in range(5, 9)]
        def f(start, callee, head=()):
            """A function with an optional head, a call to callee and the site between the two runs of distinct words."""
            return [*head, 0xaa1703e0, BL(start + 4 * (1 + len(head)), callee), LDRH(0, 0, 0x34), *around[0], 0x1e2e1000, *around[1], RET]
        site = 4 * 7   # the site's offset in f without a head
        old = self.build('a', {0x100: f(0x100, 0x400), 0x400: [MOVZ(0, 1), RET]})
        changed = self.build('b', {0x200: f(0x200, 0x600), 0x600: [MOVZ(0, 2), RET]}, build=B2)
        text = f'[Speed]\n04000000 {0x100 + site:08X} 1E2F1000\n'
        got, report, skipped = self.roundtrip(old, changed, text)
        self.assertEqual(skipped, 0, report); self.assertEqual(got['Speed'][3]['how'], 'body')
        self.assertEqual(got['Speed'][2], [0x14050000, 0x200 + site, 0x1e2e1000, 0x04000000, 0x200 + site, 0x1e2f1000, 0x20000000])
        longer = self.build('c', {0x300: f(0x300, 0x700, (NOP,)), 0x700: [MOVZ(0, 1), RET]}, build=b'\x33' * 8)
        got, report, skipped = self.roundtrip(old, longer, text)
        self.assertEqual(skipped, 0, report); self.assertEqual(got['Speed'][3]['how'], 'window'); self.assertEqual(got['Speed'][2][1], 0x304 + site)

    def test_changed_original_word_and_ambiguous_window_are_refused(self):
        """A changed site word, or a window found twice, refuses the cheat."""
        f = lambda w: [0xaa1703e0, NOP, LDRH(0, 0, 0x34), w, RET]
        old = self.build('a', {0x100: f(0x1e2e1000)})
        other = self.build('b', {0x200: f(0x1e2e9000)}, build=B2)
        _, report, skipped = self.roundtrip(old, other, '[Speed]\n04000000 0000010C 1E2F1000\n')
        self.assertEqual(skipped, 1); self.assertIn('not in this build and 0 places match', report[0])
        # two identical copies where the old build had one: neither the fingerprint (2 copies, 1 expected) nor the window decides
        twice = self.build('c', {0x400: f(0x1e2e1000), 0x800: f(0x1e2e1000)}, build=b'\x33' * 8)
        _, report, skipped = self.roundtrip(old, twice, '[Speed]\n04000000 0000010C 1E2F1000\n')
        self.assertEqual(skipped, 1); self.assertIn('2 places match', report[0])

    def test_identical_copies_are_weak_by_order_and_checked_by_their_window(self):
        """Identical copies: by order is weak, a unique window confirms it, and a window that points elsewhere refuses it."""
        f = [0xaa1703e0, NOP, LDRH(0, 0, 0x34), 0x1e2e1000, RET]
        old = self.build('a', {0x100: f, 0x300: f})   # the same zero padding around both copies: the window cannot tell them apart
        new = self.build('b', {0x500: f, 0x900: f}, build=B2)
        got, report, skipped = self.roundtrip(old, new, '[Second]\n04000000 0000030C 1E2F1000\n')
        self.assertEqual(skipped, 0, report); self.assertEqual(got['Second'][2][1], 0x90c)
        self.assertIn('copy 2 of 2 by order: weak', got['Second'][3]['how'])
        # each copy followed by a function of its own: the window identifies the copy, and contradicts a changed order
        tail = lambda k: [MOVZ(k, k), MOVZ(k, k + 1), RET]
        old = self.build('c', {0x100: f, 0x114: tail(1), 0x300: f, 0x314: tail(2)})
        same_order = self.build('d', {0x500: f, 0x514: tail(1), 0x900: f, 0x914: tail(2)}, build=B2)
        got, report, skipped = self.roundtrip(old, same_order, '[Second]\n04000000 0000030C 1E2F1000\n')
        self.assertEqual(skipped, 0, report); self.assertIn('confirmed by its window', got['Second'][3]['how'])
        swapped = self.build('e', {0x500: f, 0x514: tail(2), 0x900: f, 0x914: tail(1)}, build=b'\x33' * 8)
        _, report, skipped = self.roundtrip(old, swapped, '[Second]\n04000000 0000030C 1E2F1000\n')
        self.assertEqual(skipped, 1); self.assertIn('its window says 0x50C', report[0])

    def test_a_relocated_word_is_restored_as_this_builds_own_and_never_patched_with_old_bits(self):
        """A relocated original (an adrp's user) is restored with the new build's own word and never patched with the old build's bits."""
        g = lambda s, data: [ADRP(8, s, data), LDRW(0, 8, data & 0xfff), RET]   # ldr w0, [x8, #lo]: an adrp's user
        old = self.build('a', {0x100: g(0x100, 0x4100)})
        new = self.build('b', {0x100: g(0x100, 0x4180)}, build=B2)
        src, report, bound = ca.import_file(self.txt(f'[Next field]\n04000000 00000104 {LDRW(0, 8, 0x104):08X}\n'), old)
        self.assertEqual(bound, 1); self.assertIn('takes the page of the adrp at 0x100', ' '.join(report))
        got, report, skipped = self.roundtrip(old, new, f'[Restore]\n04000000 00000104 {LDRW(0, 8, 0x100):08X}\n')
        self.assertEqual(skipped, 0, report); self.assertEqual(got['Restore'][2], [0x04000000, 0x104, LDRW(0, 8, 0x180)])

    def test_a_word_the_rewriter_changed_is_reported(self):
        """A site the rewriter changed is reported, found through the NSP script's folder layout."""
        f = [0xaa1703e0, NOP, LDRH(0, 0, 0x34), 0x1e2e1000, RET]
        old = self.build('a', {0x100: f})
        new = self.build('b/extract/v1/ExeFS/main', {0x200: f}, build=B2)   # the NSP script's layout
        rewritten = bytearray(new.img); struct.pack_into('<I', rewritten, 0x20c, 0x14000000)
        (self.dir / 'b/patched_v1').mkdir(); (self.dir / 'b/patched_v1/main.patched.bin').write_bytes(rewritten)
        new = ca.Build(self.dir / 'b/extract/v1/ExeFS/main')
        _, report, skipped = self.roundtrip(old, new, '[Speed]\n04000000 0000010C 1E2F1000\n')
        self.assertEqual(skipped, 0); self.assertTrue(any('the rewriter changed the word at 0x20C' in r for r in report), report)

    def test_width_8_cave_store_and_branch_tokens(self):
        """An 8-byte store into the cave lands low word first, and a BL token is re-encoded; out-of-range branches are refused."""
        f = [0xaa1703e0, NOP, LDRH(0, 0, 0x34), RET]
        old = self.build('a', {0x100: f})
        cave = RO - 0x40
        got, report, skipped = self.roundtrip(old, old, f'[Cave]\n08000000 {cave:08X} 11112222 33334444\n04000000 00000104 {BL(0x104, cave):08X}\n')
        self.assertEqual(skipped, 0, report)
        prog = got['Cave'][2]   # memory order: the low word at the address, the high word after it
        self.assertEqual(prog[:14], [0x14050000, cave, 0, 0x04000000, cave, 0x33334444, 0x20000000,
                                     0x14050000, cave + 4, 0, 0x04000000, cave + 4, 0x11112222, 0x20000000])
        self.assertEqual(prog[-4:-1], [0x04000000, 0x104, BL(0x104, cave)])
        with self.assertRaisesRegex(ca.AnchorError, 'out of range'): ca.encode_branch(True, 0, 1 << 28)

    def test_keypress_code_keeps_its_program_with_anchored_addresses(self):
        """A keypress-gated program keeps its words with only its address re-anchored; a glued header still starts a cheat."""
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        new = self.build('b', {0x300: reader(0x300, 0x4180), 0x500: reader(0x500, 0x4180, 0x20)}, build=B2)
        got, report, skipped = self.roundtrip(old, new, '[Hold A]\n80000001\n02000000 00004104 00000001\n20000000[Next]\n01000000 00004120 00000001\n')
        self.assertEqual(skipped, 0, report)
        self.assertEqual(got['Hold A'][2], [0x80000001, 0x02000000, 0x4184, 1, 0x20000000])
        self.assertIn('Next', got)   # a header glued to the previous line still starts a cheat

    def test_build_bound_cheats_and_the_file_name_check(self):
        """Unanchorable cheats stay bound to their own build, and a file named after another build needs --assume-build."""
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        new = self.build('b', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)}, build=B2)
        got, report, skipped = self.roundtrip(old, new, '[Heap]\n02100000 00001234 00000001\n')   # region 1: the heap
        self.assertEqual(skipped, 1); self.assertIn('written for build 1111111111111111 only', report[0])
        with self.assertRaisesRegex(ca.AnchorError, 'names build 2222222222222222'):
            ca.import_file(self.txt('[x]\n02000000 0000411E 00000001\n', '2222222222222222.txt'), old)
        src, report, bound = ca.import_file(self.txt('[x]\n02000000 0000411E 00000001\n[h]\n02100000 00001234 00000001\n', '2222222222222222.txt'), old, assume_build=True)
        self.assertTrue(report[0].startswith('[RED] ')); self.assertEqual(bound, 1)
        self.assertEqual(src['cheats'][1]['builds'], ['2222222222222222'])   # its fixed addresses are the file's build's

    # ---- what may be written over a moved instruction ----

    def test_a_gated_store_follows_the_same_relocation_rule_as_a_patch(self):
        """A keypress-gated store over a moved bl is refused like the plain patch; a gated restore writes this build's word."""
        fa = lambda s, callee: [MOVZ(1, 1), BL(s + 4, callee), MOVZ(2, 2), MOVZ(3, 3), RET]
        callee = [MOVZ(0, 7), MOVZ(5, 9), RET]
        old = self.build('a', {0x100: fa(0x100, 0x300), 0x300: callee})
        new = self.build('b', {0x140: fa(0x140, 0x400), 0x400: callee}, build=B2)
        bad = f'{LDRX(0, 1, 8):08X}'   # a load with an immediate: carries_address_bits, so not over a moved word
        text = f'[Gated]\n80000040\n04000000 00000104 {bad}\n20000000\n[Plain]\n04000000 00000104 {bad}\n'
        text += f'[Gated restore]\n80000040\n04000000 00000104 {BL(0x104, 0x300):08X}\n20000000\n'
        got, report, skipped = self.roundtrip(old, new, text)
        self.assertEqual(skipped, 2, report)
        reasons = [r.split('skipped: ')[1] for r in report if 'skipped' in r]
        self.assertEqual(reasons[0], reasons[1]); self.assertIn('relocated in this build', reasons[0])
        self.assertEqual(got['Gated restore'][2], [0x80000040, 0x04000000, 0x144, BL(0x144, 0x400), 0x20000000])

    def test_a_word_without_address_bits_may_replace_a_moved_one(self):
        """A NOP over a moved bl and a mov over a moved adrp user carry no address bits: written, guarded by this build's word."""
        fa = lambda s, callee, data: [MOVZ(1, 1), BL(s + 4, callee), ADRP(8, s + 8, data), LDRW(0, 8, data & 0xfff), MOVZ(3, 3), RET]
        callee = [MOVZ(0, 7), MOVZ(5, 9), RET]
        old = self.build('a', {0x100: fa(0x100, 0x300, 0x4100), 0x300: callee})
        new = self.build('b', {0x140: fa(0x140, 0x400, 0x4208), 0x400: callee}, build=B2)
        got, report, skipped = self.roundtrip(old, new, f'[Nop bl]\n04000000 00000104 {NOP:08X}\n[Mov]\n04000000 0000010C {MOVZ(0, 1):08X}\n')
        self.assertEqual(skipped, 0, report)
        self.assertEqual(got['Nop bl'][2], [0x14050000, 0x144, BL(0x144, 0x400), 0x04000000, 0x144, NOP, 0x20000000])
        self.assertEqual(got['Mov'][2], [0x14050000, 0x14c, LDRW(0, 8, 0x208), 0x04000000, 0x14c, MOVZ(0, 1), 0x20000000])

    def test_carries_address_bits_is_false_only_for_address_free_encodings(self):
        """The classifier: NOP, MOVZ, register-only data processing, RET and FMOV carry no address; everything else, unknown included, does."""
        free = {'nop': NOP, 'movz': MOVZ(0, 1), 'movk x': 0xf2a00020, 'mov x0, x1': 0xaa0103e0, 'neg': 0xcb0103e0,
                'add reg': 0x8b020020, 'csel': 0x9a821020, 'mul': 0x9b027c20, 'udiv': 0x9ac20820, 'ret': RET,
                'fmov s0, s1': 0x1e204020, 'fmov s0, #1.0': 0x1e2e1000}
        bound = {'bl': BL(0, 0x100), 'b.ne': BCOND(1, 0, 8), 'cbz': 0x34000040, 'tbz': 0x36000040, 'adr': 0x10000040,
                 'adrp': ADRP(8, 0, 0x4000), 'ldr literal': 0x58000040, 'ldr imm': LDRX(0, 8, 8), 'add imm': ADDI(8, 8, 0x10),
                 'str imm': 0xf9000100, 'udf': 0, 'move wide opc 01': 0x32800000}
        for name, w in free.items():
            with self.subTest(name): self.assertFalse(ca.carries_address_bits(w))
        for name, w in bound.items():
            with self.subTest(name): self.assertTrue(ca.carries_address_bits(w))

    def test_a_flipped_conditional_branch_follows_its_function(self):
        """b.ne flipped to b.eq inside a function that moved: kept as written and checked to be found by fingerprint, refused by window."""
        def f(s, head=()):
            """Four distinct words, a b.ne to the function's ret, four more distinct words, ret."""
            n = len(head)
            return [*head, *(MOVZ(k, k) for k in range(1, 5)), BCOND(1, s + 4 * (n + 4), s + 4 * (n + 9)), *(MOVZ(k, k) for k in range(5, 9)), RET]
        old = self.build('a', {0x100: f(0x100)})
        moved = self.build('b', {0x300: f(0x300)}, build=B2)
        flip = BCOND(0, 0x110, 0x124)
        got, report, skipped = self.roundtrip(old, moved, f'[Flip]\n04000000 00000110 {flip:08X}\n')
        self.assertEqual(skipped, 0, report)
        self.assertEqual(got['Flip'][2], [0x14050000, 0x310, BCOND(1, 0x310, 0x324), 0x04000000, 0x310, flip, 0x20000000])
        longer = self.build('c', {0x300: f(0x300, (NOP,))}, build=b'\x33' * 8)   # resized: found by its window only
        _, report, skipped = self.roundtrip(old, longer, f'[Flip]\n04000000 00000110 {flip:08X}\n')
        self.assertEqual(skipped, 1); self.assertIn('found here by window', report[0])

    # ---- segments, malformed files, the rest ----

    def test_a_data_anchor_stays_in_its_target_segment(self):
        """A target at the start of .data is never anchored to a nearer .rodata base, and an old source that crosses is refused."""
        ro = {0x100: reader(0x100, 0x3ff8), 0x200: reader(0x200, 0x3ff8, 0x20)}   # an object at the end of .rodata
        old = self.build('a', ro)
        src, report, bound = ca.import_file(self.txt('[x]\n02000000 00004010 00000001\n'), old)
        self.assertEqual(bound, 1); self.assertEqual(src['cheats'][0]['builds'], ['1111111111111111'])
        self.assertIn('no describable code reference', ' '.join(report))
        # with an object at the start of .data: anchored to it, and found again when .rodata grows and .data moves up
        # (by a page: Horizon maps each segment on a page boundary, and nso.load refuses one that is not)
        old = self.build('c', {**ro, 0x300: reader(0x300, 0x4000, 0x22), 0x400: reader(0x400, 0x4000, 0x24)})
        grown = self.build('d', {**ro, 0x300: reader(0x300, 0x5000, 0x22), 0x400: reader(0x400, 0x5000, 0x24)}, build=B2, data=0x5000)
        got, report, skipped = self.roundtrip(old, grown, '[x]\n02000000 00004010 00000001\n')
        self.assertEqual(skipped, 0, report); self.assertEqual(got['x'][2][1], 0x5010)
        # a source that anchors the .data target on the .rodata object (what import used to write) is refused
        refs = ', '.join(f'{{ old = "{old.word(s):08X}", size = 0x10, at = 0x0, use = 0x4, fp = "{old.digest(s, "fp")}" }}' for s in (0x100, 0x200))
        self.source(f'format = 1\n[[cheats]]\nname = "x"\ntarget = "d_3FF8+0x18"\ntype = "u16"\nvalue = 1\n[anchors.d_3FF8]\ndata = [{refs}]\n')
        _, _, report, skipped = ca.compile_source(self.dir / 's.cheats.toml', old)
        self.assertEqual(skipped, 1); self.assertIn('crosses from ro into data', report[0])

    def test_glued_words_are_split_and_stray_tokens_make_a_cheat_malformed(self):
        """Glued words split like Atmosphère reads them; prose lines are ignored like the host does; junk on a code
        line, or a cut word, keeps the cheat whole, bound and refused, with its line; such a token survives the TOML."""
        entries, warns = ca.parse_atmosphere('[A]\n0400000000004100 00000001\n')
        self.assertEqual(entries, [('A', False, [0x04000000, 0x4100, 1], [])])
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        text = ('[B]\n80000001\n04000000 00004100 00000001\n2000000\n[C]\nby someone\n04000000 00004104 00000002\n'
                '[Credits]\nMade by X\n[D]\n04000000 00004108 00000003 x""""y\n')
        src, report, bound = ca.import_file(self.txt(text), old)
        self.assertEqual(bound, 2)
        b, cr, d = (next(c for c in src['cheats'] if c['name'] == n) for n in ('B', 'Credits', 'D'))
        self.assertIn('2000000', b['code']); self.assertIn('x""""y', d['code']); self.assertTrue(cr.get('label'))   # never shortened
        self.assertTrue(any('"B"' in r and 'line 4: "2000000"' in r for r in report), report)
        self.assertTrue(any('"by" in "C"' in r and 'ignored' in r for r in report), report)
        ca.write_toml(src, self.dir / 'g.cheats.toml', 'cheats.txt')
        self.assertIn('x""""y', tomllib.loads((self.dir / 'g.cheats.toml').read_text())['cheats'][3]['code'])
        _, entries, report, skipped = ca.compile_source(self.dir / 'g.cheats.toml', old)   # refused even on its own build
        self.assertEqual(skipped, 2); self.assertEqual([e[0] for e in entries if e[2] is not None], ['C', 'Credits'])

    def test_an_undecodable_cheat_is_kept_verbatim_on_its_own_build(self):
        """A cheat the decoder cannot read is emitted word for word on its own build, and skipped on any other."""
        old = self.build('a', {0x100: reader(0x100, 0x4100)})
        new = self.build('b', {0x100: reader(0x100, 0x4100)}, build=B2)
        got, report, skipped = self.roundtrip(old, old, '[Weird]\nB0000000 00000001\n')
        self.assertEqual(skipped, 0, report); self.assertEqual(got['Weird'][2], [0xB0000000, 1])
        self.assertIn('[Weird]\nB0000000 00000001\n', ca.render(list(got.values())))
        _, entries, report, skipped = ca.compile_source(self.dir / 'g.cheats.toml', new)
        self.assertEqual(skipped, 1)

    def test_a_file_named_after_the_title_id_names_no_build(self):
        """A file named after the title ID is taken as written for the NSO's build, not refused as another build's."""
        (self.dir / 't').mkdir()
        npdm = bytearray(0xa0); struct.pack_into('<I', npdm, 0x70, 0x80); npdm[0x80:0x84] = b'ACI0'
        struct.pack_into('<Q', npdm, 0x90, 0x0100abcd00000000); (self.dir / 't/main.npdm').write_bytes(npdm)
        old = self.build('t/main', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        self.assertEqual(old.title, '0100ABCD00000000')
        src, report, bound = ca.import_file(self.txt('[h]\n02100000 00001234 00000001\n', '0100ABCD00000000.txt'), old)
        self.assertEqual(bound, 1); self.assertEqual(src['cheats'][0]['builds'], ['1111111111111111'])

    def test_a_merged_pair_keeps_its_master_flag(self):
        """A master on or off entry stays a master after its on / off pair is merged."""
        old = self.build('a', {0x100: [0xaa1703e0, NOP, LDRH(0, 0, 0x34), 0x1e2e1000, RET]})
        for off_head, off_master in (('[Fast - off]', False), ('{Fast - off}', True)):
            with self.subTest(off_head):
                src, _, _ = ca.import_file(self.txt(f'{{Fast - on}}\n04000000 0000010C 1E2F1000\n{off_head}\n04000000 0000010C 1E2E1000\n'), old)
                self.assertEqual([c.get('on_name') for c in src['cheats']], ['Fast - on'])   # merged into one cheat
                ca.write_toml(src, self.dir / 'g.cheats.toml', 'cheats.txt')
                _, entries, report, skipped = ca.compile_source(self.dir / 'g.cheats.toml', old)
                self.assertEqual([(e[0], e[1]) for e in entries], [('Fast - on', True), ('Fast - off', off_master)])

    def test_only_opcode_0_literals_are_carried_into_the_code(self):
        """A register store (opcode A) into the code is bound to its build: its value never meets the relocation rule."""
        fa = lambda s, callee: [MOVZ(1, 1), BL(s + 4, callee), MOVZ(2, 2), MOVZ(3, 3), RET]
        old = self.build('a', {0x100: fa(0x100, 0x300), 0x300: [MOVZ(0, 7), RET]})
        src, report, bound = ca.import_file(self.txt(f'[RegStore]\n400F0000 00000000 {BL(0x104, 0x308):08X}\nA4F00400 00000104\n'), old)
        self.assertEqual(bound, 1); self.assertIn('a store of a register into the code at 0x104', ' '.join(report))

    def test_a_new_adrp_user_over_an_unmoved_word_is_bound(self):
        """A new word that takes a preceding adrp's page keeps this build's low page bits: bound at import, refused at compile elsewhere."""
        f = lambda s, data: [ADRP(8, s, data), LDRW(1, 8, data & 0xfff), MOVZ(0, 2), MOVZ(3, 3), RET]
        old = self.build('a', {0x100: f(0x100, 0x4100)})
        new = self.build('b', {0x100: f(0x100, 0x4180)}, build=B2)   # the data moved inside its page, the code did not
        text = f'[Neighbour]\n04000000 00000108 {LDRW(0, 8, 0x104):08X}\n'
        src, report, bound = ca.import_file(self.txt(text), old)
        self.assertEqual(bound, 1); self.assertIn('takes the page of the adrp at 0x100', ' '.join(report))
        self.source(f'format = 1\nauthored = "1111111111111111"\n[[cheats]]\nname = "N"\npatch = [{{ at = "s_108", new = "{LDRW(0, 8, 0x104):08X}" }}]\n'
                    f'[anchors.s_108]\ncode = {{ old = "{MOVZ(0, 2):08X}", size = 0x14, at = 0x8, fp = "{old.digest(0x100, "fp")}" }}\n')
        _, entries, report, skipped = ca.compile_source(self.dir / 's.cheats.toml', old)
        self.assertEqual(skipped, 0, report)   # its own build: the word is right there
        _, entries, report, skipped = ca.compile_source(self.dir / 's.cheats.toml', new)
        self.assertEqual(skipped, 1); self.assertIn('takes the page of the adrp', report[0])

    def test_a_narrow_write_is_checked_by_the_word_it_makes(self):
        """A 2-byte write that retargets a bl is bound at import, and refused at compile when the site was found by its window."""
        F = lambda s, g: [MOVZ(1, 1), BL(s + 4, g), MOVZ(2, 2), MOVZ(3, 3), RET]
        old = self.build('a', {0x100: F(0x100, 0x400), 0x400: [MOVZ(0, 7), RET], 0x500: [MOVZ(0, 8), MOVZ(6, 9), RET]})
        src, report, bound = ca.import_file(self.txt(f'[ToH]\n02000000 00000104 0000{BL(0x104, 0x500) & 0xffff:04X}\n'), old)
        self.assertEqual(bound, 1); self.assertIn('makes 940000FF, PC-relative to 0x500', ' '.join(report))
        self.source(f'format = 1\n[[cheats]]\nname = "N"\npatch = [{{ at = "s_104", width = 2, new = "{BL(0x104, 0x500) & 0xffff:04X}" }}]\n'
                    f'[anchors.s_104]\ncode = {{ old = "{BL(0x104, 0x400):08X}", size = 0x14, at = 0x4, fp = "{old.digest(0x100, "fp")}" }}\n')
        _, entries, report, skipped = ca.compile_source(self.dir / 's.cheats.toml', old)
        self.assertEqual(skipped, 1); self.assertIn('PC-relative to 0x500', report[0])

    def test_a_compare_with_a_moved_original_compares_with_this_builds_word(self):
        """An "if [X] == original" gate over a moved bl compares with this build's word, so it still opens."""
        fa = lambda s, callee: [MOVZ(1, 1), BL(s + 4, callee), MOVZ(2, 2), MOVZ(3, 3), RET]
        old = self.build('a', {0x100: fa(0x100, 0x300), 0x300: [MOVZ(0, 7), RET]})
        new = self.build('b', {0x140: fa(0x140, 0x400), 0x400: [MOVZ(0, 7), RET]}, build=B2)
        text = f'[Gate]\n14050000 00000104 {BL(0x104, 0x300):08X}\n04000000 00000104 {NOP:08X}\n20000000\n'
        got, report, skipped = self.roundtrip(old, new, text)
        self.assertEqual(skipped, 0, report)
        self.assertEqual(got['Gate'][2], [0x14050000, 0x144, BL(0x144, 0x400), 0x04000000, 0x144, NOP, 0x20000000])

    def test_a_store_offset_by_r0_the_cheat_sets_is_indexed(self):
        """The VM adds the offset register, r0 included: a store into code offset by an r0 the cheat loads is not static."""
        old = self.build('a', {0x100: [0xaa1703e0, NOP, LDRH(0, 0, 0x34), 0x1e2e1000, RET]})
        src, report, bound = ca.import_file(self.txt('[R0]\n40000000 00000000 00000004\n04000000 0000010C 1E2F1000\n'), old)
        self.assertEqual(bound, 1); self.assertIn('indexed by register 0', ' '.join(report))

    def test_a_data_anchor_found_among_identical_copies_by_order_is_weak(self):
        """Data references resolved among identical copies by order alone keep the weak mark."""
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x300: reader(0x300, 0x4100)})
        new = self.build('b', {0x500: reader(0x500, 0x4180), 0x900: reader(0x900, 0x4180)}, build=B2)
        got, report, skipped = self.roundtrip(old, new, '[x]\n02000000 0000411E 00000001\n')
        self.assertEqual(skipped, 0, report); self.assertEqual(got['x'][2][1], 0x419e)
        self.assertIn('weak: 2 of them identical copies by order', got['x'][3]['how'])
        self.assertTrue(any('resolved weakly' in r for r in report))

    def test_a_multi_word_patch_with_a_rewritten_word_is_skipped_whole(self):
        """A patch whose guard would skip one rewritten word and apply the other is refused instead of half-written."""
        f = [0xaa1703e0, NOP, LDRH(0, 0, 0x34), 0x1e2e1000, RET]
        old = self.build('a', {0x100: f})
        new = self.build('b/extract/v1/ExeFS/main', {0x200: f}, build=B2)
        rewritten = bytearray(new.img); struct.pack_into('<I', rewritten, 0x20c, 0x14000000)
        (self.dir / 'b/patched_v1').mkdir(); (self.dir / 'b/patched_v1/main.patched.bin').write_bytes(rewritten)
        new = ca.Build(self.dir / 'b/extract/v1/ExeFS/main')
        _, report, skipped = self.roundtrip(old, new, '[Two]\n04000000 00000108 D2800020\n04000000 0000010C 1E2F1000\n')
        self.assertEqual(skipped, 1); self.assertIn('partial hook', report[0])
        self.assertTrue(any('the rewriter changed the word at 0x20C' in r for r in report), report)

    # ---- the source and the outputs ----

    def test_data_actions_and_values(self):
        """The encodings of min, set with "ask", a pointer chain (register 15 zeroed after it), decimal offsets and the refusal of signed min / max."""
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        refs = ', '.join(f'{{ old = "{old.word(s):08X}", size = 0x10, at = 0x0, use = 0x4, fp = "{old.digest(s, "fp")}" }}' for s in (0x100, 0x200))
        self.source(f'''format = 1
[[cheats]]
name = "HP at least 50"
target = "d_4100+0x1C"
type = "u16"
action = "min"
value = 50
[[cheats]]
name = "Gold"
section = "Items"
target = "d_4100+0x40"
type = "u32"
action = "set"
value = "ask"
min = 0
max = 9999
default = 500
[[cheats]]
name = "Mog"
target = "[d_4100+0x8]+0x38"
type = "u32"
value = 0
[[cheats]]
name = "Signed"
target = "d_4100"
type = "s16"
action = "max"
value = 10
[[cheats]]
name = "Leading zero"
target = "d_4100+010"
type = "u8"
value = 1
[[cheats]]
name = "HP never decreases"
target = "d_4100+0x1C"
type = "s16"
action = "never-decrease"
[anchors.d_4100]
data = [{refs}]
''')
        _, entries, report, skipped = ca.compile_source(self.dir / 's.cheats.toml', old)
        got = {e[0]: e for e in entries}
        self.assertEqual(got['HP at least 50'][2], [0x12030000, 0x411c, 50, 0x02000000, 0x411c, 50, 0x20000000])
        self.assertEqual(got['Gold'][2], [0x04000000, 0x4140, 500]); self.assertEqual(got['Gold'][3]['ask'], '0..9999')
        self.assertEqual(got['Gold'][3]['once'], '1'); self.assertEqual(got['Gold'][3]['value_words'], '2')
        self.assertEqual(got['Mog'][2], [0x18060000, 0x4108, 0, 0, 0x580f0000, 0x4108, 0x780f0000, 0x38,
                                         0x640f0000, 0, 0, 0x400f0000, 0, 0, 0x20000000])   # r15 is zeroed after the chain
        self.assertEqual(got['Leading zero'][2][1], 0x410a)   # decimal 010 = 10
        nd = got['HP never decreases']   # no Atmosphère program: the host runs it from the meta row
        self.assertEqual((nd[2], nd[3]['action'], nd[3]['address'], nd[3]['type']), ([], 'never-decrease', '0:0x411C', 's16'))
        self.assertIn('nxnative only', nd[3]['how'])
        self.assertEqual(skipped, 1); self.assertIn('only unsigned types', [r for r in report if 'Signed' in r][0])
        text = ca.render(entries)
        self.assertIn('[--SectionStart:Items--]\n\n[Gold]\n04000000 00004140 000001F4\n\n[--SectionEnd:Items--]', text)
        self.assertIn('[HP never decreases]\n\n', text)   # an empty entry: an inert label on other engines
        meta = ca.meta_text('s.cheats.toml', old, entries, {}, text).split('\n')
        head = meta[2].split('\t'); row = dict(zip(head, next(l for l in meta if l.startswith('HP never')).split('\t')))
        self.assertEqual((row['action'], row['address'], row['type']), ('never-decrease', '0:0x411C', 's16'))

    def test_instruction_lengths_of_every_form(self):
        """Instruction lengths of the forms the host VM does not run yet: a wrong length would misread every later word of a community file."""
        cases = [([0x94100020], 1), ([0x94100100, 0], 2), ([0x98100100, 0, 0], 3),                  # 9: register / static value
                 ([0xA4100000], 1), ([0xA4100200, 0], 2), ([0xA4100400, 0x1234], 2),                # A: none / fixed / region + address
                 # C0TcSX..: X (bits 8-11) 0 region + address, 1 region + register, 4 a static value, 5 another register
                 ([0xC0450000, 0], 2), ([0xC0450100], 1), ([0xC0450400, 0], 2), ([0xC0850400, 0, 0], 3), ([0xC0450500], 1),
                 ([0xC1000000], 1), ([0xC2000000], 1), ([0xC3000000], 1), ([0xC4000000, 0, 0], 3),
                 ([0xFF000000], 1), ([0xFFF40000, 0], 2), ([0xFFF40400], 1)]
        for words, n in cases:
            with self.subTest(word=f'{words[0]:08X}'): self.assertEqual(ca.decode(words, 0)['n'], n)
        self.assertEqual(ca.decode([0xA4100400, 0x1234], 0)['rel'], 0x1234)
        with self.assertRaisesRegex(ca.AnchorError, 'unknown opcode'): ca.decode([0xB0000000], 0)

    def test_names_with_control_characters_survive_the_source(self):
        """Names with control characters, quotes and backslashes survive the TOML source."""
        src = {'format': 1, 'authored': '1111111111111111', 'cheats': [{'name': 'a\x01b"c\\d\x7f', 'label': True}], 'anchors': {}}
        ca.write_toml(src, self.dir / 'x.cheats.toml', 'x.txt')
        self.assertEqual(tomllib.loads((self.dir / 'x.cheats.toml').read_text())['cheats'][0]['name'], 'a\x01b"c\\d\x7f')

    def test_an_output_this_tool_did_not_write_is_never_overwritten(self):
        """Hand-made, edited or another source's .txt files are kept; only this source's unchanged output is rewritten."""
        old = self.build('a', {0x100: reader(0x100, 0x4100), 0x200: reader(0x200, 0x4100, 0x20)})
        src, _, _ = ca.import_file(self.txt('[x]\n02000000 0000411E 00000001\n'), old)
        ca.write_toml(src, self.dir / 'g.cheats.toml', 'cheats.txt')
        out = self.dir / 'cheats'; out.mkdir()
        txt = out / '1111111111111111.txt'
        txt.write_text('[mine]\n')
        src, entries, _, _ = ca.compile_source(self.dir / 'g.cheats.toml', old)
        compile = lambda path, replace=False: ca.write_compiled(out, path, old, entries, src, replace)
        compile(self.dir / 'g.cheats.toml')
        self.assertEqual(txt.read_text(), '[mine]\n'); self.assertTrue((out / '1111111111111111.txt.new').read_text().startswith('[x]'))
        compile(self.dir / 'g.cheats.toml', replace=True)
        self.assertEqual((out / '1111111111111111.txt.orig').read_text(), '[mine]\n'); self.assertTrue(txt.read_text().startswith('[x]'))
        compile(self.dir / 'g.cheats.toml')   # its own unchanged output: rewritten in place
        self.assertFalse((out / '1111111111111111.txt.new').read_text() != (out / '1111111111111111.txt').read_text())
        (out / '1111111111111111.txt.new').unlink()
        compile(self.dir / 'other.cheats.toml')   # another source's output for the same build
        self.assertTrue((out / '1111111111111111.txt.new').exists()); (out / '1111111111111111.txt.new').unlink()
        txt.write_text(txt.read_text() + '[edited]\n')   # edited since it was compiled
        lines = compile(self.dir / 'g.cheats.toml')
        self.assertIn('edited since it was compiled', lines[0]); self.assertTrue(txt.read_text().endswith('[edited]\n'))


if __name__ == '__main__': unittest.main()
