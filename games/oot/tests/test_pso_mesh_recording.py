"""CPU-only regression using current production recording functions verbatim."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent
SOURCE = TESTS.parents[1] / 'totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'
FUNCTIONS = (
    'static uint64_t pkg_hash(',
    'static NSNumber *seen_key(',
    'static NSNumber *seen_key_str(',
    'static NSString *pso_user_path(',
    'static FILE *pso_user_file(',
    'static NSString *pso_shared_path(',
    'static void pso_user_mesh_warm_paths(',
    'static void pso_user_record(',
)


def function(source, signature):
    """Extract one definition, skipping braces inside comments and literals."""
    start = source.index(signature)
    opening = source.index('{', start)
    depth, position, state = 0, opening, 'code'
    while position < len(source):
        char = source[position]
        pair = source[position:position + 2]
        if state == 'line':
            if char == '\n':
                state = 'code'
        elif state == 'comment':
            if pair == '*/':
                state = 'code'
                position += 1
        elif state in ('"', "'"):
            if char == '\\':
                position += 1
            elif char == state:
                state = 'code'
        elif pair == '//':
            state = 'line'
            position += 1
        elif pair == '/*':
            state = 'comment'
            position += 1
        elif char in ('"', "'"):
            state = char
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if not depth:
                return source[start:position + 1] + '\n'
        position += 1
    raise AssertionError(f'Unclosed function: {signature}')


class MeshRecordingTests(unittest.TestCase):
    def test_production_recording_and_warm_policy(self):
        source = SOURCE.read_text()
        # Check the real initialization hook precedes all warm workers, including
        # the cold-cache branch; the fixture then tests its actual functions.
        warm = function(source, 'static void warm_start(void) {')
        self.assertEqual(warm.count('pso_user_mesh_warm_paths(shipped, paths);'), 1)
        self.assertLess(warm.index('g_par_fill_lists = lists;'),
                        warm.index('pso_user_mesh_warm_paths(shipped, paths);'))
        self.assertLess(warm.index('pso_user_mesh_warm_paths(shipped, paths);'),
                        warm.index('dispatch_async(q,'))
        production = ''.join(function(source, name) for name in FUNCTIONS)
        print('production functions SHA-256:', hashlib.sha256(production.encode()).hexdigest(), flush=True)
        with tempfile.TemporaryDirectory(prefix='oot-pso-recording-') as directory:
            root = Path(directory)
            (root / 'pso_mesh_recording_production.inc').write_text(production)
            for oot in (False, True):
                executable = root / ('oot' if oot else 'other')
                command = ['clang', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                           '-fobjc-arc', '-framework', 'Foundation', '-I', str(root)]
                if oot:
                    command.append('-DGAME_OOT=1')
                subprocess.run([*command, str(TESTS / 'pso_mesh_recording.m'), '-o', str(executable)], check=True)
                subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
