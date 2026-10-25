"""The rewriter's spin wait search (games/totk/third_party/d2r/patch_nso.py "Spin waits") on hand-assembled AArch64 loops, without game files.
Ported from nxnative (https://github.com/seedds/nxnative, MIT, dm/tools/tests/test_spin_waits.py; games/totk/third_party/licenses/nxnative.txt)."""
import ast
from pathlib import Path
import re
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

TOOLS = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r'
# patch_nso.py is a command-line program whose top level loads and rewrites an image: compile only the spin wait search and its constants
tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
FUNCS = {'spin_ins', 'spin_norm', 'spin_rw', 'spin_kind', 'spin_rmw', 'spin_lock', 'spin_store', 'spin_quick', 'spin_region', 'spin_region1'}
CONSTS = {'SPIN_PATHS', 'SPIN_STEPS', 'SPIN_STORES', 'SPIN_LSE', 'SPIN_BAD', 'SPIN_STOP'}
body = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in FUNCS) or (isinstance(n, ast.ClassDef) and n.name == 'SpinNo')
        or (isinstance(n, ast.Assign) and any(isinstance(t, ast.Tuple) and any(getattr(e, 'id', None) in CONSTS for e in t.elts) or getattr(t, 'id', None) in CONSTS for t in n.targets))]

# A lock-protected queue poll, hand-written for this test (registers and offsets are our own; the loop shape is the one of a job
# queue whose first item belongs to another thread): take the queue's spin lock, look whether the wait is over, release, again.
#   0x00 T:  cbz x2, X                  0x2c     str w6, [x2, #0x40]        0x50     ldr w11, [x7, #0x10]       (the wait is over: take an item,
#   0x04     str w5, [x2, #0x40]        0x30     ldrb w3, [x7, #0x18]       0x54     sub w12, w11, w10           store the new cursor, release
#   0x08 L0: ldr w3, [x1]               0x34     cmp w9, #0                 0x58     add w13, w12, w10           and leave)
#   0x0c     cmp w3, #1                 0x38     ccmp w3, #0, #4, ne        0x5c     str w13, [x7, #0x14]
#   0x10     b.eq L0                    0x3c     ldr w10, [x7, #0x14]       0x60     cset w11, hs
#   0x14 L1: ldaxr w3, [x1]             0x40     ccmp w10, #0, #0, ne       0x64     b U
#   0x18     cbz w3, L2                 0x44     cset w3, gt                0x68 C:  clrex
#   0x1c     clrex                      0x48     cmp w3, #1                 0x6c U:  ldxr w13, [x1] / cmp w13, #1 / b.ne C
#   0x20     b L1                       0x4c     b.ne U                     0x78     stlxr w13, wzr, [x1] / cbnz w13, U
#   0x24 L2: stxr w3, w4, [x1] / cbnz w3, L1                                0x80     cbz w3, T           0x84 X: ret
POLL = [0xb4000422, 0xb9004045, 0xb9400023, 0x7100047f, 0x54ffffc0, 0x885ffc23, 0x34000063, 0xd5033f5f, 0x17fffffd, 0x88037c24, 0x35ffff63, 0xb9004046,
        0x394060e3, 0x7100013f, 0x7a401864, 0xb94014ea, 0x7a401940, 0x1a9fd7e3, 0x7100047f, 0x54000101, 0xb94010eb, 0x4b0a016c, 0x0b0a018d, 0xb90014ed,
        0x1a9f37eb, 0x14000002, 0xd5033f5f, 0x885f7c2d, 0x710005bf, 0x54ffffa1, 0x880dfc3f, 0x35ffff8d, 0x34fffc03, 0xd65f03c0]


def region(words, T, bes):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
    scope = {'re': re, 'struct': struct, 'ARM64_OP_IMM': ARM64_OP_IMM, 'ARM64_OP_MEM': ARM64_OP_MEM, 'ARM64_OP_REG': ARM64_OP_REG,
             'IMG': struct.pack('<' + 'I' * len(words), *words) + bytes(64), 'md': md, 'spin_dec': {}, 'func_of': lambda a: (0, 1 << 30), 'spin_touched': set()}
    exec(compile(ast.Module(body=body, type_ignores=[]), str(TOOLS / 'patch_nso.py'), 'exec'), scope)
    return scope['spin_region'](T, bes)


def patched(**at):
    w = list(POLL)
    for off, word in at.items(): w[int(off[1:], 16) // 4] = word
    return w


class SpinWaits(unittest.TestCase):
    def test_lock_poll_is_a_wait(self):
        g = region(POLL, 0, [0x80])
        self.assertIsNotNone(g)
        self.assertEqual(g['valid'], {0x80})
        self.assertNotIn(0x5c, g['R'])          # the path that takes an item stores a new cursor: not part of the wait
        self.assertEqual({g['nodes'][a]['kind'] for a in g['R']} >= {'acq', 'rel', 'tst', 'stc'}, True)

    def test_store_of_a_changing_value(self):   # the wait path stores w3, which the pass writes
        self.assertIsNone(region(patched(o2c=0xb9004043), 0, [0x80]))

    def test_lock_not_released(self):           # the wait path skips the release (b.ne U → b.ne to the back-edge)
        self.assertIsNone(region(patched(o4c=0x540001a1), 0, [0x80]))

    def test_nothing_polled(self):              # str xzr, [x8]; b back: stores of a constant and no load
        self.assertIsNone(region([0xf900011f, 0x17ffffff], 0, [4]))

    def test_plain_test_loop(self):             # ldr w8, [x0]; cbz w8, back
        g = region([0xb9400008, 0x34ffffe8], 0, [4])
        self.assertIsNotNone(g)
        self.assertEqual(g['valid'], {4})


if __name__ == '__main__':
    unittest.main()
