"""CPU attachment-boundary checks using production source; no Metal device or game data."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn"


def probe_source(source):
    def function(signature):
        start = source.rindex(signature)
        return source[start:source.index("\n}", start) + 2]

    declarations = source[source.index("typedef struct { uint64_t ser, bytes;"):
                          source.index("static unsigned g_sd_plain;")]
    registration = source[source.index("    g_nsd = 0; g_sd_plain = 0;"):
                          source.index("    memcpy(g_nt_pass_clear,")]
    collector = source[source.index("static int draw_att_reads(NvProg *pr,"):
                       source.index("    if (!pr || !nc) return 0;")]
    # Only append observation code: all candidate writes and their order remain production code.
    collector += "    memcpy(out, cand, nc * sizeof *cand); return nc;\n}\n"
    lookahead = source[source.index("typedef struct { uint32_t seq, rtseq;"):
                       source.index("static uint64_t g_pp_slot[")]
    parts = {
        "/* LEDGER */": declarations,
        "/* REGISTER */": registration,
        "/* NOTE_DRAW */": function("static void udc_note_draw(NvProg *pr) {"),
        "/* COLLECT */": collector,
        "/* LOOKAHEAD */": lookahead + "\n" + function("static int sdx_decide(uint8_t st["),
        "/* NST */": re.search(r"uint8_t nst\[[^]]+\];", source).group(),
    }
    fixture = (ROOT / "tests/attachment_capacity_probe.m").read_text()
    for marker, value in parts.items():
        fixture = fixture.replace(marker, value)
    return fixture


@unittest.skipUnless(sys.platform == "darwin", "Objective-C Foundation CPU probe")
class AttachmentCapacity(unittest.TestCase):
    def run_probe(self, source, expected_error=None):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "probe.m"
            binary = Path(folder) / "probe"
            path.write_text(probe_source(source))
            build = subprocess.run([
                "xcrun", "--sdk", "macosx", "clang", "-std=gnu11", "-O1", "-g", "-fobjc-arc",
                "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-variable",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(NVN), str(path), "-framework", "Foundation", "-o", str(binary),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=0" if expected_error else "halt_on_error=1")
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30, env=env)
            if expected_error:
                self.assertNotEqual(run.returncode, 0, "old capacity unexpectedly passed")
                self.assertIn(expected_error, run.stderr, run.stdout + run.stderr)
            else:
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("36 attachment shapes passed", run.stdout)

    def test_production_boundaries(self):
        self.run_probe((NVN / "nvn_mtl.m").read_text())

    def test_old_capacities_fail_independently(self):
        source = (NVN / "nvn_mtl.m").read_text()
        for old, new, error in (
            ("g_sd[SD_ATT_MAX]", "g_sd[9]", "global-buffer-overflow"),
            ("uint8_t nst[SD_ATT_MAX];", "uint8_t nst[9];", "stack-buffer-overflow"),
            ("cand[8 + 1 + SD_ATT_MAX]", "cand[18]", "stack-buffer-overflow"),
        ):
            with self.subTest(capacity=new):
                self.assertEqual(source.count(old), 1)
                self.run_probe(source.replace(old, new), error)


if __name__ == "__main__":
    unittest.main()
