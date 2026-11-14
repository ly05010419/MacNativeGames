"""patch_nso.py's search for test-and-set lock acquires without acquire ordering (acquire_lock_sites), on hand-assembled words, without game files."""
import ast
from pathlib import Path
import struct
from typing import Any
import unittest

tree = ast.parse((Path(__file__).resolve().parents[1] / 'patch_nso.py').read_text())   # the program loads and rewrites an image: compile only the search
ns: dict[str, Any] = {'struct': struct}
exec(compile(ast.Module([n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'acquire_lock_sites'], []), 'patch_nso.py', 'exec'), ns)
find = ns['acquire_lock_sites']

def cbnz(rt, back, x=0): return 0x35000000 | x << 31 | (0x80000 - back) << 5 | rt
def ldxr(rt, rn, x=0): return 0x885F7C00 | x << 30 | rn << 5 | rt
def stxr(rs, rk, rn, x=0): return 0x88007C00 | x << 30 | rs << 16 | rn << 5 | rk
def img(*w): return b''.join(struct.pack('<I', x) for x in w)

class AcquireLock(unittest.TestCase):
    def test_w_and_x_forms_found(self):   # BotW main+0x13a0784: ldxr w9,[x23]; cbnz w9; stxr w9,w8,[x23]; cbnz w9
        pad = img(0xD503201F)
        for x in (0, 1):
            code = pad + img(ldxr(9, 23, x), cbnz(9, 1, x), stxr(9, 8, 23, x), cbnz(9, 3, x)) + pad
            self.assertEqual(find(code, [(0, len(code))]), [4])

    def test_not_a_plain_acquire(self):
        pad = img(0xD503201F)
        for name, w in (('release store', (ldxr(9, 23), cbnz(9, 1), stxr(9, 31, 23), cbnz(9, 3))),   # stxr wzr: an unlock
                        ('already ldaxr', (ldxr(9, 23) | 0x8000, cbnz(9, 1), stxr(9, 8, 23), cbnz(9, 3))),
                        ('other base', (ldxr(9, 23), cbnz(9, 1), stxr(9, 8, 22), cbnz(9, 3))),
                        ('retry elsewhere', (ldxr(9, 23), cbnz(9, 1), stxr(9, 8, 23), cbnz(9, 4))),
                        ('stlxr', (ldxr(9, 23), cbnz(9, 1), stxr(9, 8, 23) | 0x8000, cbnz(9, 3)))):
            code = pad + img(*w) + pad
            self.assertEqual(find(code, [(0, len(code))]), [], name)

    def test_fenced_lock_left_alone(self):   # Tears of the Kingdom, Super Mario Bros. Wonder: the same loop, then dmb ish (or dmb ishld): already ordered
        for dmb in (0xD5033BBF, 0xD50339BF):
            code = img(ldxr(9, 23), cbnz(9, 1), stxr(9, 8, 23), cbnz(9, 3), dmb)
            self.assertEqual(find(code, [(0, len(code))]), [])

    def test_range_is_respected(self):
        code = img(ldxr(9, 23), cbnz(9, 1), stxr(9, 8, 23), cbnz(9, 3), 0xD503201F)
        self.assertEqual(find(code, [(0, 12)]), [])
        self.assertEqual(find(code, [(0, 16)]), [0])


if __name__ == '__main__':
    unittest.main()
