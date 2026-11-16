import subprocess
import tempfile
import unittest
from pathlib import Path


class GateTests(unittest.TestCase):
    def test_dispatch(self):
        game = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'gate-probe'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-I', str(game / 'runtime'),
                            str(game / 'runtime/gates.c'), str(game / 'tests/gates_probe.c'),
                            '-o', str(probe)], check=True)
            subprocess.run([str(probe)], check=True)


if __name__ == '__main__':
    unittest.main()
