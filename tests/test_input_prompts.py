"""Compile the production prompt model and platform renderer; no game files or GPU."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games/totk/runtime'


@unittest.skipUnless(sys.platform == 'darwin', 'CoreText is a macOS renderer')
class InputPrompts(unittest.TestCase):
    def test_model_and_live_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            for source, extra in (
                ('test_prompt_bindings.c', []),
                ('test_input_prompts.m', [str(RUNTIME / 'input_prompts.m'), '-fobjc-arc',
                                         '-framework', 'Foundation', '-framework', 'CoreGraphics', '-framework', 'CoreText']),
            ):
                with self.subTest(source=source):
                    binary = pathlib.Path(tmp) / pathlib.Path(source).stem
                    build = subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-I', str(RUNTIME),
                                            str(ROOT / 'tests' / source), *extra, '-o', str(binary)],
                                           capture_output=True, text=True)
                    self.assertEqual(build.returncode, 0, build.stderr)
                    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_live_texture_replacements(self):
        gfx = RUNTIME.parent / 'third_party/d2r_gfx/gfx'
        with tempfile.TemporaryDirectory() as tmp:
            (pathlib.Path(tmp) / 'build').mkdir()
            binary = pathlib.Path(tmp) / 'prompt-textures'
            args = ['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-fobjc-arc',
                    '-I', str(RUNTIME), '-I', str(gfx / 'nvn'), '-I', str(gfx / 'nvn/include'),
                    str(ROOT / 'tests/test_prompt_textures.m'), str(RUNTIME / 'input_prompts.m'),
                    str(gfx / 'nvn/totk_prompt_icons.m'), str(gfx / 'nvn/nvn_tex.c'), str(gfx / 'tex/nvn_deswizzle.c'),
                    '-framework', 'Foundation', '-framework', 'CoreText', '-framework', 'CoreGraphics',
                    '-framework', 'Metal', '-o', str(binary)]
            build = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], cwd=tmp, capture_output=True, text=True, timeout=30)
            if run.returncode == 77:
                self.skipTest('Metal device unavailable in this environment')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
