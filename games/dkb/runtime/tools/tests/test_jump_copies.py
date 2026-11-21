"""Exercise offset jump-table recognition on real AArch64 instructions and synthetic modules, without game files."""
import ast
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import ADRP, ADDI, ADDR, B, BEQ, BLR, BR, NOP

# patch_nso.py is a command-line program whose top level loads and rewrites an image.
# Compile its actual recognition functions without executing that driver.
tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
IMG = bytearray(0x5000); tabjumps = {}; jumps = {}   # the globals the recognizers read: the module image and the jump tables resolved so far in the function
scope = {'re': re, 'struct': struct, 'ARM64_OP_IMM': ARM64_OP_IMM, 'IMG': IMG, 'tabjumps': tabjumps, 'jumps': jumps}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('num', 'written_w', 'writes_reg', 'writes_reg_any', 'half_count', 'copied_base', 'table_base', 'table_cands', 'forward_table_base', 'check_before_copy', 'jump_offsets', 'jump_adr_offsets', 'jump_zero_extended', 'jump_masked_pred', 'jump_masked_not')], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)


def mov(dst, src, wide=True): return (0xaa0003e0 if wide else 0x2a0003e0) | src << 16 | dst
def ldr_s(reg): return 0xbd400000 | 2 << 10 | 31 << 5 | reg   # ldr sN, [sp, #8]


class CopiedBase(unittest.TestCase):
    def follow(self, words, reg='x7'):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(ins), len(words))
        return scope['copied_base'](ins, len(ins), reg)

    def base(self, reg=7): return [ADRP(reg, 0, 0x4000), ADDI(reg, reg, 0x40c)]

    def test_far_copy_through_lr(self):
        self.assertEqual(self.follow(self.base() + [mov(30, 7)] + [NOP] * 688 + [mov(7, 30)]), 0x440c)

    def test_32_bit_writers_destroy_base(self):
        for clobber in (mov(7, 0, False), 0x52800027, ADDI(7, 7, 1) & 0x7fffffff):
            with self.subTest(clobber=hex(clobber)):
                self.assertIsNone(self.follow(self.base() + [clobber, mov(30, 7), mov(7, 30)]))
        self.assertIsNone(self.follow(self.base() + [mov(30, 7), mov(30, 0, False), mov(7, 30)]))

    def test_calls_clobber_volatile_and_link_registers(self):
        for call in (0x94000000, BLR(0)):
            self.assertIsNone(self.follow(self.base() + [call]))
            self.assertIsNone(self.follow(self.base() + [mov(30, 7), call, mov(7, 30)]))
            self.assertEqual(self.follow(self.base(19) + [call, mov(7, 19)]), 0x440c)

    def base_at_load(self, words, reg='x7'):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(ins), len(words))
        return scope['table_base'](ins, len(ins), reg)

    def test_table_base_sees_every_write_of_the_register(self):
        self.assertEqual(self.base_at_load(self.base()), 0x440c)
        self.assertIsNone(self.base_at_load(self.base() + [mov(7, 0, False)]))   # mov w7,w0 replaces the base too
        self.assertEqual(self.base_at_load(self.base(30), 'x30'), 0x440c)   # capstone names the writes lr
        self.assertEqual(self.base_at_load(self.base() + [0x710004ff]), 0x440c)   # cmp w7,#1 only sets the flags
        self.assertEqual(self.base_at_load(self.base() + [0xf10004ff]), 0x440c)   # cmp x7,#1

    def test_aliases_and_unrelated_writes(self):
        self.assertEqual(self.follow(self.base(29) + [mov(7, 29)]), 0x440c)
        self.assertEqual(self.follow(self.base(30), 'lr'), 0x440c)
        self.assertEqual(self.follow(self.base() + [mov(8, 0, False)]), 0x440c)
        self.assertIsNone(self.follow(self.base() + [mov(7, 0)]))


RET = 0xd65f03c0
AND_X6_X17_7FF = 0x92400000 | 10 << 10 | 17 << 5 | 6      # and x6, x17, #0x7ff
AND_X6_X17_FF = 0x92400000 | 7 << 10 | 17 << 5 | 6        # and x6, x17, #0xff
AND_W18_W17_7FF = 0x12000000 | 10 << 10 | 17 << 5 | 18    # and w18, w17, #0x7ff
UBFX_W18_W17 = 0x53000000 | 11 << 16 | 12 << 10 | 17 << 5 | 18   # ubfx w18, w17, #0xb, #2
CMP_W18_3 = 0x7100001f | 3 << 10 | 18 << 5                # cmp w18, #3
CMP_W18_1 = 0x7100001f | 1 << 10 | 18 << 5                # cmp w18, #1
LDRSW_X18 = 0xb8a07800 | 6 << 16 | 27 << 5 | 18           # ldrsw x18, [x27, x6, lsl #2]


