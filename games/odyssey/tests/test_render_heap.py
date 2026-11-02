"""Render target growth must not take memory away from the game's root arena."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "macOS runtime")
class RenderHeap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="odyssey-render-heap-")
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / "probe.c"
        cls.exe = source.with_suffix("")
        source.write_text(r'''
#include <assert.h>
#include <setjmp.h>
#include "game_patches.c"
static jmp_buf failed;
void runtime_fail(const char *s) { (void)s; longjmp(failed, 1); }
void odyssey_aspect_install(const Module *m) { (void)m; }
void odyssey_aspect_present(void) {}
void *runtime_hle_wrap_named(void *p, const char *s) { (void)s; return p; }
int mfxup_enabled(void) { return 0; }
int dm_gfx_opt_get(int option) { (void)option; return 0; }
void dm_gfx_set_flag(const char *s, int v) { (void)s; (void)v; }
static const unsigned offsets[] = {0x624b4c, 0x624b7c, 0x4e321c, 0x4bf714};
static const uint32_t original[] = {0x52a15400, 0x52a0c808, 0x52b86000, 0x52b81808};
static uint32_t word(Module *m, unsigned i) { uint32_t w; memcpy(&w, m->base + offsets[i], 4); return w; }
int main(int argc, char **argv) {
    assert(argc == 3);
    Module m = {.base = calloc(1, 0x625000)}; assert(m.base);
    for (unsigned i = 0; i < 4; i++) memcpy(m.base + offsets[i], &original[i], 4);
    setenv("TOTK_RENDER_RES", argv[1], 1);
    int corrupt = !strcmp(argv[2], "corrupt");
    if (corrupt) m.base[offsets[3]] ^= 1;
    if (setjmp(failed)) {
        assert(corrupt);
        for (unsigned i = 0; i < 3; i++) assert(word(&m, i) == original[i]);
        free(m.base); return 0;
    }
    patch_rt_pool(&m);
    assert(!corrupt);
    uint64_t extra = strtoull(argv[2], NULL, 0) << 20;
    assert(runtime_render_pool_extra() == extra);
    /* Decode MOVZ W, LSL #16: the arena, OS heap and render heap grow together.
       In particular, the non-render portion of the root arena must be unchanged. */
    for (unsigned i = 0; i < 4; i++) {
        uint32_t w = word(&m, i);
        assert((w & 0xffe0001f) == (original[i] & 0xffe0001f));
        assert((uint64_t)((w >> 5) & 0xffff) * 65536 ==
               (uint64_t)((original[i] >> 5) & 0xffff) * 65536 + extra);
    }
    free(m.base); return 0;
}
''')
        subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(GAME / "runtime"),
                        "-I" + str(GAME / "third_party/d2r_gfx/gfx/nvn/include"),
                        str(source), "-framework", "CoreFoundation", "-o", str(cls.exe)],
                       check=True, capture_output=True, text=True)

    def test_capacity_preserved_at_each_launcher_resolution(self):
        for size, extra in [("1600x900", "0"), ("1920x1080", "0"),
                            ("2240x1260", "28"), ("2560x1440", "60")]:
            with self.subTest(size=size):
                result = subprocess.run([str(self.exe), size, extra], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_arena_instruction_rejected_before_any_heap_patch(self):
        result = subprocess.run([str(self.exe), "2560x1440", "corrupt"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
