"""The shared graphics layer's empty-grid guard never drops an indirect compute dispatch (#2190, #2152).

dispatch() (nvn_mtl.m) returns early for a zero grid: Metal's direct-dispatch API rejects zero dimensions. A recorded
indirect dispatch (hle_nvn.c nvnCommandBufferDispatchComputeIndirect) carries no x/y/z, its grid lives in the argument
buffer, so a guard that looks at x/y/z alone dropped every indirect dispatch from 2026-11-05 (658bb801) until #2190:
Ocarina of Time's grass stopped being generated. The end-to-end check is probe-gpu-drv-c0 (it needs the game's code);
this test compiles the guard's own condition from the production source and runs it on any Mac.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


def guard_condition(source):
    """The condition of dispatch()'s early return that reads the grid (dp->x / y / z)."""
    body = source[source.index('static void dispatch(const p_dispatch *dp) {'):]
    body = body[:body.index('\n}\n')]
    for line in body.splitlines():
        m = re.match(r'\s*if \((.*)\) return;', line)
        if m and 'dp->x' in m.group(1):
            return m.group(1)
    raise AssertionError('dispatch() has no early return on the grid')


def skips(condition, cases):
    """For each p_dispatch initializer in cases, whether the guard with this condition returns early (1) or not (0)."""
    program = '#include <stdio.h>\n#include "nvn_obj.h"\n' \
              f'static int skipped(const p_dispatch *dp) {{ if ({condition}) return 1; return 0; }}\n' \
              'int main(void) {\n' + ''.join(f'  {{ p_dispatch d = {c}; printf("%d\\n", skipped(&d)); }}\n' for c in cases) + \
              '  return 0;\n}\n'
    with tempfile.TemporaryDirectory() as folder:
        path, binary = Path(folder) / 'guard.c', Path(folder) / 'guard'
        path.write_text(program)
        build = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                                '-I' + str(NVN), '-I' + str(NVN / 'include'), str(path), '-o', str(binary)],
                               capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        return [int(x) for x in run.stdout.split()]


CASES = {
    '{.indirect = 0x1000}': 0,                       # indirect, as recorded: x/y/z zero, the grid in the buffer
    '{.x = 4, .y = 2, .z = 1, .indirect = 0x1000}': 0,
    '{.x = 0, .y = 1, .z = 1}': 1,                   # direct, empty: skipped (Metal rejects it)
    '{.x = 1, .y = 0, .z = 1}': 1,
    '{.x = 1, .y = 1, .z = 0}': 1,
    '{.x = 8, .y = 1, .z = 1}': 0,                   # direct, ordinary
}


@unittest.skipUnless(sys.platform == 'darwin', 'Xcode clang')
class IndirectDispatchGuard(unittest.TestCase):
    def test_an_indirect_dispatch_is_recorded_without_its_grid(self):
        """The premise: the recorder fills only the caller and the argument address."""
        hle = (NVN / 'hle_nvn.c').read_text()
        line = hle[hle.index('NVN(nvnCommandBufferDispatchComputeIndirect'):]
        line = line[:line.index('\n}')]
        self.assertIn('.indirect=indirect', line.replace(' ', ''))
        self.assertNotRegex(line, r'\.[xyz]\s*=')

    def test_the_guard_skips_only_empty_direct_dispatches(self):
        condition = guard_condition((NVN / 'nvn_mtl.m').read_text())
        self.assertEqual(skips(condition, list(CASES)), list(CASES.values()), condition)


if __name__ == '__main__':
    unittest.main()