def cbz_w18(frm, to): return 0x34000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | 18
def bhi(frm, to): return 0x54000008 | (((to - frm) >> 2) & 0x7ffff) << 5


class AdrOffsetBounds(unittest.TestCase):
    """ADR-relative dispatches can compare the full 64-bit table index."""
    TABLE = 0x4040
    TARGETS = [0, 8, 36, 40, 44]

    def run_site(self, wide=True, index=9, clobber=NOP, loaded=9, after_adr=NOP):
        # adrp/add x15; cmp x9,#4; b.hi; adr x18,.; ldrsw x0,[x15,x9,lsl #2]; add x18,x18,x0; br x18.
        # The compare and ADR bases are independent, as in DRAGON QUEST I & II's copy dispatch.
        words = [ADRP(15, 0, self.TABLE), ADDI(15, 15, self.TABLE & 0xfff),
                 (0xf100001f if wide else 0x7100001f) | 4 << 10 | index << 5, bhi(12, 44), clobber,
                 0x10000012, after_adr, 0xb8a07800 | loaded << 16 | 15 << 5, ADDR(18, 18, 0), BR(18), RET, RET, RET]
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, target - 20)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(ins), len(words))
        return scope['jump_adr_offsets'](ins, 9)

    def test_a_32_bit_write_after_the_adr_replaces_its_value(self):
        self.assertIsNone(self.run_site(after_adr=mov(18, 0, False)))

    def test_64_bit_comparison_bounds_the_table(self):
        self.assertEqual(self.run_site(), self.TARGETS)

    def test_32_bit_comparison_keeps_its_existing_targets(self):
        self.assertEqual(self.run_site(wide=False), self.TARGETS)

    def test_unrelated_checks_and_changed_indices_are_refused(self):
        self.assertIsNone(self.run_site(index=8))
        for wide in (False, True):
            with self.subTest(wide=wide):
                self.assertIsNone(self.run_site(clobber=mov(9, 0, wide)))

    def test_a_copy_must_be_bounded_by_its_source(self):
        self.assertEqual(self.run_site(index=0, clobber=mov(9, 0, False)), self.TARGETS)

    def test_a_vector_load_does_not_change_the_index(self):
        # ldr s9,[sp,#8] between the check and the load, as Tomodachi Life: Living the Dream 1.0.0 main+0x3ce58 loads s0 with the index in w0
        self.assertEqual(self.run_site(clobber=ldr_s(9)), self.TARGETS)

    def test_writes_of_x29_and_x30_change_the_index(self):
        # Capstone's reg_name calls them fp and lr
        for reg in (29, 30):
            with self.subTest(reg=reg):
                self.assertEqual(self.run_site(index=reg, loaded=reg), self.TARGETS)
                self.assertIsNone(self.run_site(index=reg, loaded=reg, clobber=mov(reg, 0)))


class HalfCount(unittest.TestCase):
    """The pads a halfword table needs: the check's bound, or None when the index may have changed after it."""
    TABLE = 0x4100

    def run_site(self, index=9, clobber=NOP, loaded=None):
        loaded = index if loaded is None else loaded
        # adrp/add x15; cmp w9,#4; b.hi; <clobber>; ldrh w0,[x15,x9,lsl #1]; adr x18,.; add x18,x18,x0,lsl #2; br x18
        words = [ADRP(15, 0, self.TABLE), ADDI(15, 15, self.TABLE & 0xfff), 0x7100001f | 4 << 10 | index << 5, bhi(12, 36), clobber,
                 0x78607800 | loaded << 16 | 15 << 5, 0x10000012, 0x8b000a52, BR(18), RET]
        struct.pack_into('<5H', IMG, self.TABLE, 0, 1, 2, 3, 4)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(ins), len(words))
        return scope['half_count'](ins, ins[5], 0)

    def test_bound_from_the_check(self):
        self.assertEqual(self.run_site(), 5)

    def test_a_vector_load_does_not_change_the_index(self):
        self.assertEqual(self.run_site(clobber=ldr_s(9)), 5)

    def test_general_purpose_writes_change_the_index(self):
        for reg in (9, 29, 30):
            with self.subTest(reg=reg):
                self.assertEqual(self.run_site(index=reg), 5)
                self.assertIsNone(self.run_site(index=reg, clobber=mov(reg, 0)))

    def test_a_copy_replaces_the_index(self):
        self.assertIsNone(self.run_site(clobber=mov(9, 0, False)))   # cmp w9; mov w9,w0: the check says nothing of w0
        self.assertEqual(self.run_site(index=0, loaded=9, clobber=mov(9, 0, False)), 5)   # cmp w0; mov w9,w0


