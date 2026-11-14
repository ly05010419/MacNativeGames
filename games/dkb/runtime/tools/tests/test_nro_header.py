"""NRO metadata must survive the real rewriter even when a build ID decodes as x18 instructions."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))   # its sibling test module, also when the file runs alone (python -m unittest <file>, as CI does)
from test_nso_match import image, RET   # noqa: E402

TOOLS = Path(__file__).resolve().parents[1]


class NroHeader(unittest.TestCase):
    def test_header_preserved_and_text_rewritten(self):
        self.check_rewrite(0x80, 0x58ff0012)   # literal ldr x18, outside the module

    def test_function_before_first_fde_cannot_reenter_header(self):
        # The linear scans alone are not enough: func_of must also start after the header.
        # Otherwise its br x18 looks like an unrecognized jump table during function analysis.
        self.check_rewrite(0x100, 0xd61f0240)   # br x18

    def check_rewrite(self, first_fde, header_word):
        # A real function starts immediately after the NRO header. Both it and the build ID name x18.
        raw = image({first_fde: [0xaa0003f2, RET]})
        struct.pack_into('<II', raw, 0x80, 0xaa0003f2, RET)   # mov x18, x0; ret, with or without an FDE
        raw[0x1100:0x111c] = raw[0x20:0x3c]
        struct.pack_into('<I', raw, 4, 0x1100)
        struct.pack_into('<ii', raw, 0x1110, 0x3000 - 0x1100, 0x3014 - 0x1100)
        raw[0x10:0x14] = b'NRO0'
        struct.pack_into('<I', raw, 0x18, len(raw))
        struct.pack_into('<6I', raw, 0x20, 0, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000)
        struct.pack_into('<I', raw, 0x38, 0)
        struct.pack_into('<I', raw, 0x44, header_word)
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'input.nro'; src.write_bytes(raw)
            env = os.environ | {'DM_GAME': '010015100B514000'}
            run = subprocess.run([sys.executable, str(TOOLS / 'patch_nso.py'), str(src), tmp, 'nro0'], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            patched = (Path(tmp) / 'nro0.patched.bin').read_bytes()
            self.assertEqual(patched[:0x80], raw[:0x80])
            self.assertNotEqual(patched[0x80:0x84], raw[0x80:0x84])


if __name__ == '__main__': unittest.main()
