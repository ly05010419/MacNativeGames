"""Exercise actual AArch64 thunk shapes without any game files."""
import ast
from pathlib import Path
import re
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from nso_asm import STP, STUR, STRX

tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
scope = {'re': re}
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef)
    and n.name in ('tail_call', 'tail_call_at_end')], type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)

# Standard frame, null guard to error block, pointer load, argument move,
# frame restore, tail jump, error call, trap padding.
THUNK = [0xa9bf7bfd, 0x910003fd, 0xb40000a1, 0xf9400812,
         0xaa0103e0, 0xa8c17bfd, 0xd61f0240, 0x94000400, 0]
# The same frame with two guards, the second on the pointer's table
GUARDS = [0xa9bf7bfd,  # 0x00: stp x29, x30, [sp, #-0x10]!
          0x910003fd,  # 0x04: mov x29, sp
          0xb40000c0,  # 0x08: cbz x0, #0x20
          0xf9400012,  # 0x0c: ldr x18, [x0]
          0xb4000092,  # 0x10: cbz x18, #0x20
          0xf9400652,  # 0x14: ldr x18, [x18, #8]
          0xa8c17bfd,  # 0x18: ldp x29, x30, [sp], #0x10
          0xd61f0240,  # 0x1c: br x18
          0x940003f8,  # 0x20: bl #0x1000
          0]           # 0x24: udf #0
# A frame of N bytes with the pair saved inside it
FRAME = [0xd10083ff,  # 0x00: sub sp, sp, #0x20
         0xa9017bfd,  # 0x04: stp x29, x30, [sp, #0x10]
         0x910043fd,  # 0x08: add x29, sp, #0x10
         0xb40000c0,  # 0x0c: cbz x0, #0x24
         0xf9400012,  # 0x10: ldr x18, [x0]
         0xaa0103e0,  # 0x14: mov x0, x1
         0xa9417bfd,  # 0x18: ldp x29, x30, [sp, #0x10]
         0x910083ff,  # 0x1c: add sp, sp, #0x20
         0xd61f0240,  # 0x20: br x18
         0x940003f7,  # 0x24: bl #0x1000
         0]           # 0x28: udf #0


def decode(words):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    return list(md.disasm(struct.pack('<' + 'I' * len(words), *words), 0))


