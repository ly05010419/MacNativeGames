"""The rewriter's spin wait search (patch_nso.py "Spin waits") on hand-assembled AArch64 loops, without game files."""
import ast
from pathlib import Path
import re
import struct
import sys
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

TOOLS = Path(__file__).resolve().parents[1]
# patch_nso.py is a command-line program whose top level loads and rewrites an image: compile only the spin wait search and its constants
tree = ast.parse((TOOLS / 'patch_nso.py').read_text())
FUNCS = {'spin_ins', 'spin_norm', 'spin_rw', 'spin_kind', 'spin_rmw', 'spin_lock', 'spin_store', 'spin_quick', 'spin_region', 'spin_region1'}
CONSTS = {'SPIN_PATHS', 'SPIN_STEPS', 'SPIN_STORES', 'SPIN_LSE', 'SPIN_BAD', 'SPIN_STOP'}
body = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in FUNCS) or (isinstance(n, ast.ClassDef) and n.name == 'SpinNo')
        or (isinstance(n, ast.Assign) and any(isinstance(t, ast.Tuple) and any(getattr(e, 'id', None) in CONSTS for e in t.elts) or getattr(t, 'id', None) in CONSTS for t in n.targets))]

# A job queue poll (the shape of Breath of the Wild's main+0xef6354): take the queue's spin lock, look whether the wait is over, release the lock, again.
#   0x00 T:  cbz x20, X                 0x2c     str w12, [x20, #0x100]     0x50     ldr w13, [x21, #0x90]      (the wait is over: take an item,
#   0x04     str w11, [x20, #0x100]     0x30     ldrb w14, [x21, #0x98]     0x54     sub w8, w13, w25            store the new cursor, release
#   0x08 L0: ldr w14, [x9]              0x34     cmp w0, #0                 0x58     add w15, w8, w25            and leave)
#   0x0c     cmp w14, #1                0x38     ccmp w14, #0, #4, ne       0x5c     str w15, [x21, #0x94]
#   0x10     b.eq L0                    0x3c     ldr w25, [x21, #0x94]      0x60     cset w13, hs
#   0x14 L1: ldaxr w14, [x9]            0x40     ccmp w25, #0, #0, ne       0x64     b U
#   0x18     cbz w14, L2                0x44     cset w14, gt               0x68 C:  clrex
#   0x1c     clrex                      0x48     cmp w14, #1                0x6c U:  ldxr w15, [x9] / cmp w15, #1 / b.ne C
#   0x20     b L1                       0x4c     b.ne U                     0x78     stlxr w15, wzr, [x9] / cbnz w15, U
#   0x24 L2: stxr w14, w10, [x9] / cbnz w14, L1                             0x80     cbz w14, T          0x84 X: ret
POLL = [0xb4000434, 0xb901028b, 0xb940012e, 0x710005df, 0x54ffffc0, 0x885ffd2e, 0x3400006e, 0xd5033f5f, 0x17fffffd, 0x880e7d2a, 0x35ffff6e, 0xb901028c,
        0x394262ae, 0x7100001f, 0x7a4019c4, 0xb94096b9, 0x7a401b20, 0x1a9fd7ee, 0x710005df, 0x54000101, 0xb94092ad, 0x4b1901a8, 0x0b19010f, 0xb90096af,
        0x1a9f37ed, 0x14000002, 0xd5033f5f, 0x885f7d2f, 0x710005ff, 0x54ffffa1, 0x880ffd3f, 0x35ffff8f, 0x34fffc0e, 0xd65f03c0]


def region(words, T, bes):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
    scope = {'re': re, 'struct': struct, 'ARM64_OP_IMM': ARM64_OP_IMM, 'ARM64_OP_MEM': ARM64_OP_MEM, 'ARM64_OP_REG': ARM64_OP_REG,
             'IMG': struct.pack('<' + 'I' * len(words), *words) + bytes(64), 'md': md, 'spin_dec': {}, 'DATA_BLOCKS': [], 'spin_touched': set()}
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

    def test_store_of_a_changing_value(self):   # the wait path stores w14, which the pass writes
        self.assertIsNone(region(patched(o2c=0xb901028e), 0, [0x80]))

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
