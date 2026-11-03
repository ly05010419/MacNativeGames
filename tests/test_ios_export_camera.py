"""iOS exports must not install camera veneers containing Mac function pointers."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'the runtime builds on macOS')
class ExportCamera(unittest.TestCase):
    def test_export_leaves_camera_unhooked(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'export-camera.c'
            source.write_text(
                '#include "' + str(ROOT / 'games/totk/runtime/game_patches_v143.c') + '"\n'
                '#include <assert.h>\n'
                'int main(void) {\n'
                '  setenv("TOTK_EXPORT_IOS", "test-export", 1);\n'
                '  unsetenv("TOTK_VR_HEADLOOK");\n'
                '  Module module = {0};\n'
                '  vr_headlook_install(&module);\n'
                '  assert(g_vr_headlook == NULL);\n'
                '  return 0;\n'
                '}\n')
            executable = root / 'export-camera'
            build = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                 '-DTOTK_GAME_V143=1', '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/include'),
                 '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip', str(source), '-o', str(executable)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
