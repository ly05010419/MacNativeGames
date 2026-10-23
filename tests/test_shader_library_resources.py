"""Retained production Metal libraries must not exhaust the process file table."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from build_app import metal_compiler

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(shutil.which('xcrun'), 'requires the Mac Metal runtime')
class ShaderLibraryResources(unittest.TestCase):
    def test_512_live_libraries_and_functions_fit_a_256_descriptor_limit(self):
        explicit = os.environ.get('MNG_TEST_METAL_COMPILER')
        found = (explicit, '', dict(os.environ)) if explicit else metal_compiler()
        if not found:
            self.skipTest('requires an installed offline Metal compiler')
        compiler, _, environment = found
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, library, probe = root/'tiny.metal', root/'tiny.metallib', root/'probe'
            source.write_text('#include <metal_stdlib>\nusing namespace metal;\n'
                              'kernel void mng_library_fd_probe(device uint *a [[buffer(0)]]) { a[0] = 17; }\n')
            subprocess.run([compiler, '-fmetal-math-mode=safe', '-o', str(library), str(source)],
                           check=True, capture_output=True, env=environment)
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wno-deprecated-declarations', '-Wno-unused-function', '-I'+str(INCLUDE),
                            str(ROOT/'games/totk/scripts/diag/check_shader_library_fds.m'),
                            '-framework', 'Foundation', '-framework', 'Metal', '-o', str(probe)],
                           check=True, capture_output=True)
            result = subprocess.run([str(probe), str(library), str(root/'held'), '512'],
                                    capture_output=True, text=True, timeout=60)
            if result.returncode == 77:
                self.skipTest('no Metal device')
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual((report['held_libraries'], report['held_functions']), (512, 512))
            self.assertLessEqual(report['fd_soft_limit'], 256)
            self.assertLessEqual(report['fd_peak']-report['fd_baseline'], 8, report)