class CopyAfterCheck(unittest.TestCase):
    TABLE = 0x4040
    TARGETS = [32 + i * 4 for i in range(5)]

    def run_site(self, checked):
        # adrp/add x4; cmp wN,#4; b.hi; mov w18,w21; ldrsw x18,[x4,x18,lsl #2]; add x18,x18,x4; br x18 (Link's Awakening's movie module checks w21)
        words = [ADRP(4, 0, self.TABLE), ADDI(4, 4, self.TABLE & 0xfff), 0x7100001f | 4 << 10 | checked << 5, bhi(12, 32),
                 mov(18, 21, False), 0xb8a07800 | 18 << 16 | 4 << 5 | 18, ADDR(18, 18, 4), BR(18)] + [RET] * 5
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, target - self.TABLE)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        return scope['jump_offsets'](ins, 7)

    def test_the_check_must_be_of_the_source(self):
        self.assertEqual(self.run_site(21), self.TARGETS)
        self.assertIsNone(self.run_site(18))


class IndexChangedAfterCheck(unittest.TestCase):
    TABLE = 0x4040
    TARGETS = [32 + i * 4 for i in range(5)]

    def run_site(self, between, index=18):
        # adrp/add x4; cmp wN,#4; b.hi; <between>; ldrsw x18,[x4,xN,lsl #2]; add x18,x18,x4; br x18
        words = [ADRP(4, 0, self.TABLE), ADDI(4, 4, self.TABLE & 0xfff), 0x7100001f | 4 << 10 | index << 5, bhi(12, 32),
                 between, 0xb8a07800 | index << 16 | 4 << 5 | 18, ADDR(18, 18, 4), BR(18)] + [RET] * 5
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, target - self.TABLE)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        return scope['jump_offsets'](ins, 7)

    def test_unrelated_writes_keep_the_site(self):
        for word in (NOP, ldr_s(18), mov(9, 0), 0xeb00025f):   # cmp x18,x0 only sets the flags
            with self.subTest(word=hex(word)):
                self.assertEqual(self.run_site(word), self.TARGETS)

    def test_a_write_of_the_index_is_refused(self):
        for word in (ADDI(18, 18, 1) & 0x7fffffff, ADDI(18, 18, 1), mov(18, 0)):   # add w18,w18,#1; add x18,x18,#1; mov x18,x0
            with self.subTest(word=hex(word)):
                self.assertIsNone(self.run_site(word))

    def test_a_call_changes_only_a_caller_saved_index(self):
        self.assertIsNone(self.run_site(0x94000040))
        self.assertEqual(self.run_site(0x94000040, index=19), self.TARGETS)

    def test_a_write_before_the_check_does_not_count(self):
        # ldr w21,[sp,#8]; mov w18,w21; cmp w21,#4; b.hi; ldrsw x18,[x4,x18,lsl #2]: the walk passes the check first, and check_before_copy takes it
        words = [ADRP(4, 0, self.TABLE), ADDI(4, 4, self.TABLE & 0xfff), 0xb9400000 | 2 << 10 | 31 << 5 | 21, mov(18, 21, False), 0x7100001f | 4 << 10 | 21 << 5, bhi(20, 32),
                 0xb8a07800 | 18 << 16 | 4 << 5 | 18, ADDR(18, 18, 4), BR(18)] + [RET] * 5
        targets = [36 + i * 4 for i in range(5)]
        for i, target in enumerate(targets): struct.pack_into('<i', IMG, self.TABLE + 4 * i, target - self.TABLE)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(scope['jump_offsets'](ins, 8, copy_first=True), targets)


