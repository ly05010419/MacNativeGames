"""CPU regressions for Odyssey's post-merge reports on PR #256.

Run with Xcode selected: python -m unittest discover -s games/odyssey/tests.
No game files, save data, translator process or audio device are required.
"""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
NVN = GAME / "third_party/d2r_gfx/gfx/nvn"


@unittest.skipUnless(sys.platform == "darwin" and platform.machine() == "arm64", "Apple ARM64 runtime")
class OdysseyRegressions(unittest.TestCase):
    def run_probe(self, sources, flags=(), arguments=((),)):
        with tempfile.TemporaryDirectory(prefix="odyssey-regression-") as folder:
            binary = Path(folder) / "probe"
            build = subprocess.run([
                "xcrun", "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                "-I" + str(GAME / "runtime"), "-I" + str(NVN / "include"),
                "-Wl,-dead_strip", *flags, *map(str, sources), "-o", str(binary),
            ], capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            for args in arguments:
                # Each scheduling scenario runs in a fresh process, including its watcher.
                result = subprocess.run([str(binary), *args],
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_audio_routing_and_stall(self):
        self.run_probe([GAME / "tests/audio_routing/probe.c", GAME / "runtime/audio_effects.c",
                        GAME / "runtime/audio_reverb.c"],
                       ["-Werror", "-fsanitize=undefined", "-fno-sanitize-recover=all", "-framework", "AudioToolbox"])

    def test_no_svc_preemption_and_host_guard(self):
        self.run_probe([GAME / "tests/core_preempt/probe.c", GAME / "tests/core_preempt/spin.S"],
                       ["-Werror"], [("priority",), ("starve",), ("host",)])

    def test_shader_cache_and_uniform_reflection(self):
        with tempfile.TemporaryDirectory(prefix="odyssey-shaders-") as folder:
            self.run_probe([GAME / "tests/shader_regressions/probe.m", NVN / "nvn_refl.m"],
                           ["-fobjc-arc", "-framework", "Foundation", "-framework", "Metal"], [(folder,)])

    def test_uniform_handle_replay_reads(self):
        # Compile the actual replay helper with a tiny pending-write reader. This keeps
        # the probe CPU-only while testing the same bounds and byte offsets as Metal.
        source = (NVN / "nvn_mtl.m").read_text()
        start = source.index("static uint32_t ubo_tex_handle(")
        end = source.index("\n}", start) + 2
        helper = source[start:end]
        probe = r'''
#include "nvn_refl.h"
#include <assert.h>
#include <string.h>
typedef struct { uint64_t addr, size; } p_bind_buf;
static struct { p_bind_buf ubo[6][16]; } S;
static uint32_t pending;
static void pend_materialize(uint64_t addr, uint8_t *out, size_t n, int depth) {
    (void)depth; memcpy(out, (void *)(uintptr_t)addr, n);
    if (pending) memcpy(out, &pending, n);
}
HELPER
int main(void) {
    uint32_t words[8] = {0}; words[3] = 7 | (9 << 20);
    S.ubo[1][0] = (p_bind_buf){(uintptr_t)words, sizeof words};
    nvn_rbind b = {.kind=NVN_RB_TEXU, .reg=0, .off=12};
    assert(ubo_tex_handle(1, &b) == words[3]);
    pending = 11 | (13 << 20); assert(ubo_tex_handle(1, &b) == pending);
    pending = 17 | (19 << 20); assert(ubo_tex_handle(1, &b) == pending);
    b.off = sizeof words - 3; assert(ubo_tex_handle(1, &b) == 0);
    b.off = 0; b.reg = 16; assert(ubo_tex_handle(1, &b) == 0);
    b.reg = 0; assert(ubo_tex_handle(-1, &b) == 0 && ubo_tex_handle(6, &b) == 0);
    S.ubo[1][0].addr=0; assert(ubo_tex_handle(1, &b) == 0);
    return 0;
}
'''.replace("HELPER", helper)
        with tempfile.TemporaryDirectory(prefix="odyssey-handles-") as folder:
            path = Path(folder) / "probe.c"
            path.write_text(probe)
            self.run_probe([path], ["-I" + str(NVN), "-Werror", "-fsanitize=undefined", "-fno-sanitize-recover=all"])


if __name__ == "__main__":
    unittest.main()
