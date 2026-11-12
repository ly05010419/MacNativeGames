"""Ocarina of Time's per-frame GPU accounting, run (not read): the production bodies of gpu_tag_begin / gpu_tag_done /
mtl_gpu_ms / mtl_gpu_span and of sysload_busy_add are spliced into tests/oot_gpu_span_probe.c and driven with synthetic
command-buffer completions, including the captured pair with no start time (GPUStartTime 0.000000000, GPUEndTime
588279.960720750). The negative controls put each guard back the way it was and require the same probe to fail."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / "games/oot/third_party/d2r_gfx/gfx/nvn"
GUARD = "    int usable = d > 0 && g0 > 0;\n"
NOTE = "    if (usable) sysload_note_gpu(g0, g1);"


def _block(source, start, end="\n}\n"):
    at = source.index(start)
    return source[at:source.index(end, at) + len(end)]


def probe_source(mtl, sysload):
    parts = {
        "/* SYSLOAD_ADD */": _block(sysload, "double sysload_busy_add(sysload_busy *b,"),
        "/* RING */": _block(mtl, "#define GRING ", "g_gpu[GRING];"),
        "/* COUNTERS */": _block(mtl, "static _Atomic uint64_t g_cbw_committed,", ";\n"),
        "/* TAG_BEGIN */": _block(mtl, "static void gpu_tag_begin(uint64_t f) {"),
        "/* TAG_DONE */": _block(mtl, "static void gpu_tag_done(uint64_t f, double g0, double g1) {"),
        "/* READERS */": _block(mtl, "double mtl_gpu_ms(uint64_t f) {", "    return *t1 >= *t0 && *t1;\n}\n"),
    }
    fixture = (ROOT / "tests/oot_gpu_span_probe.c").read_text()
    for marker, value in parts.items():
        assert fixture.count(marker) == 1, marker
        assert value.strip(), marker
        fixture = fixture.replace(marker, value)
    return fixture


@unittest.skipUnless(sys.platform == "darwin", "clock_gettime_nsec_np and the Apple toolchain")
class GpuSpan(unittest.TestCase):
    def setUp(self):
        self.mtl = (NVN / "nvn_mtl.m").read_text(errors="replace")
        self.sysload = (NVN / "totk_sysload.c").read_text(errors="replace")

    def run_probe(self, mtl, expected_failure=None):
        with tempfile.TemporaryDirectory() as folder:
            path, binary = Path(folder) / "probe.c", Path(folder) / "probe"
            path.write_text(probe_source(mtl, self.sysload))
            build = subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-fno-sanitize-recover=all",
                str(path), "-o", str(binary),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
            if expected_failure is None:
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("6 cases passed", run.stdout)
            else:
                self.assertNotEqual(run.returncode, 0, "the old rule unexpectedly passed")
                self.assertIn(expected_failure, run.stderr, run.stdout + run.stderr)

    def test_production_accounting(self):
        self.run_probe(self.mtl)

    def test_without_the_guard_a_zero_start_is_charged_to_the_frame(self):
        self.assertEqual(self.mtl.count(GUARD), 1)
        self.run_probe(self.mtl.replace(GUARD, "    int usable = d > 0;\n"),
                       "a zero start adds no GPU time")

    def test_without_the_guard_a_zero_start_reaches_the_load_line(self):
        self.assertEqual(self.mtl.count(NOTE), 1)
        self.run_probe(self.mtl.replace(NOTE, "    sysload_note_gpu(g0, g1);"),
                       "a zero start adds no busy time after a gap")


if __name__ == "__main__":
    unittest.main()