class ForwardBase(unittest.TestCase):
    """Entry jumps over the dispatch to its sole base initialization, then branches backward."""
    TABLE = 0x4040
    TARGETS = [24, 28, 44]

    def setUp(self):
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, target - self.TABLE)
        struct.pack_into('<i', IMG, self.TABLE + 12, 0x1000)   # not a fourth entry
        tabjumps.clear(); jumps.clear()

    def words(self, **over):
        # cmp w18,#2; b.hi; ldrsw x18,[x17,x18,lsl #2]; add x18,x18,x17; br x18.
        words = [B(0, 32), 0x71000a5f, bhi(8, 28), 0xb8b27a32, ADDR(18, 18, 17), BR(18), NOP, RET,
                 ADRP(17, 32, self.TABLE), ADDI(17, 17, self.TABLE & 0xfff), B(40, 4), NOP, RET]
        for k, v in over.items(): words[int(k[1:])] = v
        return words

    def run_site(self, words, forward=True):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]
        j = next(i for i, p in enumerate(ins) if p.address == 20)
        return scope['jump_offsets'](ins, j, copy_first=True, far_base=True, forward_base=forward)

    def test_later_initialization_and_exact_extent(self):
        self.assertIsNone(self.run_site(self.words(), forward=False))
        self.assertEqual(self.run_site(self.words()), self.TARGETS)

    def test_base_writers_must_be_unique_full_width_and_adjacent(self):
        for slot, word in ((8, NOP), (9, mov(17, 0, False)), (9, ADDI(17, 17, 0x40) & 0x7fffffff),
                           (11, mov(17, 0)), (11, 0x94000000), (11, BLR(0))):
            with self.subTest(slot=slot, word=hex(word)):
                self.assertIsNone(self.run_site(self.words(**{f'w{slot}': word})))
        words = self.words(); words.insert(9, NOP)
        self.assertIsNone(self.run_site(words))

    def test_no_entry_may_bypass_the_page_initialization(self):
        for target in (8, 12, 16, 20, 36):
            self.assertIsNone(self.run_site(self.words(w11=B(44, target))))
        tabjumps[0x1000] = [36]
        self.assertIsNone(self.run_site(self.words()))
        tabjumps.clear(); jumps[0x1000] = (32, 2)
        self.assertIsNone(self.run_site(self.words()))
        jumps.clear()
        struct.pack_into('<i', IMG, self.TABLE, 36 - self.TABLE)
        self.assertIsNone(self.run_site(self.words()))

    def test_guard_must_bound_the_loaded_index(self):
        for word in (BEQ(8, 28), bhi(8, 12), NOP):
            self.assertIsNone(self.run_site(self.words(w2=word)))

    def test_targets_must_be_decoded_instructions_and_table_in_bounds(self):
        self.assertIsNone(self.run_site(self.words(w11=0xe7ffdefe)))
        for target in (25, 0x1000):
            struct.pack_into('<i', IMG, self.TABLE, target - self.TABLE)
            self.assertIsNone(self.run_site(self.words()))
        struct.pack_into('<i', IMG, self.TABLE, self.TARGETS[0] - self.TABLE)
        self.assertIsNone(self.run_site(self.words(w8=ADRP(17, 32, 0x5000))))


class IndexCopiedBeforeCheck(unittest.TestCase):
    def run_site(self, middle):
        table = 0x4040
        words = [ADRP(9, 0, table), ADDI(9, 9, table & 0xfff), mov(18, 17, False)] + middle
        at = len(words) * 4
        words += [0x7100123f, bhi(at + 4, at + 20), 0xb8b27932, ADDR(18, 18, 9), BR(18), RET]
        for i in range(5): struct.pack_into('<i', IMG, table + 4 * i, at + 20 - table)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = [p for i, w in enumerate(words) for p in md.disasm(struct.pack('<I', w), 4 * i)]
        j = next(i for i, p in enumerate(ins) if p.mnemonic == 'br')
        return scope['jump_offsets'](ins, j, copy_first=True, far_base=True), [at + 20] * 5

    def test_copy_beyond_old_search_limit(self):
        actual, expected = self.run_site([NOP] * 75)
        self.assertEqual(actual, expected)

    def test_source_and_index_must_remain_unchanged(self):
        for reg in (17, 18):
            with self.subTest(reg=reg):
                self.assertIsNone(self.run_site([NOP] * 65 + [mov(reg, 0, False)])[0])

    def test_a_vector_load_changes_neither(self):
        for reg in (17, 18):
            with self.subTest(reg=reg):
                actual, expected = self.run_site([NOP] * 65 + [ldr_s(reg)])
                self.assertEqual(actual, expected)

    def test_calls_and_undecodable_gaps_break_the_chain(self):
        for word in (0x94000000, BLR(0), 0xe7ffdefe):
            with self.subTest(word=hex(word)):
                self.assertIsNone(self.run_site([NOP] * 65 + [word])[0])


