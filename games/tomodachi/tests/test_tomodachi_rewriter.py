"""PC-relative addresses must survive moving an exclusive sequence to a trampoline."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "third_party/d2r/pc_relative.py"
spec = importlib.util.spec_from_file_location("pc_relative", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def encode(register, pages):
    return 0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5) | register


def target(word, pc):
    bits = f'{(word >> 5) & 0x7ffff:019b}{(word >> 29) & 3:02b}'
    pages = int(bits, 2) - ((1 << 21) if bits[0] == '1' else 0)
    return (pc // 4096 + pages) * 4096


class AdrpRelocation(unittest.TestCase):
    def test_addresses_and_destination_survive(self):
        for old, new in [(0x1e2a064, 0x5400030), (0x5400030, 0x1e2a064)]:
            for pages in [-1000, -1, 0, 1, 1000]:
                for register in [0, 15, 16, 17, 18, 30]:
                    word = encode(register, pages)
                    relocated = module.relocate_adrp(word, old, new)
                    self.assertEqual(target(word, old), target(relocated, new))
                    self.assertEqual(relocated & 31, register)

    def test_rejects_wrong_instruction_and_overflow(self):
        with self.assertRaises(ValueError):
            module.relocate_adrp(0xd503201f, 0, 4)
        with self.assertRaises(ValueError):
            module.relocate_adrp(encode(1, 0), 0, 1 << 34)


if __name__ == "__main__":
    unittest.main()
