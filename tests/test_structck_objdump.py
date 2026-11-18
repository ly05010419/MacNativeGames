"""structck.py's independent disassembly without Homebrew's llvm (2026-11-12): code_elf wraps raw words in an AArch64 ELF
that Xcode's own llvm-objdump reads, in place of `llvm-objcopy -I binary` (Xcode has no llvm-objcopy).

structck.py runs as a script (its checks are top-level code), so the test takes code_elf and llvm_dis out of its source.
On the real rewritten modules (OoT main/sdk, TotK 1.4.3 main/sdk/subsdk0) the STRUCT lines matched Homebrew llvm@23 word
for word; the old ELF only added adrp lines whose symbol comments matched the grep, never an x18 / system-register / svc line.
"""
import ast
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COPIES = [ROOT / 'games/oot/third_party/d2r/structck.py', ROOT / 'games/totk/third_party/d2r/structck.py']
WORDS = {
    0x00: (0xD4000001, 'svc'),                  # svc #0
    0x04: (0xD53BD040, 'tpidr_el0'),            # mrs x0, tpidr_el0
    0x08: (0xAA1203E0, 'x18'),                  # mov x0, x18
    0x0C: (0xD503201F, None),                   # nop
    0x10: (0xD53BE040, 'cntvct_el0'),           # mrs x0, cntvct_el0
}


def functions(path, names):
    """The named top-level functions of a script, compiled on their own."""
    tree = ast.parse(path.read_text())
    keep = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    scope = {'os': os, 're': re, 'struct': struct, 'subprocess': subprocess, 'tempfile': tempfile}
    exec(compile(ast.Module(body=keep, type_ignores=[]), str(path), 'exec'), scope)
    return scope


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode')
class StructckObjdump(unittest.TestCase):
    def test_copies_agree(self):
        self.assertEqual(COPIES[0].read_bytes(), COPIES[1].read_bytes())   # TotK / BotW and OoT check the same way

    def test_no_homebrew_llvm(self):
        text = COPIES[0].read_text()
        self.assertNotIn('/opt/homebrew', text)
        self.assertNotIn('llvm-objcopy\',', text)

    def test_xcode_objdump_reads_the_wrapped_words(self):
        scope = functions(COPIES[0], {'find_objdump', 'code_elf', 'llvm_dis'})
        env = {k: v for k, v in os.environ.items() if k not in ('LLVM_BIN', 'LLVM_OBJDUMP')}
        old = os.environ.copy()
        try:
            os.environ.clear(); os.environ.update(env)
            scope['OBJDUMP'] = scope['find_objdump']()
        finally:
            os.environ.clear(); os.environ.update(old)
        self.assertTrue(scope['OBJDUMP'].endswith('llvm-objdump'))
        self.assertNotIn('/opt/homebrew', scope['OBJDUMP'])
        blob = b''.join(struct.pack('<I', word) for word, _ in WORDS.values())
        lines = scope['llvm_dis'](blob, 0x1000)
        for offset, (_, needle) in WORDS.items():
            if needle is None:
                self.assertNotIn(0x1000 + offset, lines)
            else:
                self.assertIn(needle, lines[0x1000 + offset])

    def test_elf_layout(self):
        elf = functions(COPIES[0], {'code_elf'})['code_elf'](b'\x1f\x20\x03\xd5' * 3)
        self.assertEqual(elf[:4], b'\x7fELF')
        self.assertEqual(struct.unpack_from('<H', elf, 18)[0], 183)   # EM_AARCH64
        self.assertEqual(elf[64:76], b'\x1f\x20\x03\xd5' * 3)          # .text holds the words as given


if __name__ == '__main__':
    unittest.main()