class ZeroExtendedIndex(unittest.TestCase):
    TABLE = 0x4040
    TARGETS = [52 + i * 4 for i in range(5)]

    def setUp(self):
        tabjumps.clear(); jumps.clear()
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, target - self.TABLE)
        struct.pack_into('<i', IMG, self.TABLE + 20, 0x1000)   # not a sixth target

    def words(self, **over):
        # cmp w8,#4; b.hi exit; and x18,x8,#0xffffffff; ldrsw x18,[x16,x18,lsl #2]; add x18,x18,x16; br x18.
        words = [ADRP(16, 0, self.TABLE), ADDI(16, 16, self.TABLE & 0xfff), 0x7100111f, bhi(12, 48), NOP,
                 0x92407d12, NOP, 0xb8b27a12, NOP, ADDR(18, 18, 16), NOP, BR(18), RET] + [NOP] * 8
        for k, v in over.items(): words[int(k[1:])] = v
        return words

    def decode(self, words):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        return [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]

    def run_site(self, words):
        ins = self.decode(words)
        return scope['jump_zero_extended'](ins, next(i for i, p in enumerate(ins) if p.address == 44))

    def test_exact_five_targets_and_unrelated_writes(self):
        self.assertEqual(self.run_site(self.words()), self.TARGETS)
        self.assertEqual(self.run_site(self.words(w4=mov(9, 0), w6=mov(8, 0, False))), self.TARGETS)   # source is dead after the mask
        self.assertEqual(self.run_site(self.words(w6=0x7100051f)), self.TARGETS)   # cmp w8,#1 only writes flags

    def test_existing_masked_copy_keeps_its_targets(self):
        # and w9,w8,#0x3f; cmp w9,#4; b.hi exit; and x18,x8,#0x3f.
        words = self.words(w2=0x12001509, w3=0x7100113f, w4=bhi(16, 48), w5=0x92401512)
        ins = self.decode(words)
        self.assertEqual(scope['jump_offsets'](ins, 11), self.TARGETS)
        self.assertIsNone(self.run_site(words))

    def test_source_and_index_writes_are_rejected(self):
        for slot, reg in ((4, 8), (6, 18)):
            for wide in (False, True):
                with self.subTest(slot=slot, wide=wide):
                    self.assertIsNone(self.run_site(self.words(**{f'w{slot}': mov(reg, 0, wide)})))

    def test_calls_and_other_control_flow_are_rejected(self):
        for slot in (4, 6, 8, 10):
            for word in (0x94000040, BLR(0), RET, B(slot * 4, (slot + 1) * 4), BEQ(slot * 4, 48), 0xd4000521, 0xd4000002, 0xd4000003, 0xd4200000, 0xd4400000):
                with self.subTest(slot=slot, word=hex(word)):
                    self.assertIsNone(self.run_site(self.words(**{f'w{slot}': word})))

    def test_system_call_cannot_replace_the_checked_value(self):
        # cmp w0,#4; b.hi exit; svc #0x29; and x18,x0,#0xffffffff. GetInfo replaces x0/x1, which Capstone does not report.
        self.assertIsNone(self.run_site(self.words(w2=0x7100101f, w4=0xd4000521, w5=0x92407c12)))

    def test_mask_source_and_compare_width_must_agree(self):
        for slot, word in ((2, 0x7100113f), (2, 0xf100111f), (5, 0x92403d12), (5, 0x92407d32), (5, NOP)):
            with self.subTest(slot=slot, word=hex(word)):
                self.assertIsNone(self.run_site(self.words(**{f'w{slot}': word})))

    def test_guard_must_branch_out_on_unsigned_above(self):
        for word in (NOP, BEQ(12, 48), bhi(12, 20), bhi(12, 8)):
            self.assertIsNone(self.run_site(self.words(w3=word)))

    def test_loaded_entry_base_and_final_target_are_preserved(self):
        for slot, reg in ((8, 18), (8, 16), (10, 18)):
            for wide in (False, True):
                with self.subTest(slot=slot, reg=reg, wide=wide):
                    self.assertIsNone(self.run_site(self.words(**{f'w{slot}': mov(reg, 0, wide)})))

    def test_incoming_edges_cannot_bypass_the_check(self):
        for target in (12, 16, 20, 28, 36, 44):
            for word in (B(80, target), B(80, target) | 0x80000000):
                with self.subTest(target=target, word=hex(word)):
                    self.assertIsNone(self.run_site(self.words(w20=word)))
        self.assertEqual(self.run_site(self.words(w20=B(80, 8))), self.TARGETS)
        tabjumps[0x1000] = [20]
        self.assertIsNone(self.run_site(self.words()))
        tabjumps.clear(); jumps[0x1000] = (20, 1)
        self.assertIsNone(self.run_site(self.words()))

    def test_undecodable_words_break_the_proof(self):
        for slot in (4, 6, 8, 10, 13):
            with self.subTest(slot=slot):
                self.assertIsNone(self.run_site(self.words(**{f'w{slot}': 0xe7ffdefe})))

    def test_table_base_extent_and_targets_are_validated(self):
        self.assertIsNone(self.run_site(self.words(w0=NOP)))
        self.assertIsNone(self.run_site(self.words(w1=mov(16, 0, False))))
        self.assertIsNone(self.run_site(self.words(w0=ADRP(16, 0, 0x5000))))
        for target in (20, 44, 53, 0x1000):
            with self.subTest(target=target):
                struct.pack_into('<i', IMG, self.TABLE, target - self.TABLE)
                self.assertIsNone(self.run_site(self.words()))
        struct.pack_into('<i', IMG, self.TABLE, 8 - self.TABLE)
        self.assertEqual(self.run_site(self.words()), [8] + self.TARGETS[1:])

    def test_real_rewriter_uses_fallback_and_preserves_an_older_match(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_nso_match import image
        for older in (False, True):
            with self.subTest(older=older), tempfile.TemporaryDirectory() as tmp:
                words = self.words()
                if older:
                    # Both bounds are valid. The old matcher used cmp w18,#7; keep its eight targets rather than taking the nearer cmp w8,#4.
                    words[2:10] = [mov(18, 8, False), 0x71001e5f, bhi(16, 48), 0x7100111f, bhi(24, 48), 0x92407d12, 0xb8b27a12, ADDR(18, 18, 16)]
                table = 0x2040; words[0] = ADRP(16, 0x100, table)
                raw = image({0x100: words})
                targets = [0x100 + 52 + 4 * i for i in range(8 if older else 5)]
                for i, target in enumerate(targets): struct.pack_into('<i', raw, table + 4 * i, target - table)
                # Give the synthetic MOD0 image an NRO header, keeping its unwind metadata outside the header.
                raw[0x1100:0x111c] = raw[0x20:0x3c]; struct.pack_into('<I', raw, 4, 0x1100)
                struct.pack_into('<ii', raw, 0x1110, 0x3000 - 0x1100, 0x3014 - 0x1100)
                raw[0x10:0x14] = b'NRO0'; struct.pack_into('<I', raw, 0x18, len(raw))
                struct.pack_into('<6I', raw, 0x20, 0, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000); struct.pack_into('<I', raw, 0x38, 0)
                src = Path(tmp) / 'input.nro'; src.write_bytes(raw)
                run = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), tmp, 'nro0'],
                                     env=os.environ | {'DM_GAME': '010015100B514000', 'DM_REWRITE_J': '1'}, capture_output=True, text=True)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                rows = [line.split('\t') for line in (Path(tmp) / 'nro0.sites.tsv').read_text().splitlines()]
                self.assertEqual([[int(t, 0) for t in row[2].split()] for row in rows if row[1] == 'T'], [targets])


