"""Compile the shared shader preference code and check startup precedence."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AsyncShaderSettings(unittest.TestCase):
    def test_persistence_environment_and_failed_write(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler required')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'settings.c'
            source.write_text(r'''
#include "async_shader_settings.h"
#include <assert.h>
int main(int argc, char **argv) {
    assert(argc == 2);
    unsetenv("TOTK_ASYNC_SHADERS");
    setenv("TOTK_PROFILE_DIR", argv[1], 1);
    assert(!async_shader_choice());
    assert(async_shader_choose(1));
    assert(async_shader_choice());
    setenv("TOTK_ASYNC_SHADERS", "0", 1);
    assert(!async_shader_choice());
    assert(!async_shader_choose(1));
    setenv("TOTK_ASYNC_SHADERS", "garbage", 1);
    assert(!async_shader_choice());
    setenv("TOTK_ASYNC_SHADERS", "1", 1);
    assert(async_shader_choice());
    unsetenv("TOTK_ASYNC_SHADERS");
    assert(async_shader_choice());
    assert(async_shader_choose(0));
    assert(!async_shader_choice());
    setenv("TOTK_PROFILE_DIR", "/nonexistent/profile", 1);
    assert(!async_shader_choose(1));
    assert(!async_shader_choice());
    return 0;
}
''')
            executable = root / 'settings'
            build = subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-I' + str(ROOT / 'games/totk/runtime'), str(source), '-o', str(executable)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(executable), str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / 'async-shaders.txt').read_text(), '0\n')


if __name__ == '__main__':
    unittest.main()
