"""Guard failures are transactional; execute the generated AArch64 size code.

Uses the player's existing verified 1.4.0 extraction/rewrite, never writes to it.
The small host test library is compiled in a temporary directory.
"""
import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
import unicorn.arm64_const as arm
from unicorn.arm64_const import (
    UC_ARM64_REG_X8, UC_ARM64_REG_X10, UC_ARM64_REG_X11,
    UC_ARM64_REG_NZCV, UC_ARM64_REG_S0, UC_ARM64_REG_S1,
)

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x100000000
DATA = 0x200000000


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


class LM3RenderPatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ROOT / 'extracted/v140/exefs/main'
        rewritten = ROOT / 'rewritten/v140/main.patched.bin'
        if sys.platform != 'darwin' or not source.exists() or not rewritten.exists():
            raise unittest.SkipTest('requires macOS and the existing verified 1.4.0 input')
        cls.original = rewritten.read_bytes()
        cls.bid = source.read_bytes()[0x40:0x60]
        cls.temp = tempfile.TemporaryDirectory(prefix='lm3-render-patch-')
        library = Path(cls.temp.name) / 'patch.dylib'
        subprocess.run([
            'xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-DLM3_RENDER_PATCH_TEST=1', '-dynamiclib',
            str(ROOT / 'runtime/lm3_render_patch.c'), '-o', str(library),
        ], check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.apply = cls.lib.lm3_render_apply_image
        cls.apply.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                              ctypes.c_uint, ctypes.c_uint,
                              ctypes.c_char_p, ctypes.c_size_t]
        cls.apply.restype = ctypes.c_int
        cls.extra = cls.lib.lm3_render_extra_bytes_for_size
        cls.extra.argtypes = [ctypes.c_uint, ctypes.c_uint]
        cls.extra.restype = ctypes.c_uint64

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'temp'):
            cls.temp.cleanup()

    def patch(self, payload=None, bid=None, size=(2560, 1440)):
        payload = self.original if payload is None else payload
        image = ctypes.create_string_buffer(payload)
        error = ctypes.create_string_buffer(192)
        result = self.apply(image, len(payload), self.bid if bid is None else bid,
                            *size, error, len(error))
        return result, image.raw[:len(payload)], error.value.decode()

    def emulator(self, patched):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(BASE, (len(patched) + 0xfff) & ~0xfff)
        uc.mem_write(BASE, patched)
        uc.mem_map(DATA, 0x100000)
        return uc

    def test_all_sizes_execute_own_initialization_and_drs(self):
        for width, height in [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440)]:
            with self.subTest(size=(width, height)):
                ok, image, error = self.patch(size=(width, height))
                self.assertEqual((ok, error), (1, ''))
                uc = self.emulator(image)
                uc.reg_write(UC_ARM64_REG_X10, 0xdeadbeefdeadbeef)
                uc.reg_write(UC_ARM64_REG_X11, 0xfacefacefaceface)
                uc.reg_write(UC_ARM64_REG_NZCV, 0xa0000000)
                uc.emu_start(BASE + 0x78e174, BASE + 0x78e17c, count=16)
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X10), width | (width << 32))
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X11), height)
                self.assertEqual(uc.reg_read(UC_ARM64_REG_NZCV), 0xa0000000)
                uc.emu_start(BASE + 0x78e188, BASE + 0x78e18c, count=1)
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X11), height | (height << 32))

                # The game's original aspect computation feeds s0; the selected
                # integer height must convert to float without loading 1080.
                uc.reg_write(UC_ARM64_REG_X11, DATA)
                uc.reg_write(UC_ARM64_REG_S0, bits(16 / 9))
                uc.emu_start(BASE + 0x78c2a0, BASE + 0x78c2b4, count=8)
                self.assertEqual(struct.unpack('<I', uc.mem_read(DATA, 4))[0], height)
                self.assertEqual(uc.reg_read(UC_ARM64_REG_S1), bits(height))
                self.assertAlmostEqual(struct.unpack('<f', struct.pack('<I', uc.reg_read(UC_ARM64_REG_S0)))[0], width, places=2)
                uc.reg_write(UC_ARM64_REG_X8, DATA)
                uc.reg_write(UC_ARM64_REG_S0, bits(16 / 9))
                uc.emu_start(BASE + 0x78c304, BASE + 0x78c320, count=8)
                self.assertEqual(struct.unpack('<I', uc.mem_read(DATA, 4))[0], height)
                self.assertEqual(struct.unpack('<I', uc.mem_read(DATA + 8, 4))[0], width)

                # Only the DRS result is fixed; its original store writes 1.0.
                uc.reg_write(UC_ARM64_REG_X8, DATA)
                uc.emu_start(BASE + 0x45df48, BASE + 0x45df58, count=4)
                self.assertEqual(struct.unpack('<f', uc.mem_read(DATA + 0xc88, 4))[0], 1.0)
                self.assertEqual(image[0x13c94d8:0x13c94dc], self.original[0x13c94d8:0x13c94dc])
                self.assertEqual(image[0x13c95a8:0x13c95ac], self.original[0x13c95a8:0x13c95ac])

    def test_fixed_scale_creates_only_one_descriptor_negative_control(self):
        ok, image, error = self.patch()
        self.assertEqual((ok, error), (1, ''))

        def descriptor_count(payload):
            uc = self.emulator(payload)
            uc.reg_write(arm.UC_ARM64_REG_SP, DATA + 0xf0000)
            uc.reg_write(arm.UC_ARM64_REG_LR, DATA + 0xff000)
            uc.reg_write(arm.UC_ARM64_REG_X0, DATA)
            uc.reg_write(arm.UC_ARM64_REG_X1, DATA + 0x2000)
            uc.mem_write(DATA + 0x28, struct.pack('<Q', DATA + 0x3000))
            uc.mem_write(DATA + 0x40, struct.pack('<Q', DATA + 0x4000))
            calls = []
            # Model only the external NVN accessors/initializer; run the actual
            # guest counter, address arithmetic, loop and epilogue in Unicorn.
            getters = {0xb81b64: DATA + 0x5000, 0xb81b78: 0,
                       0xb81bdc: 2560, 0xb81bf0: 1440}
            no_result = {0xb81b90, 0xb81c40, 0xb81ca0, 0xb81cb0, 0xb81cd4}

            def external(emu, address, size, _):
                at = address - BASE
                if at in getters:
                    emu.reg_write(arm.UC_ARM64_REG_X0, getters[at])
                elif at in no_result:
                    if at in (0xb81c40, 0xb81cd4):
                        calls.append(emu.reg_read(arm.UC_ARM64_REG_X0))
                else:
                    return
                emu.reg_write(arm.UC_ARM64_REG_PC, address + 4)

            uc.hook_add(UC_HOOK_CODE, external)
            uc.emu_start(BASE + 0xb81af0, DATA + 0xff000, count=30000)
            self.assertEqual(uc.reg_read(arm.UC_ARM64_REG_SP), DATA + 0xf0000)
            count = struct.unpack('<I', uc.mem_read(DATA + 0x4000, 4))[0]
            self.assertEqual(count, len(calls))
            return count

        self.assertEqual(descriptor_count(image), 1)
        negative = bytearray(image)
        negative[0xb81c44:0xb81c48] = self.original[0xb81c44:0xb81c48]
        self.assertGreater(descriptor_count(bytes(negative)), 600)

    def test_1440p_gpu_pool_heap_and_available_memory_guard_move_together(self):
        for size in [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440)]:
            with self.subTest(size=size):
                extra = 0x20000000 if size == (2560, 1440) else 0
                self.assertEqual(self.extra(*size), extra)
                ok, image, error = self.patch(size=size)
                self.assertEqual((ok, error), (1, ''))
                uc = self.emulator(image)
                uc.reg_write(arm.UC_ARM64_REG_X25, DATA)
                uc.reg_write(arm.UC_ARM64_REG_X8, 0x1fb8d000)
                uc.emu_start(BASE + 0x78c374, BASE + 0x78c380, count=3)
                self.assertEqual(struct.unpack('<QQ', uc.mem_read(DATA + 0x28, 16)),
                                 (0x1fb8d000, 0x3f3aa000 + extra))
                uc.emu_start(BASE + 0xb5f698, BASE + 0xb5f69c, count=1)
                heap = uc.reg_read(arm.UC_ARM64_REG_X20)
                self.assertEqual(heap, 0xbbe00000 + extra)
                self.assertLess(heap, 0x100000000)
                # Existing strict-greater predicate must reject one 2MiB unit
                # below the requested heap, and accept exactly enough units.
                for available, expected_pc in [(heap // 0x200000 - 1, 0xb5f6e0),
                                                (heap // 0x200000, 0xb5f6e4)]:
                    uc.reg_write(arm.UC_ARM64_REG_X8, available)
                    uc.emu_start(BASE + 0xb5f6d8, BASE + 0xb5f6e4, count=2)
                    self.assertEqual(uc.reg_read(arm.UC_ARM64_REG_PC), BASE + expected_pc)
                if not extra:
                    for at in (0x78c378, 0xb5f698, 0xb5f6d8):
                        self.assertEqual(image[at:at + 4], self.original[at:at + 4])

    def test_every_build_id_byte_is_checked_before_writes(self):
        for i in range(32):
            bid = bytearray(self.bid)
            bid[i] ^= 1
            ok, image, error = self.patch(bid=bytes(bid))
            self.assertEqual(ok, 0)
            self.assertIn('build', error)
            self.assertEqual(hashlib.sha256(image).digest(), hashlib.sha256(self.original).digest())

    def test_three_window_factory_inputs_before_storage_allocation(self):
        ok, image, error = self.patch()
        self.assertEqual((ok, error), (1, ''))

        def construct(payload):
            uc = self.emulator(payload)
            uc.reg_write(arm.UC_ARM64_REG_X19, DATA + 0x9000)
            uc.reg_write(arm.UC_ARM64_REG_SP, DATA + 0xf0000)
            # Execute the actual default-config initializer, including its
            # four packed width/height stores, before the unmodified factory.
            uc.emu_start(BASE + 0x78e168, BASE + 0x78e1c0, count=40)
            uc.reg_write(arm.UC_ARM64_REG_X25, BASE + 0x1bd2ec0)
            uc.reg_write(arm.UC_ARM64_REG_X21, BASE + 0x1cea8e0)
            uc.reg_write(arm.UC_ARM64_REG_W20, 0x48)
            uc.mem_write(BASE + 0x1cea950, struct.pack('<Q', DATA + 0x8000))
            uc.mem_write(DATA + 0x8010, struct.pack('<Q', DATA + 0x9000))
            uc.mem_write(DATA + 0x9000, struct.pack('<Q', DATA + 0xa000))
            objects, formats, resources = [], {}, []

            def external(emu, address, size, _):
                at = address - BASE
                if at in (0x78d838, 0x78d8ac, 0x78d914, 0x78d9ec):
                    # Native host allocator only; resource descriptor writes
                    # between these calls execute unchanged in the emulator.
                    pointer = DATA + 0x10000 + len(objects) * 0x1000
                    objects.append(pointer)
                    emu.reg_write(arm.UC_ARM64_REG_X0, pointer)
                elif at == 0x78da00:
                    pointer = emu.reg_read(arm.UC_ARM64_REG_X0)
                    header = emu.reg_read(arm.UC_ARM64_REG_X2)
                    formats[pointer] = struct.unpack('<HH', emu.mem_read(header + 4, 4))
                elif at in (0x78da44, 0x78da64, 0x78da7c):
                    argument = emu.reg_read(arm.UC_ARM64_REG_X1)
                    colour = struct.unpack('<Q', emu.mem_read(argument, 8))[0]
                    format_pointer = struct.unpack('<Q', emu.mem_read(argument + 0x38, 8))[0]
                    resources.append((struct.unpack('<HH', emu.mem_read(colour + 0xc, 4)),
                                      formats[format_pointer]))
                    emu.reg_write(arm.UC_ARM64_REG_X0, DATA + 0x30000 + len(resources) * 0x1000)
                else:
                    return
                emu.reg_write(arm.UC_ARM64_REG_PC, address + 4)

            uc.hook_add(UC_HOOK_CODE, external)
            uc.emu_start(BASE + 0x78d80c, BASE + 0x78da84, count=300)
            return resources

        self.assertEqual(construct(image), [((2560, 1440), (2560, 1440))] * 3)
        negative = bytearray(image)
        for at in (0x78e174, 0x78e178, 0x78e188):
            negative[at:at + 4] = self.original[at:at + 4]
        self.assertEqual(construct(bytes(negative)), [((1920, 1080), (1920, 1080))] * 3)

    def test_original_window_lifecycle_and_wait_order_are_unchanged(self):
        ok, image, error = self.patch()
        self.assertEqual((ok, error), (1, ''))
        for start, end in [(0xb80660, 0xb807e0), (0x4585cc, 0x4585f0),
                           (0x45d5a0, 0x45d5c8), (0x7c1a30, 0x7c1a80)]:
            self.assertEqual(image[start:end], self.original[start:end])

    def test_instruction_and_cave_conflicts_leave_image_unchanged(self):
        # Separate startup, operation-mode, conversion, final DRS store, size
        # callback, and code-cave conflicts exercise both patch and guard sites.
        for offset in [0x458568, 0xb7a924, 0x78c2a8, 0x78c310,
                       0x45df48, 0x45df54, 0xb81c44, 0xb81ce4,
                       0x78e174, 0x78e1a8, 0x78d980, 0x78da64,
                       0x78c378, 0xb5f698, 0xb5f6d8,
                       0x458614, 0x1045fa4]:
            with self.subTest(offset=hex(offset)):
                payload = bytearray(self.original)
                payload[offset] ^= 1
                ok, image, error = self.patch(payload=bytes(payload))
                self.assertEqual(ok, 0)
                self.assertIn('main+0x', error)
                self.assertEqual(hashlib.sha256(image).digest(), hashlib.sha256(payload).digest())

    def test_invalid_sizes_and_truncated_image_are_rejected(self):
        for size in [(0, 0), (2560, 1080), (3840, 2160), (1921, 1080)]:
            ok, image, error = self.patch(size=size)
            self.assertEqual(ok, 0)
            self.assertIn('size', error)
            self.assertEqual(hashlib.sha256(image).digest(), hashlib.sha256(self.original).digest())
        ok, image, error = self.patch(payload=self.original[:0x1045fa4])
        self.assertEqual(ok, 0)
        self.assertIn('small', error)
        self.assertEqual(image, self.original[:0x1045fa4])


if __name__ == '__main__':
    unittest.main()
