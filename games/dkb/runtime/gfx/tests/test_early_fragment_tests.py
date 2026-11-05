"""Early fragment tests come from control +0x718 of a pixel shader, unless its SPH writes depth (dmsh_build.py early_fragment_tests)."""
import ast
from pathlib import Path
import struct
import unittest

path = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
node = next(n for n in ast.parse(path.read_text()).body if isinstance(n, ast.FunctionDef) and n.name == 'early_fragment_tests')
scope = {'struct': struct}
exec(compile(ast.Module(body=[node], type_ignores=[]), str(path), 'exec'), scope)
early = scope['early_fragment_tests']


def control(flag, size=0x880):
    c = bytearray(size); c[:4] = bytes.fromhex('34127698')
    if size > 0x718: c[0x718] = flag
    return bytes(c)


def sph(word19=0):
    return struct.pack('<20I', *([0] * 19 + [word19])) + bytes(0x40)   # the SPH, then instructions


class EarlyFragmentTests(unittest.TestCase):
    def test_flag_set(self):
        self.assertTrue(early(control(1), sph()))

    def test_flag_clear(self):
        self.assertFalse(early(control(0), sph()))

    def test_short_control_section(self):   # 0x850 (SDK 10's 3D World) still holds +0x718
        self.assertTrue(early(control(1, 0x850), sph()))
        self.assertFalse(early(control(1, 0x700), sph()))

    def test_depth_writer_left_late(self):   # SPH word 19 bit 1: OmapDepth
        self.assertFalse(early(control(1), sph(2)))
        self.assertTrue(early(control(1), sph(1)))   # bit 0, the sample mask output, does not matter

    def test_no_sph(self):
        self.assertFalse(early(control(1), bytes(0x20)))


if __name__ == '__main__':
    unittest.main()