class TailCall(unittest.TestCase):
    def check(self, words, j=6):
        ins = decode(words)
        self.assertEqual(len(ins), len(words))
        return scope['tail_call'](ins, j)

    def test_framed_thunk_with_error_block(self):
        self.assertTrue(self.check(THUNK))
        self.assertTrue(self.check(THUNK[:-1]))

    def test_legacy_terminal_patterns_are_unchanged(self):
        for words in ([0xf9400812, 0xd61f0240],
                      [0xf9400812, 0xb4000081, 0xaa0103e0, 0xd61f0240],
                      [0xaa0103f2, 0xd61f0240]):
            ins = decode(words)
            self.assertEqual(self.check(words, len(words) - 1), scope['tail_call_at_end'](ins, len(ins) - 1))
        self.assertTrue(self.check([0xf9400812, 0xb4000081, 0xaa0103e0, 0xd61f0240], 3))

    def test_clobbers_calls_branches_and_traps_rejected(self):
        for word in (0x52800012, 0xaa0003f2, 0x94000400, 0xd63f0000,
                     0x14000001, 0xb4000020, 0xd65f03c0, 0, 0x910043ff):
            with self.subTest(word=hex(word)):
                words = THUNK.copy(); words[4] = word
                self.assertFalse(self.check(words))

    def test_required_frame_and_guard(self):
        for index, word in ((0, 0xd503201f), (1, 0xd503201f), (2, 0xb4000021),
                            (2, 0xd503201f), (3, 0xaa0003f2), (5, 0xd503201f),
                            (7, 0xd503201f), (7, 0x97fffffd), (8, 0xaa0003e0)):
            with self.subTest(index=index, word=hex(word)):
                words = THUNK.copy(); words[index] = word
                self.assertFalse(self.check(words))

    def test_decoding_gap_is_rejected(self):
        ins = decode(THUNK)
        del ins[4]
        self.assertFalse(scope['tail_call'](ins, 5))

    def test_unframed_mid_function_pointer_jump_rejected(self):
        self.assertFalse(self.check([0xf9400812, 0xaa0103e0, 0xd61f0240, 0x94000400], 2))
        # The broader rule proposed in #188 accepts an unframed guarded block.
        # Keep the framed fallback's boundary even when a null guard is present.
        self.assertFalse(self.check([0xb4000061, 0xf9400812, 0xd61f0240, 0x94000400], 2))

    def test_stores_must_not_overwrite_saved_frame(self):
        for base in (31, 29):   # sp and the unchanged frame pointer
            for word in (STRX(0, base), STRX(0, base, 1), STUR(0, base, 15),
                         STP(0, 1, base, -8), STP(0, 1, base, 8)):
                with self.subTest(base=base, word=hex(word)):
                    words = THUNK.copy(); words[4] = word
                    self.assertFalse(self.check(words))

    def test_stack_and_frame_register_aliases_rejected(self):
        for word in (0x1100001f, 0x9100001f,   # mov wsp, w0; mov sp, x0
                     0x2a0003fd, 0xaa0003fd,   # mov w29, w0; mov x29, x0
                     0x2a0003fe, 0xaa0003fe):  # mov w30, w0; mov x30, x0
            with self.subTest(word=hex(word)):
                words = THUNK.copy(); words[4] = word
                self.assertFalse(self.check(words))

    def test_caller_stack_argument_stores_are_accepted(self):
        for base in (31, 29):
            for word in (STRX(0, base, 2), STUR(0, base, 16), STP(0, 1, base, 16)):
                with self.subTest(base=base, word=hex(word)):
                    words = THUNK.copy(); words[4] = word
                    self.assertTrue(self.check(words))

    def test_unproven_store_addresses_are_rejected(self):
        for word in (STRX(0, 9, 2), STUR(0, 9, 16), STP(0, 1, 9, 16),
                     0xf8206be0):   # str x0, [sp, x0]: dynamic offset may hit the frame
            with self.subTest(word=hex(word)):
                words = THUNK.copy(); words[4] = word
                self.assertFalse(self.check(words))

    def test_either_guard_sense_reaches_the_error_block(self):
        words = THUNK.copy(); words[2] = 0xb50000a1   # cbnz x1 to the error call
        self.assertTrue(self.check(words))

    def test_multi_guard_framed_thunk_with_error_blocks(self):
        self.assertTrue(self.check(GUARDS, 7))
        for index, word in ((2, 0x14000002),   # b #0x10: the body jumps inside itself
                            (2, 0xb4000040),   # cbz x0, #0x10: so does a guard
                            (2, 0xb5ffffc0),   # cbnz x0, #0: a loop to the entry
                            (8, 0x17fffffc),   # the error block's b #0x10: back into the body
                            (5, 0xa9400412)):  # ldp x18, x1, [x0] stays good in this form, below
            with self.subTest(index=index, word=hex(word)):
                words = GUARDS.copy(); words[index] = word
                self.assertEqual(self.check(words, 7), index == 5)

    def test_n_byte_framed_thunk_with_error_blocks(self):
        self.assertTrue(self.check(FRAME, 8))
        # Callee-saved spills around the frame pair and their restores in the epilogue, as on Ori
        spills = [0xd10143ff,  # sub sp, sp, #0x50
                  0xa9025ff8,  # stp x24, x23, [sp, #0x20]
                  0xa90357f6,  # stp x22, x21, [sp, #0x30]
                  0xa9047bfd,  # stp x29, x30, [sp, #0x40]
                  0x910103fd,  # add x29, sp, #0x40
                  0xb4000100,  # cbz x0, #0x34
                  0xf90003e0,  # str x0, [sp]: a local below the spills
                  0xf9400012,  # ldr x18, [x0]
                  0xa9447bfd,  # ldp x29, x30, [sp, #0x40]
                  0xa94357f6,  # ldp x22, x21, [sp, #0x30]
                  0xa9425ff8,  # ldp x24, x23, [sp, #0x20]
                  0x910143ff,  # add sp, sp, #0x50
                  0xd61f0240,  # br x18
                  0x90000000,  # adrp x0, #0
                  0x940003f0,  # bl out of the function
                  0]
        self.assertTrue(self.check(spills, 12))

    def test_function_calls_come_before_the_load(self):
        # A frame that saved x30 may call. A callee can change x18, so not after its load
        call = FRAME[:4] + [0x94000400] + FRAME[4:]
        call[3] = 0xb40000e0   # cbz x0 still reaches the code after the br
        self.assertTrue(self.check(call, 9))
        late = FRAME[:5] + [0x94000400] + FRAME[5:]
        late[3] = 0xb40000e0
        self.assertFalse(self.check(late, 9))
        for word in (0x94000400, 0xd63f0000):   # after the restore x30 is live again
            bad = FRAME[:7] + [word] + FRAME[7:]
            bad[3] = 0xb40000e0
            with self.subTest(word=hex(word)):
                self.assertFalse(self.check(bad, 9))

    def test_function_body_branches(self):
        # An ordinary function's lazy init: a forward branch over a call and a store through a global, ahead of the load
        init = [0xd10083ff,  # 0x00: sub sp, sp, #0x20
                0xa9017bfd,  # 0x04: stp x29, x30, [sp, #0x10]
                0x910043fd,  # 0x08: add x29, sp, #0x10
                0x37000068,  # 0x0c: tbnz w8, #0, #0x18
                0x94000400,  # 0x10: bl #0x1010
                0x39000128,  # 0x14: strb w8, [x9]
                0xf9400012,  # 0x18: ldr x18, [x0]
                0xa9417bfd,  # 0x1c: ldp x29, x30, [sp, #0x10]
                0x910083ff,  # 0x20: add sp, sp, #0x20
                0xd61f0240,  # 0x24: br x18
                0xd65f03c0]  # 0x28: another path's end
        self.assertTrue(self.check(init, 9))
        for index, word, why in ((3, 0xb5ffffa0, 'cbnz x0, #0: back into the frame setup'),
                                 (3, 0xb50000a0, 'cbnz x0, #0x20: between the restore and the br'),
                                 (3, 0x14000005, 'b #0x20: the same by a jump'),
                                 (10, 0x17fffffc, 'b #0x18: the code after the br comes back'),
                                 (10, 0x97fffffe, 'bl #0x20: a call into the function'),
                                 (4, 0xd61f0100, 'br x8: a jump whose targets are unknown')):
            with self.subTest(why=why):
                words = init.copy(); words[index] = word
                self.assertFalse(self.check(words, 9))
        # A guard may land on the load, or skip it for the restore: the frame's end still runs straight
        self.assertTrue(self.check(init[:3] + [0xb5000060] + init[4:], 9))
        self.assertTrue(self.check(init[:3] + [0xb5000080] + init[4:], 9))   # cbnz x0, #0x1c
        # A loop in the body, and a guard after the load that leaves for code after the br
        looped = init.copy(); looped[5] = 0xb5ffffe0   # cbnz x0, #0x10
        self.assertTrue(self.check(looped, 9))
        guard = FRAME[:5] + [0xb4000092] + FRAME[6:]   # cbz x18, #0x24
        self.assertTrue(self.check(guard, 8))

    def test_function_may_have_other_paths_after_the_jump(self):
        for tail in ([0xd65f03c0],              # its other path returns
                     [0xd61f0100],              # or jumps through another register
                     [0xd63f0100, 0],           # or calls through one
                     [0xaa0003e0],              # or runs on past the unwind entry
                     [0]):                      # or a trap fills the space: no guard needed
            with self.subTest(tail=tail):
                self.assertTrue(self.check(FRAME[:3] + [0xd503201f] + FRAME[4:9] + tail, 8))

    def test_n_byte_frame_must_come_back_as_found(self):
        for index, word in ((7, 0x9100c3ff),   # add sp, sp, #0x30: another size
                            (6, 0xa9407bfd),   # ldp x29, x30, [sp]: another slot
                            (1, 0xa9007bfd),   # stp x29, x30, [sp]: so is this save
                            (1, 0xa90107e0),   # stp x0, x1, [sp, #0x10]: the pair never saved
                            (2, 0x910003fd),   # mov x29, sp: not the saved pair's address
                            (4, 0x8b000252),   # add x18, x18, x0: not a pointer load
                            (5, 0x9100001f),   # mov sp, x0
                            (5, 0xaa0003fe),   # mov x30, x0
                            (5, 0xaa0003fd)):  # mov x29, x0
            with self.subTest(index=index, word=hex(word)):
                words = FRAME.copy(); words[index] = word
                self.assertFalse(self.check(words, 8))
        for word in (0xd10043ff, 0xaa0003e1):   # sub sp after the restore; a move is fine there
            with self.subTest(word=hex(word)):
                words = FRAME[:7] + [word] + FRAME[7:]
                words[3] = 0xb40000e0
                self.assertEqual(self.check(words, 9), word == 0xaa0003e1)

    def test_n_byte_stores_are_checked_by_their_bytes(self):
        # The pair is at sp + 0x10, which is x29 + 0
        for word, good in ((STRX(0, 31), True),            # [sp]
                           (0xf80083e0, True),             # stur x0, [sp, #8], its last byte right below the pair
                           (STP(0, 1, 31, 8), False),      # sp + 8 .. 0x18 overlaps the pair's first half
                           (STRX(0, 31, 2), False),        # sp + 0x10
                           (STRX(0, 31, 3), False),        # sp + 0x18
                           (0x390063e0, False),            # strb w0, [sp, #0x18]: one byte is enough
                           (0xf90013e0, True),             # [sp, #0x20]: the caller's stack arguments
                           (STRX(0, 29), False),           # x29 + 0
                           (STUR(0, 29, 15), False),       # x29 + 15
                           (STRX(0, 29, 2), True),         # x29 + 0x10: above the pair
                           (STUR(0, 29, -8), True),        # x29 - 8: below it
                           (STP(0, 1, 29, -8), False),     # x29 - 8 .. 8
                           (STRX(0, 9), True),             # another base: a function's callees cannot be proven either
                           (0xf8206be0, False),            # str x0, [sp, x0]: a register offset is unproven
                           (0xf81f0fe0, False)):           # str x0, [sp, #-0x10]!
            with self.subTest(word=hex(word)):
                words = FRAME.copy(); words[5] = word
                self.assertEqual(self.check(words, 8), good)

    def test_exclusive_and_atomic_accesses_are_not_modelled(self):
        for word in (0x88007c20, 0xc89ffc20, 0xb8208020):   # stxr w0, w0, [x1]; stlr x0, [x1]; swp w0, w0, [x1]
            with self.subTest(word=hex(word)):
                words = FRAME.copy(); words[5] = word
                self.assertFalse(self.check(words, 8))

    def test_nothing_is_stored_between_the_restore_and_the_jump(self):
        bad = FRAME[:7] + [STRX(0, 31, 3)] + FRAME[7:]
        bad[3] = 0xb40000e0
        self.assertFalse(self.check(bad, 9))

    def test_thunk_error_blocks_never_return(self):
        for index, word in ((8, 0xd65f03c0),   # ret
                            (9, 0xd65f03c0),
                            (9, 0xd61f0100),   # br x8
                            (9, 0xd63f0100),   # blr x8
                            (9, 0xaa0003e0),   # runs on past the blocks
                            (8, 0xd503201f),   # no call out of the function
                            (8, 0x97fffffa),   # bl into the body
                            (9, 0x17fffffc)):  # b back into the body
            with self.subTest(index=index, word=hex(word)):
                words = GUARDS.copy(); words[index] = word
                self.assertFalse(self.check(words, 7))
        self.assertTrue(self.check(GUARDS[:9], 7))   # an error call needs no trap after it
        longer = GUARDS[:8] + [0x90000000, 0x91000000, 0x94000400, 0x90000000, 0x94000400, 0x17ffffff]   # adrp, add, bl; adrp, bl; b back
        self.assertTrue(self.check(longer, 7))

    def test_thunk_rejects_what_a_function_may_do(self):
        for word in (0x94000400, 0xd63f0000,   # calls
                     STRX(0, 9, 2)):           # a store through another base
            with self.subTest(word=hex(word)):
                words = GUARDS.copy(); words[5] = word; words[3] = 0xf9400012
                self.assertFalse(self.check(words, 7))

    def test_terminal_rule_loads_with_ldr_only(self):
        # The ldp load belongs to the framed rule's proof, not to the terminal one, which proves no frame
        self.assertFalse(self.check([0xa9400412, 0xd61f0240], 1))


if __name__ == '__main__':
    unittest.main()
