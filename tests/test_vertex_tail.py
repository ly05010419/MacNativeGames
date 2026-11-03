"""Unused vertex lanes can be omitted without changing the data's stride."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VertexTail(unittest.TestCase):
    def test_lane_usage_and_format_bounds(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'vertex-tail'
            build = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                 '-fno-sanitize-recover=all', str(ROOT / 'tests/vertex_tail_probe.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('vertex tail checks passed', run.stdout)

    @unittest.skipUnless(sys.platform == 'darwin', 'Metal requires macOS')
    def test_gpu_output_matches(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'vertex-tail-gpu'
            build = subprocess.run(
                [compiler, '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 str(ROOT / 'tests/vertex_tail_gpu_probe.m'), '-framework', 'Foundation', '-framework', 'Metal',
                 '-o', str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            if run.returncode == 77:
                self.skipTest('no Metal device available in this session')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('vertex fetch GPU output matches', run.stdout)