class MaskedPredecessor(unittest.TestCase):
    """The block `and w18,w17,#M; cmp w18,#3; b.hi; ldrsw x18,[x27,x6,lsl #2]; add; br x18` whose index x6 was masked from x17 in the block that branches to it.
    Word indices of the synthetic function: 0-1 base (adrp + add), 2-4 the predecessor (and x6, ubfx, cbz), 5 filler, 6 ret, 7-15 the dispatch block (and at 7, slots at 10, 12 and 14),
    16 ret, 17-20 the four targets. The table is at 0x4040 in the image."""
    TABLE = 0x4040
    TARGETS = [0x44, 0x48, 0x4c, 0x50]

    def setUp(self):
        for i, t in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, t - self.TABLE)
        tabjumps.clear(); jumps.clear()

    def words(self, **over):
        w = [ADRP(27, 0, self.TABLE), ADDI(27, 27, self.TABLE & 0xfff), AND_X6_X17_7FF, UBFX_W18_W17, cbz_w18(16, 28), NOP, RET,
             AND_W18_W17_7FF, CMP_W18_3, bhi(36, 64), NOP, LDRSW_X18, NOP, ADDR(18, 18, 27), NOP, BR(18), RET, NOP, NOP, NOP, NOP]
        for k, v in over.items(): w[int(k[1:])] = v
        return w

    def run_site(self, words, undecodable=False):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        # The driver skips words Capstone cannot decode and resumes at the next word.
        ins = [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]
        self.assertEqual(len(ins), len(words) - int(undecodable))
        return scope['jump_masked_pred'](ins, next(i for i, p in enumerate(ins) if p.mnemonic == 'br' and p.op_str == 'x18'))

    def test_recognized(self):
        self.assertEqual(self.run_site(self.words()), self.TARGETS)

    def test_base_can_be_hoisted_far_back_or_built_in_dispatch(self):
        for local in (False, True):
            with self.subTest(local=local):
                words = self.words()
                if local:
                    words[:2] = [NOP, NOP]
                    words[10] = ADRP(27, 40, self.TABLE)
                    words.insert(11, ADDI(27, 27, self.TABLE & 0xfff))
                    words[16:16] = [mov(22, 30)] * 10   # ten register moves after the add
                    shift = 44; words[9] = bhi(36, 64 + shift)
                else:
                    words[2:2] = [NOP] * 300   # base more than 256 instructions before the load
                    shift = 1200
                targets = [t + shift for t in self.TARGETS]
                for i, t in enumerate(targets): struct.pack_into('<i', IMG, self.TABLE + 4 * i, t - self.TABLE)
                self.assertEqual(self.run_site(words), targets)

    def test_compare_in_the_block_does_not_count_as_a_write(self):
        self.assertEqual(self.run_site(self.words(w14=CMP_W18_1)), self.TARGETS)

    def test_predecessor_must_mask_the_same_way(self):
        self.assertIsNone(self.run_site(self.words(w2=AND_X6_X17_FF)))
        self.assertIsNone(self.run_site(self.words(w2=NOP)))
        self.assertIsNone(self.run_site(self.words(w3=0x2a0003f1)))   # mov w17, w0: the source changes between the and and the branch

    def test_block_entered_only_by_branches(self):
        self.assertIsNone(self.run_site(self.words(w6=NOP)))   # falls through into the and
        self.assertIsNone(self.run_site(self.words(w5=cbz_w18(20, 44))))   # another branch lands inside the block, past its and
        tabjumps[0x1000] = {0x20}   # a jump table already resolved in the function lands on the cmp
        self.assertIsNone(self.run_site(self.words()))
        tabjumps.clear(); jumps[0x1004] = (0x20, 4)   # a byte or half-word table that reaches the block
        self.assertIsNone(self.run_site(self.words()))

    def test_registers_unchanged_between_check_and_use(self):
        self.assertIsNone(self.run_site(self.words(w10=mov(6, 0))))   # the index changes after the check
        self.assertIsNone(self.run_site(self.words(w12=mov(27, 0))))   # the base changes between the load and the add
        self.assertIsNone(self.run_site(self.words(w12=mov(18, 0))))   # x18 is rewritten before the add
        self.assertIsNone(self.run_site(self.words(w14=mov(18, 0))))   # x18 is rewritten after the add

    def test_tables_cannot_bypass_predecessor_mask(self):
        for target in (12, 16):   # the ubfx or cbz, after the predecessor's and
            with self.subTest(target=target, table='offset'):
                tabjumps[0x1000] = {target}
                self.assertIsNone(self.run_site(self.words()))
                tabjumps.clear()
            with self.subTest(target=target, table='byte'):
                jumps[0x1004] = (target, 1)
                self.assertIsNone(self.run_site(self.words()))
                jumps.clear()

    def test_table_can_enter_at_predecessor_mask(self):
        tabjumps[0x1000] = {8}; jumps[0x1004] = (8, 1)
        self.assertEqual(self.run_site(self.words()), self.TARGETS)

    def test_calls_cannot_enter_after_a_mask(self):
        for target in (12, 16, 28, 44):   # predecessor, dispatch head or load
            with self.subTest(target=target):
                self.assertIsNone(self.run_site(self.words(w5=B(20, target) | 0x80000000)))

    def test_32_bit_writer_destroys_table_base(self):
        self.assertIsNone(self.run_site(self.words(w5=mov(27, 0, wide=False))))

    def test_undecodable_words_cannot_be_targets_or_hide_writes(self):
        for slot in (3, 10, 17):   # predecessor, dispatch, target
            with self.subTest(slot=slot):
                self.assertIsNone(self.run_site(self.words(**{f'w{slot}': 0xe7ffdefe}), undecodable=True))

    def test_targets_must_be_instructions_of_the_function(self):
        struct.pack_into('<i', IMG, self.TABLE + 12, 0x1000)
        self.assertIsNone(self.run_site(self.words()))

    def test_no_base_no_site(self):
        self.assertIsNone(self.run_site(self.words(w0=NOP, w1=NOP)))


