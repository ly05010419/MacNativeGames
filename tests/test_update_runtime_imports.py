"""update_runtime.py / build_app.py: no function uses a top-level module before importing it again. The import makes the
name local to the whole function, so the earlier use fails with UnboundLocalError (2026-11-03: `import plistlib`
inside main() broke every update before it started)."""
import ast
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def shadowing_imports(path):
    """(function, name) for every module the file imports at the top that a function imports again after using it."""
    tree = ast.parse(path.read_text())
    top = {(a.asname or a.name).split('.')[0] for node in tree.body if isinstance(node, (ast.Import, ast.ImportFrom))
           for a in node.names}
    found = []
    for fn in ast.walk(tree):
        if not isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        local = {}
        for node in ast.walk(fn):
            if isinstance(node, (ast.Import, ast.ImportFrom)):
                for a in node.names:
                    name = (a.asname or a.name).split('.')[0]
                    if name in top: local[name] = min(local.get(name, node.lineno), node.lineno)
        for node in ast.walk(fn):
            if isinstance(node, ast.Name) and node.id in local and node.lineno < local[node.id]:
                found.append((fn.name, node.id)); local.pop(node.id)
    return found


class ImportShadowing(unittest.TestCase):
    def test_no_use_before_a_local_reimport(self):
        for name in ('update_runtime.py', 'build_app.py'):
            with self.subTest(name):
                self.assertEqual(shadowing_imports(ROOT / name), [])


if __name__ == '__main__':
    unittest.main()
