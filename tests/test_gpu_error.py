"""Exercise production GPU diagnostics without submitting a hanging GPU workload."""
from pathlib import Path
import os
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

    def test_bind_trace_is_on_for_ocarina_only_unless_asked(self):
        # Each recorded bind reads a label and retains it on the replay thread; the other games skip it by default.
        with tempfile.TemporaryDirectory() as folder:
            for oot in (False, True):
                exe = Path(folder) / ('probe-oot' if oot else 'probe')
                build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                        *(['-DGAME_OOT=1'] if oot else []), f'-I{NVN}', str(ROOT / 'tests/gpu_error_probe.m'),
                                        '-framework', 'Metal', '-framework', 'Foundation', '-o', str(exe)],
                                       capture_output=True, text=True, timeout=120)
                self.assertEqual(build.returncode, 0, build.stderr)
                for value, expected in ((None, oot), ('1', True), ('0', False)):
                    env = {k: v for k, v in os.environ.items() if k != 'TOTK_GPU_TRACE'}
                    if value is not None:
                        env['TOTK_GPU_TRACE'] = value
                    run = subprocess.run([str(exe), '--trace-on'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                         text=True, timeout=30, env=env)
                    self.assertEqual(run.stdout.strip(), f'trace={int(expected)} switched={int(not expected)}', (oot, value))
        source = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('if (gpu_trace_on()) gpu_trace_pipeline(p.label);', source)
        self.assertIn('if (!strcmp(name, "gpu_trace")) { gpu_trace_set(v);', source)

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

    def test_the_first_hang_reports_the_mesh_stage_memory_before_the_game_stops(self):
        # M1 / M2 hangs (#2036 #2040 #2103 #2108 #2112 #2239) are in passes drawing mesh 3439aaa0, whose stage memory is
        # claimed in a spin loop: the slots at the hang tell whether that claim is the cause (2026-11-18)
        source = (NVN / 'nvn_mtl.m').read_text()
        guard = source[source.index('static void gpu_hang_guard('):]
        guard = guard[:guard.index('\n}\n')]
        self.assertLess(guard.index('if (n == 1) mtk_scratch_hang_report();'), guard.index('if (n >= 2) MDIE('))
        tasks = (NVN / 'nvn_mesh_tasks.inc').read_text()
        report = tasks[tasks.index('static void mtk_scratch_hang_report(void) {'):]
        report = report[:report.index('\n}\n')]
        for words in ('slots held', 'forced takes', 'MTK_SCRATCH_FORCED', 'mtk_split_on()'):
            self.assertIn(words, report)
        # PR #2258: mesh draws in passes of their own by default only before Apple9 (M1 / M2); OOT_MESH_PASS_SPLIT forces it
        self.assertIn('v=e&&*e?atoi(e):[g_dev supportsFamily:MTLGPUFamilyApple9]?0:2;', tasks)
        # mode 2 (only where mesh and ordinary draws meet) is the M1 / M2 default; an unknown value falls back to mode 1
        self.assertIn('if(v<0||v>2)v=1;', tasks)

    def test_mode_2_counts_every_ordinary_draw(self):
        # PR #2324 review: mid-pass clears draw a triangle through rp_begin, outside draw_inner; mode 2 must see them as
        # ordinary draws too, or a clear after mesh draws would stay in their pass
        source = (NVN / 'nvn_mtl.m').read_text()
        for name in ('static void clear_quad(', 'static void clear_ds_quad(', 'static void draw_inner(const p_draw *dw, const p_draw_idx *di) {'):
            with self.subTest(function=name):
                body = source[source.index(name):]
                body = body[:body.index('rp_begin()') if 'clear' in name else body.index('\n}\n')]
                self.assertIn('mtk_pass_plain_draw();', body)


if __name__ == '__main__':
    unittest.main()
