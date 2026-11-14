import subprocess
import tempfile
import unittest
from pathlib import Path


class LazyBridgeTests(unittest.TestCase):
    def test_native_integer_vector_and_stack_arguments(self):
        game = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'lazy-probe'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(game / 'runtime/lazy_bind.S'), str(game / 'tests/lazy_bridge_probe.S'),
                            str(game / 'tests/lazy_bridge_probe.c'), '-o', str(probe)], check=True)
            subprocess.run([str(probe)], check=True)


if __name__ == '__main__':
    unittest.main()
