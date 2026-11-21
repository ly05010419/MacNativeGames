"""dmsh_chain.code_form, the text dmsh_build.py's CODE_FP hashes: edits that cannot change what the build computes leave it alone, every other edit changes it."""
from pathlib import Path
import sys
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain  # noqa: E402

BASE = '''"""module doc"""
import re
LIMIT = 8   # a limit
def f(x, y=1):
    """what f does"""
    s = "text"
    return x + y if x else len(s)
class C:
    """a class"""
    def m(self): return LIMIT
'''


def form(src, blank=()): return dmsh_chain.code_form(src, blank)


class CodeForm(unittest.TestCase):
    def test_edits_that_cannot_change_output_keep_the_form(self):
        for what, src in (
                ('comment', BASE.replace('# a limit', '# another limit, longer').replace('s = "text"', '# note\n    s = "text"')),
                ('docstring', BASE.replace('what f does', 'rewritten').replace('module doc', 'x').replace('"""a class"""', '"""b"""')),
                ('docstring removed', BASE.replace('    """what f does"""\n', '')),
                ('blank lines and layout', BASE.replace('def f(x, y=1):', '\n\ndef f( x,y = 1 ):').replace('LIMIT = 8', 'LIMIT=8').replace("'", '"'))):
            with self.subTest(what):
                self.assertEqual(form(src), form(BASE))

    def test_edits_that_can_change_output_change_the_form(self):
        for what, src in (
                ('operator', BASE.replace('x + y', 'x - y')),
                ('constant', BASE.replace('LIMIT = 8', 'LIMIT = 9')),
                ('float constant', BASE.replace('LIMIT = 8', 'LIMIT = 8.0')),
                ('string literal', BASE.replace('"text"', '"texts"')),
                ('default argument', BASE.replace('y=1', 'y=2')),
                ('statement', BASE.replace('    s = "text"\n', '    s = "text"\n    s += "!"\n')),
                ('name', BASE.replace('def m(self)', 'def n(self)')),
                ('bare string statement that is not a docstring', BASE.replace('    s = "text"\n', '    s = "text"\n    "not a docstring"\n')),
                ('decorator', BASE.replace('def f(', '@staticmethod\ndef f('))):
            with self.subTest(what):
                self.assertNotEqual(form(src), form(BASE))

    def test_blanked_function_bodies(self):
        self.assertEqual(form(BASE.replace('x + y', 'x * y'), {'f'}), form(BASE, {'f'}))
        self.assertNotEqual(form(BASE.replace('x + y', 'x * y'), {'m'}), form(BASE, {'m'}))
        self.assertNotEqual(form(BASE.replace('def f(x, y=1)', 'def f(x, y=2)'), {'f'}), form(BASE, {'f'}))   # the signature stays

    def test_python_version_is_part_of_the_form(self):
        with mock.patch.object(sys, 'version_info', (3, 99, 0)):
            other = form(BASE)
        self.assertNotEqual(other, form(BASE))

    def test_real_build_script(self):
        src = (TOOLS / 'dmsh_build.py').read_text()
        self.assertEqual(form(src + '\n# trailing comment\n'), form(src))
        self.assertNotEqual(form(src + '\nX = 1\n'), form(src))
        self.assertNotEqual(form(src.replace("'--msl-version', '20300'", "'--msl-version', '20400'")), form(src))


if __name__ == '__main__':
    unittest.main()