class MaskedCopyCheck(unittest.TestCase):
    TABLE = 0x4040
    TARGETS = [40 + i * 4 for i in range(5)]

    def run_site(self, between=NOP):
        # and w16,w18,#0x3f; <between>; cmp w16,#4; b.hi; and x18,x18,#0x3f; ldrsw x18,[x15,x18,lsl #2]; add x18,x18,x15; br x18
        words = [ADRP(15, 0, self.TABLE), ADDI(15, 15, self.TABLE & 0xfff), 0x12001650, between, 0x7100121f, bhi(20, 40),
                 0x92401652, 0xb8b279f2, ADDR(18, 18, 15), BR(18)] + [RET] * 5
        for i, target in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, target - self.TABLE)
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))
        self.assertEqual(len(ins), len(words))
        return scope['jump_offsets'](ins, 9)

    def test_a_vector_load_changes_neither_the_checked_register_nor_the_source(self):
        self.assertEqual(self.run_site(), self.TARGETS)
        for reg in (16, 18):
            with self.subTest(reg=reg):
                self.assertEqual(self.run_site(ldr_s(reg)), self.TARGETS)

    def test_a_general_purpose_write_of_either_is_refused(self):
        for reg in (16, 18):
            with self.subTest(reg=reg):
                self.assertIsNone(self.run_site(mov(reg, 0, False)))


