"""Exercise production GPU diagnostics without submitting a hanging GPU workload."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin', 'Apple Metal/Foundation SDK')
class GpuError(unittest.TestCase):
    def test_error_domains_and_delayed_feedback(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = Path(folder) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=undefined', f'-I{NVN}', str(ROOT / 'tests/gpu_error_probe.m'),
                                    '-framework', 'Metal', '-framework', 'Foundation', '-o', str(exe)],
                                   capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr)
            # Match the launcher's debug-off mode: stderr is discarded.
            run = subprocess.run([str(exe), str(Path(folder) / 'gpu-error.log')], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout)
            self.assertIn('gpu-error: 23 checks passed', run.stdout)

    def test_failure_is_saved_before_waiters_or_guard_can_abort(self):
        source = (NVN / 'nvn_mtl.m').read_text()
        for callback in ('if (f.error) {\n            gpu_error_report', 'if (cb.error) {\n            gpu_error_report'):
            start = source.index(callback)
            body = source[start:source.index('\n        }', start)]
            self.assertLess(body.index('gpu_error_report'), body.index('atomic_store(&g_last_commit_error'))
            if 'gpu_hang_guard' in body:
                self.assertLess(body.index('gpu_error_report'), body.index('gpu_hang_guard'))
        self.assertIn('gpu_trace_pipeline(p.label);', source)
        self.assertIn('setenv("OOT_GPU_ERROR_LOG"', (ROOT / 'games/oot/app/launcher.m').read_text())


if __name__ == '__main__':
    unittest.main()
