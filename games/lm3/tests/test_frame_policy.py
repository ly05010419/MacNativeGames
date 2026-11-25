"""Synthetic identity guards and real-frame pacing at 30/60 on high-refresh displays."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FramePolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='lm3-frame-policy-')
        source = Path(cls.work.name) / 'policy.c'
        library = Path(cls.work.name) / 'policy.dylib'
        source.write_text('''#include "lm3_frame_policy.h"
int guard(const unsigned char *p, size_t n, const unsigned char *id) { return lm3_frame_guard(p,n,id); }
unsigned long long period(int fps,int vsync,int cap,int hz) { return lm3_frame_period_ns(fps,vsync,cap,hz); }
int interval(unsigned long long ns) { return lm3_frame_interval(ns); }
''')
        subprocess.run(['xcrun', 'clang', '-std=c11', '-dynamiclib', '-I', str(ROOT / 'runtime'),
                        str(source), '-o', str(library)], check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.guard.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lib.period.argtypes = [ctypes.c_int] * 4
        cls.lib.period.restype = ctypes.c_uint64
        cls.lib.interval.argtypes = [ctypes.c_uint64]

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def fixture(self):
        image = bytearray(0x13c94e0)
        for offset, word in ((0x7a3e9c, 0xd000612c), (0x7a3ea8, 0xbd44dd80),
                             (0x7a3f18, 0xbd44dd82), (0x13c94dc, 0x42055555)):
            struct.pack_into('<I', image, offset, word)
        build = bytearray.fromhex('79e5950ffa85acf63e28c9aec051eac27d6f7f8d' + '00' * 12)
        return image, build

    def guarded(self, image, build, size=None):
        data = (ctypes.c_ubyte * len(image)).from_buffer(image)
        identity = (ctypes.c_ubyte * len(build)).from_buffer(build)
        return self.lib.guard(data, len(image) if size is None else size, identity)

    def test_exact_build_and_all_consumer_bits(self):
        image, build = self.fixture()
        self.assertTrue(self.guarded(image, build))
        for offset in (0x7a3e9c, 0x7a3ea8, 0x7a3f18, 0x13c94dc):
            for bit in range(32):
                byte = offset + bit // 8
                image[byte] ^= 1 << (bit % 8)
                self.assertFalse(self.guarded(image, build), (offset, bit))
                image[byte] ^= 1 << (bit % 8)
        for byte in range(32):
            for bit in range(8):
                build[byte] ^= 1 << bit
                self.assertFalse(self.guarded(image, build), (byte, bit))
                build[byte] ^= 1 << bit

    def test_truncated_image_and_null_rejected(self):
        image, build = self.fixture()
        for length in (0, 0x7a3e9c, 0x7a3ea9, 0x13c94df):
            self.assertFalse(self.guarded(image, build, length))
        self.assertFalse(self.lib.guard(None, len(image), None))

    def test_caps_cannot_exceed_engine_target(self):
        cases = [
            ((60, 1, 0, 120), 60, 1),
            ((60, 0, 0, 144), 60, 1),
            ((60, 1, 240, 144), 60, 1),
            ((60, 1, 30, 120), 30, 2),
            ((60, 1, 60, 30), 30, 2),
            ((60, 0, 60, 30), 60, 1),
            ((30, 1, 60, 120), 30, 2),
            ((30, 0, 0, 144), 30, 2),
            ((60, 1, 15, 60), 15, 4),
            ((60, 1, 60, 0), 60, 1),
        ]
        for args, fps, interval in cases:
            period = self.lib.period(*args)
            self.assertEqual(period, 1000000000 // fps, args)
            self.assertEqual(self.lib.interval(period), interval, args)
        self.assertEqual(self.lib.period(0, 1, 0, 120), 0)
        self.assertEqual(self.lib.period(120, 0, 0, 144), 0)


if __name__ == '__main__':
    unittest.main()
