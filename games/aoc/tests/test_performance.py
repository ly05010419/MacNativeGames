import pathlib
import subprocess
import tempfile
import unittest
GAME=pathlib.Path(__file__).resolve().parents[1]
class PerformanceProbe(unittest.TestCase):
    def test_telemetry_and_runtime_timing_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=pathlib.Path(tmp)/'performance-probe'
            subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                            str(GAME/'tests/performance_probe.c'),str(GAME/'runtime/gfx/nvn/aoc_stats.c'),
                            str(GAME/'runtime/gfx/nvn/aoc_timing.c'),'-lpthread','-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,capture_output=True)
