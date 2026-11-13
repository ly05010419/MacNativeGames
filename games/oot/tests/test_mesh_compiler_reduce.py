"""tools/mesh_compiler_reduce/reduce.py (#2107): the parts that run without an affected Mac. The statement reduction keeps
exactly what a fake compiler crash needs, the manifest rows give each failing mesh its object and fragment shaders, and the
report masks every number."""
import importlib.util
import io
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1] / 'tools/mesh_compiler_reduce/reduce.py'
spec = importlib.util.spec_from_file_location('mesh_reduce', HERE)
reduce = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reduce)

SOURCE = """#include <metal_stdlib>
struct S
{
    int member;
};
static inline int helper(int v)
{
    int w = v + 1;
    return w;
}
[[mesh]] void main0(int x)
{
    int a = 1;
    int b = 2;
    int c = b * 3;
    atomic_fetch_add(a, 0x10u);
    float d = 1.5f;
    store(c, d);
}"""


def compiles(text):
    """A front end that rejects a use of a removed declaration."""
    for name in 'abcdw':
        declared = f'int {name} =' in text or f'float {name} =' in text
        used = any(f'({name}' in l or f' {name})' in l or f'{name}, ' in l or f'= {name} ' in l or f'({name},' in l
                   for l in text.split('\n') if f'{name} =' not in l)
        if used and not declared: return False
    return True


class Reduction(unittest.TestCase):
    def test_keeps_only_what_the_crash_needs(self):
        lines = SOURCE.split('\n')
        entry = next(i for i, l in enumerate(lines) if '[[mesh]]' in l)
        cand = reduce.removable(lines, entry)
        self.assertNotIn(lines.index('    int member;'), cand)        # struct members are not statements
        self.assertIn(lines.index('    int a = 1;'), cand)

        def test(kept):
            drop = set(cand) - set(kept)
            text = '\n'.join(l for i, l in enumerate(lines) if i not in drop)
            return compiles(text) and 'atomic_fetch_add' in text      # the fake crash

        kept = reduce.ddmin(cand, test, time.time() + 60, io.StringIO())
        self.assertEqual([lines[i].strip() for i in kept], ['int a = 1;', 'atomic_fetch_add(a, 0x10u);'])

    def test_deadline_stops_with_what_still_crashes(self):
        kept = reduce.ddmin(list(range(10)), lambda kept: True, time.time() - 1, io.StringIO())
        self.assertEqual(kept, list(range(10)))

    def test_numbers_are_masked(self):
        self.assertEqual(reduce.masked('    x = y * 1.5f + 0x10u - 3 + _42;'), 'x = y * K + K - K + _42;')

    def test_manifest_rows(self):
        mesh = reduce.FAILING[0] + reduce.SUFFIX
        obj, frag = 'a' * 64 + reduce.SUFFIX, 'b' * 64 + reduce.SUFFIX
        other = 'c' * 64 + reduce.SUFFIX
        with tempfile.TemporaryDirectory() as d:
            m = Path(d) / 'pso_manifest.tsv'
            m.write_text('# comment\n'
                         f'M\t-\t{(obj + chr(0) + mesh + chr(0) + frag + chr(0)).encode().hex()}04\n'
                         f'M\t-\t{(other + chr(0) + frag + chr(0)).encode().hex()}\n'
                         f'R\t-\t{mesh.encode().hex()}\n')
            self.assertEqual(reduce.pipelines(m), {mesh: (obj, frag)})


if __name__ == '__main__':
    unittest.main()
