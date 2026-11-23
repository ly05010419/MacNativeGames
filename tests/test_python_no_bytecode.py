"""A Python started with -I ignores PYTHONDONTWRITEBYTECODE, so from a released Launcher it writes .pyc files into the
signed app and breaks its seal (Launcher 1.1.4's verification: 107 new files from one `python -I -m pip --version`, none
with -I -B). Every Python command the Launcher's code runs with -I therefore also says -B."""
import ast
from pathlib import Path
import subprocess
import sys
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))


def runs_python(node):
    """Whether a command list's first item is a Python: sys.executable, a variable named python, or python(...)."""
    if isinstance(node, ast.Attribute):
        return node.attr == 'executable'
    if isinstance(node, ast.Name):
        return 'python' in node.id.lower()
    if isinstance(node, ast.Call):
        return runs_python(node.func) or (isinstance(node.func, ast.Attribute) and node.func.attr == 'python')
    return False


def missing_b(source):
    out = []
    for node in ast.walk(ast.parse(source)):
        if not isinstance(node, ast.List) or len(node.elts) < 2 or not runs_python(node.elts[0]):
            continue
        flags = [e.value for e in node.elts[1:3] if isinstance(e, ast.Constant)]
        if flags[:1] == ['-I'] and '-B' not in flags:
            out.append(node.lineno)
    return out


class PythonNeverWritesIntoTheApp(unittest.TestCase):
    def test_the_check_finds_the_problem(self):
        self.assertEqual(missing_b("import sys\na = [sys.executable, '-I', 'x.py']\n"), [2])
        self.assertEqual(missing_b("a = [python(f), '-I', '-m', 'pip']\n"), [1])
        self.assertEqual(missing_b("import sys\na = [sys.executable, '-I', '-B', 'x.py']\n"), [])
        self.assertEqual(missing_b("a = ['-I', src]\nb = [clang, '-I', src]\n"), [])

    def test_every_command_says_B(self):
        from tools.builder_jobs.repo import GAME_FOLDERS
        carried = sorted({f'games/{folder}' for folders in GAME_FOLDERS.values() for folder in folders})
        files = subprocess.run(['git', '-C', str(REPO), 'ls-files', 'tools', 'build_app.py', 'update_runtime.py',
                                'shader_update.py', *carried], capture_output=True, text=True, check=True)
        found = {}
        for name in files.stdout.splitlines():
            if name.endswith('.py') and (REPO / name).is_file():
                lines = missing_b((REPO / name).read_text())
                if lines:
                    found[name] = lines
        self.assertEqual(found, {})


if __name__ == '__main__':
    unittest.main()
