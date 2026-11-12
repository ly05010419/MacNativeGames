import subprocess
import tempfile
import unittest
from pathlib import Path


class RelocationProbeTests(unittest.TestCase):
    def test_relocation_probe(self):
        game = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / 'relocate-probe'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I', str(game / 'runtime'), str(game / 'runtime/relocate.c'),
                            str(game / 'tests/relocate_probe.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
