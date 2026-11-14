import subprocess
import tempfile
import unittest
from pathlib import Path


class SymbolProbeTests(unittest.TestCase):
    def test_symbol_binding(self):
        game = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / 'symbols-probe'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(game / 'runtime'), str(game / 'runtime/symbols.c'),
                            str(game / 'tests/symbols_probe.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