class MaskedComplement(unittest.TestCase):
    TABLE = 0x4040
    TARGETS = [0x30 + i * 4 for i in range(7)]

    def setUp(self):
        tabjumps.clear(); jumps.clear()
        for i, t in enumerate(self.TARGETS): struct.pack_into('<i', IMG, self.TABLE + 4 * i, t - self.TABLE)
        # The next table's first word is deliberately not a target of this function.
        struct.pack_into('<i', IMG, self.TABLE + 28, 0x1000)

    def words(self, **over):
        w = [ADRP(11, 0, self.TABLE), ADDI(11, 11, self.TABLE & 0xfff),
             0x720009ff, BEQ(12, 44), 0x2a2f03ef, 0x924009ef, NOP,
             0xb8af796f, ADDR(18, 15, 11), mov(15, 16, False), BR(18), RET] + [NOP] * 7
        for k, v in over.items(): w[int(k[1:])] = v
        return w

    def run_site(self, words):
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = [p for i, word in enumerate(words) for p in md.disasm(struct.pack('<I', word), 4 * i)]
        return scope['jump_masked_not'](ins, next(i for i, p in enumerate(ins) if p.mnemonic == 'br' and p.op_str == 'x18'))

    def test_reads_exactly_seven_entries(self):
        self.assertEqual(self.run_site(self.words()), self.TARGETS)

    def test_guard_source_mask_and_complement_must_agree(self):
        for slot, word in ((2, 0x720009df), (2, 0x72000dff), (3, BEQ(12, 44) | 1), (3, BEQ(12, 20)),
                           (4, NOP), (4, 0x2a2e03ef), (5, 0x92400def)):
            with self.subTest(slot=slot, word=hex(word)):
                self.assertIsNone(self.run_site(self.words(**{f'w{slot}': word})))

    def test_intervening_writes_control_flow_and_undecodable_words(self):
        for slot, word in ((6, mov(15, 0)), (6, mov(11, 0, False)), (6, BLR(0)), (6, B(24, 28)), (6, 0xe7ffdefe), (9, mov(18, 0))):
            with self.subTest(slot=slot, word=hex(word)):
                self.assertIsNone(self.run_site(self.words(**{f'w{slot}': word})))

    def test_a_vector_load_is_not_a_write_and_x30_is(self):
        self.assertEqual(self.run_site(self.words(w6=ldr_s(15))), self.TARGETS)
        # the index in x30: and x30,x15,#7; <slot 6>; ldrsw x15,[x11,x30,lsl #2]
        index_x30 = {'w5': 0x924009fe, 'w7': 0xb8be796f}
        self.assertEqual(self.run_site(self.words(**index_x30)), self.TARGETS)
        self.assertIsNone(self.run_site(self.words(**index_x30, w6=mov(30, 0))))

    def test_incoming_edges_cannot_bypass_the_guard(self):
        for target in (12, 16, 20, 24, 28, 32, 36, 40):
            with self.subTest(target=target):
                self.assertIsNone(self.run_site(self.words(w11=B(44, target))))
        tabjumps[0x1000] = [20]
        self.assertIsNone(self.run_site(self.words()))
        tabjumps.clear(); jumps[0x1004] = (0, 8)
        self.assertIsNone(self.run_site(self.words()))
        jumps.clear()
        self.assertEqual(self.run_site(self.words(w11=B(44, 8))), self.TARGETS)

    def test_base_and_targets_must_be_valid(self):
        self.assertIsNone(self.run_site(self.words(w0=NOP)))
        self.assertIsNone(self.run_site(self.words(w1=mov(11, 0, False))))
        self.assertIsNone(self.run_site(self.words(w12=0xe7ffdefe)))
        struct.pack_into('<i', IMG, self.TABLE + 24, 0x1000)
        self.assertIsNone(self.run_site(self.words()))

    def test_this_tables_targets_cannot_bypass_the_guard(self):
        for target in (12, 16, 20, 28, 40):
            with self.subTest(target=target):
                struct.pack_into('<i', IMG, self.TABLE, target - self.TABLE)
                self.assertIsNone(self.run_site(self.words()))
        struct.pack_into('<i', IMG, self.TABLE, 8 - self.TABLE)
        self.assertEqual(self.run_site(self.words()), [8] + self.TARGETS[1:])


if __name__ == '__main__': unittest.main()
