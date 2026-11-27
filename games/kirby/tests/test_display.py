"""Exercise the startup resolution patcher without loading game data."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

RUNTIME = Path(__file__).resolve().parents[1] / "runtime"


class DisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        source = root / "probe.c"
        source.write_text(r'''
#include "game_patches.c"
void runtime_survey_init(Module *m) { (void)m; }
uint64_t dm_nvn_frame_count(void) { return 0; }
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "%s\n", s); exit(42); }
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "target")) {
        uint32_t source[10] = {1, 0, 1600, 900, 1, 1920, 1080, 1, 0, 0};
        if (argc > 2) { source[2]=source[5]=(unsigned)atoi(argv[2]); source[3]=source[6]=(unsigned)atoi(argv[3]); }
        const uint32_t *result = native_target_descriptor(source);
        printf("%u,%u,%u,%u|%u,%u,%u,%u\n", result[2],result[3],result[5],result[6],source[2],source[3],source[5],source[6]);
        return 0;
    }
    Module m = {.name="main", .size=0x900000, .text_end=TEXT_END, .ro_off=PADDING_END};
    m.base = mmap(NULL, m.size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (m.base == MAP_FAILED) return 2;
    put(&m, FPS_SITE, 0xf9402a68); put(&m, RES_SITE, 0x7100043f);
    put(&m, 0x745698, argc > 1 ? 0 : 0xd280f008); put(&m, 0x74569c, 0xf2c08708);
    put(&m, 0x745e0c, 0xd280f008); put(&m, 0x745e10, 0xf2c08708);
    put(&m, 0x745a5c, 0x52a2e008);
    put(&m, 0x8ac780, 1920); put(&m, 0x8ac784, 1080);
    put(&m, RT_CREATE, 0xb00018e8);
    runtime_game_patches(&m);
    uint32_t crop[2]; memcpy(crop,m.base+0x8ac780,sizeof crop);
    if (crop[0] != (g_canvas_1440 ? 2560 : 1920) || crop[1] != (g_canvas_1440 ? 1440 : 1080)) return 3;
    uint32_t width, height, heap;
    memcpy(&width, m.base+0x745698, 4); memcpy(&height, m.base+0x74569c, 4);
    memcpy(&heap, m.base+0x745a5c, 4);
    printf("%d|%llu|%llx|%08x|%08x|%08x\n", runtime_kirby_resolution(),
           (unsigned long long)runtime_output_size_current(),
           (unsigned long long)runtime_output_res_extra_bytes(), width, height, heap);
    return 0;
}
''')
        cls.probe = root / "probe"
        subprocess.run(["xcrun", "clang", "-std=gnu11", "-I", str(RUNTIME), str(source),
                        "-framework", "CoreFoundation", "-o", str(cls.probe)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, mode, *args):
        env = {k: v for k, v in os.environ.items() if not k.startswith("KIRBY_")}
        env.update(KIRBY_RES=mode, KIRBY_FPS="60")
        return subprocess.run([str(self.probe), *args], env=env, capture_output=True, text=True)

    def test_dynamic_and_1080_keep_original_allocations(self):
        for value, mode in (("dynamic", 0), ("1080", 1)):
            with self.subTest(value=value):
                result = self.run_probe(value)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), f"{mode}|{(1080 << 32) | 1920}|0|d280f008|f2c08708|52a2e008")

    def test_native_1440_changes_both_canvas_dimensions(self):
        result = self.run_probe("1440")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), f"2|{(1440 << 32) | 2560}|0|d2814008|f2c0b408|52a2e008")

    def test_changed_game_instructions_are_rejected(self):
        result = self.run_probe("1440", "wrong-version")
        self.assertEqual(result.returncode, 42)
        self.assertIn("not the supported 1.0.0 build", result.stderr)

    def test_scene_and_postprocess_targets_preserve_original_presets(self):
        result = self.run_probe("1440", "target")
        self.assertEqual(result.stdout.strip(), "2560,1440,2560,1440|1600,900,1920,1080")
        for width, height, want in ((960,540,"1280,720"), (480,270,"640,360"), (1024,1024,"1024,1024")):
            result = self.run_probe("1440", "target", str(width), str(height))
            self.assertEqual(result.stdout.strip(), f"{want},{want}|{width},{height},{width},{height}")


if __name__ == "__main__":
    unittest.main()
