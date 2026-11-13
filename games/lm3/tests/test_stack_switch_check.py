import struct
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/d2r'))
from stack_switch_check import saved_stack_switch_registers


class StackSwitchGuard(unittest.TestCase):
    # Synthetic mov-sp trampoline, with key page 0x4000 and slot index 3.
    words = (0xa93f07e0, 0xd53bd061, 0xf0000000, 0xf9400c00, 0x8b000021,
             0xf9400020, 0x910003e1, 0x9100001f, 0xa97f0420, 0x17fffcb7)

    def check(self, words=None, **kwargs):
        return saved_stack_switch_registers(
            kwargs.get('original', 0x9100025f), struct.pack('<10I', *(words or self.words)),
            0x1000, kwargs.get('return_pc', 0x300), kwargs.get('key_page', 0x4000), kwargs.get('key_index', 3))

    def test_saved_old_sp_path(self):
        self.assertEqual(self.check(), {'x0', 'x1'})

    def test_every_instruction_bit_is_guarded(self):
        for index in range(10):
            for bit in range(32):
                words = list(self.words)
                words[index] ^= 1 << bit
                self.assertFalse(self.check(words), (index, bit))

    def test_original_slot_page_and_return_identity(self):
        for kwargs in ({'original': 0x9100023f}, {'key_index': 4}, {'key_page': 0x5000}, {'return_pc': 0x304}):
            self.assertFalse(self.check(**kwargs))


if __name__ == '__main__':
    unittest.main()
