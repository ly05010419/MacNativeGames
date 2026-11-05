"""Execute ASTRAL CHAIN's emitted timing stubs on ARM64, without game files.

Optional dependency: unicorn. On macOS its JIT needs execution outside the sandbox.
"""
import importlib.util
import math
from pathlib import Path
import struct
import sys
import types
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    import unicorn as uc
    from unicorn import arm64_const as arm
except ImportError:
    uc = arm = None

spec = importlib.util.spec_from_file_location('astral_patches', Path(__file__).resolve().parents[2] / 'games/01007300020FA000/patches.py')
patch = importlib.util.module_from_spec(spec); spec.loader.exec_module(patch)


@unittest.skipIf(uc is None, 'optional ARM64 execution dependency: unicorn')
class Timing(unittest.TestCase):
    base, area, state, obj, stack = 0x4000000, 0x2000000, 0x10cec000, 0x6000000, 0x6100000

    def setUp(self):
        self.u = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        for a, n in ((self.base, 0xe00000), (self.area, 0x20000), (self.state, 0x4000), (self.obj, 0x1000), (self.stack, 0x10000)):
            self.u.mem_map(a, n)
        self.cursor = self.area - self.base
        self.ctx = types.SimpleNamespace(here=lambda: self.cursor, emit=self.emit, site=lambda off, orig: None,
                                         put=self.put, state=self.state-self.base)
        patch.frame_step(self.ctx)
        self.u.mem_write(self.base+0xdb89a0, struct.pack('<Q', self.obj))
        self.u.mem_write(self.base+0x8a9570, bytes.fromhex('e003271ec0035fd6'))  # no speed override
        self.u.reg_write(arm.UC_ARM64_REG_SP, self.stack+0x8000)
        self.u.reg_write(arm.UC_ARM64_REG_X19, self.obj)
        self.u.reg_write(arm.UC_ARM64_REG_X24, self.obj)
        self.u.mem_write(self.state, struct.pack('<f', 60))

    def emit(self, w):
        self.u.mem_write(self.base+self.cursor, struct.pack('<I', w)); self.cursor += 4

    def put(self, off, words, why):
        self.u.mem_write(self.base+off, struct.pack('<'+'I'*len(words), *words))

    def execute(self, start, end, count):
        # A previously translated Unicorn block can cross a new emu_start end.
        # Stop at the exact return site even when adjacent hooks share a block.
        stop = self.u.hook_add(uc.UC_HOOK_CODE, lambda u, a, n, d: u.emu_stop(), begin=end, end=end)
        try:
            self.u.emu_start(start, end, count=count)
        finally:
            self.u.hook_del(stop)

    def read32(self, a): return struct.unpack('<I', self.u.mem_read(a, 4))[0]
    def step(self, vsyncs=1, variable=True):
        self.u.mem_write(self.state+0x20, struct.pack('<I', vsyncs))
        self.u.reg_write(arm.UC_ARM64_REG_W21, int(variable))
        self.execute(self.base+0x91dc10, self.base+0x91dcb4, count=500)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), self.base+0x91dcb4)
        return struct.unpack('<f', self.u.mem_read(self.obj+0xd0, 4))[0], self.read32(self.obj+0xd8)

    def test_integer_clock_keeps_thirty_ticks_per_second(self):
        for vsyncs, frames in ((1, 60), (2, 30)):
            self.u.mem_write(self.obj+0xd4, bytes(4))
            total = 0
            for _ in range(frames):
                dt, ticks = self.step(vsyncs)
                self.assertEqual(dt, vsyncs/2)
                # Exercise the actual integer consumer hook, not a Python conversion.
                self.u.reg_write(arm.UC_ARM64_REG_X8, self.obj)
                self.execute(self.base+0x48be14, self.base+0x48be18, count=100)
                self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_W8), ticks)
                total += ticks
            self.assertEqual(total, 30)

    def test_fractional_carry_across_variable_frame_steps(self):
        elapsed, integral = 0, 0
        for v in (1, 2, 1, 3, 1)*20:
            dt, ticks = self.step(v); elapsed += dt; integral += ticks
            self.assertEqual(integral, int(elapsed))
        self.assertEqual(struct.unpack('<Q', self.u.mem_read(self.state+0x30, 8))[0], 160)

    def test_cinematics_remain_thirty_and_release_the_cap(self):
        for _ in range(60): self.assertEqual(self.step(1, False), (1.0, 1))
        self.assertEqual(self.read32(self.state+4), 1)
        self.assertEqual(self.read32(self.state+0x24), 2)
        self.assertEqual(self.step(1), (0.5, 0))
        self.assertEqual(self.read32(self.state+4), 0)
        self.assertEqual(self.read32(self.state+0x24), 0)

    def test_initial_step_follows_target(self):
        self.assertEqual(self.step(0), (0.5, 0))
        self.u.mem_write(self.state, struct.pack('<f', 30))
        self.assertEqual(self.step(0)[0], 1.0)

    def test_first_sample_enters_clock_only_without_previous_timestamp(self):
        self.u.reg_write(arm.UC_ARM64_REG_W21, 1)
        self.u.reg_write(arm.UC_ARM64_REG_X8, 0)
        self.execute(self.base+0x91dbdc, self.base+0x91dcb4, count=500)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), self.base+0x91dcb4)
        self.assertEqual(struct.unpack('<f', self.u.mem_read(self.obj+0xd0, 4))[0], 0.5)
        self.u.reg_write(arm.UC_ARM64_REG_X8, 123)
        self.execute(self.base+0x91dbdc, self.base+0x91dbe0, count=100)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), self.base+0x91dbe0)
        self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X8), 123)

    def test_second_integer_consumer_skips_zero_and_accepts_whole_ticks(self):
        for ticks in (0, 1, 2):
            self.u.mem_write(self.obj+0xd8, struct.pack('<I', ticks))
            self.execute(self.base+0x57e7dc, self.base+0x57e7e0, count=100)
            self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_W8), ticks)
            end = self.base + (0x57e810 if ticks == 0 else 0x57e7e4)
            self.execute(self.base+0x57e7e0, end, count=100)
            self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), end)

    def test_input_window_decrements_once_per_thirty_hz_tick(self):
        remaining = 15
        for frame in range(30):
            self.step(1)
            self.u.reg_write(arm.UC_ARM64_REG_W8, remaining)
            self.execute(self.base+0x308a9c, self.base+0x308aa0, count=100)
            remaining = self.u.reg_read(arm.UC_ARM64_REG_W8)
            self.assertEqual(remaining, 15-(frame+1)//2)
            self.assertEqual(bool(self.u.reg_read(arm.UC_ARM64_REG_NZCV) & (1 << 30)), remaining == 0)
        self.assertEqual(remaining, 0)

    def test_disabled_hook_preserves_registers_and_original_flags(self):
        self.u.mem_write(self.state, bytes(4))
        for count in (0, 1, 15):
            self.u.reg_write(arm.UC_ARM64_REG_W8, count)
            self.u.reg_write(arm.UC_ARM64_REG_X16, 0x12345678)
            self.u.reg_write(arm.UC_ARM64_REG_X17, 0x87654321)
            self.execute(self.base+0x308a9c, self.base+0x308aa0, count=100)
            self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_W8), (count-1) & 0xffffffff)
            self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X16), 0x12345678)
            self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X17), 0x87654321)
            self.assertEqual(bool(self.u.reg_read(arm.UC_ARM64_REG_NZCV) & (1 << 30)), count == 1)

    def test_camera_blend_has_same_decay_after_equal_elapsed_time(self):
        def fbits(f): return struct.unpack('<I', struct.pack('<f', f))[0]
        def getf(reg): return struct.unpack('<f', struct.pack('<I', self.u.reg_read(reg)))[0]
        def powf(u, address, size, data):
            u.reg_write(arm.UC_ARM64_REG_S0, fbits(math.pow(getf(arm.UC_ARM64_REG_S0), getf(arm.UC_ARM64_REG_S1))))
            u.reg_write(arm.UC_ARM64_REG_PC, u.reg_read(arm.UC_ARM64_REG_LR))
        self.u.hook_add(uc.UC_HOOK_CODE, powf, begin=self.base+0xa0d610, end=self.base+0xa0d610)
        decay = []
        for vsyncs, frames in ((1, 60), (2, 30)):
            remaining = 1.0
            for _ in range(frames):
                self.step(vsyncs)
                for reg, value in ((arm.UC_ARM64_REG_S0, 0.1), (arm.UC_ARM64_REG_S10, 1.0)):
                    self.u.reg_write(reg, fbits(value))
                for reg, value in ((arm.UC_ARM64_REG_X0, 101), (arm.UC_ARM64_REG_X1, 102), (arm.UC_ARM64_REG_X2, 103)):
                    self.u.reg_write(reg, value)
                self.execute(self.base+0x3a2d40, self.base+0x3a2d44, count=200)
                self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_PC), self.base+0x3a2d44)
                self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X0), 101)
                self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X1), 102)
                self.assertEqual(self.u.reg_read(arm.UC_ARM64_REG_X2), 103)
                remaining *= 1-getf(arm.UC_ARM64_REG_S8)
            decay.append(remaining)
        self.assertAlmostEqual(decay[0], decay[1], places=6)


if __name__ == '__main__': unittest.main()
