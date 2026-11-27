"""The app icon from a pinned official image (games/*/assets/app-icon.json, extract_app_icon.py --source-spec) is optional:
an image the store took down only costs the icon, never the build. 2026-11-27, #2401: Ocarina of Time's pinned image went
404 and every build from a checkout stopped at the icon step. The icon from a game's control data was already optional."""
import ast
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CALLERS = ('build_app.py', 'games/oot/build.py')


def source_spec_calls(path):
    """(line, inside a try that catches an error) of every call whose arguments name --source-spec."""
    tree = ast.parse(path.read_text())
    parents = {child: node for node in ast.walk(tree) for child in ast.iter_child_nodes(node)}
    found = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        names = [n.value for n in ast.walk(node) if isinstance(n, ast.Constant) and isinstance(n.value, str)]
        if '--source-spec' not in names or 'scripts/extract_app_icon.py' not in names:
            continue
        guarded, up = False, node
        while up in parents:
            up = parents[up]
            if isinstance(up, ast.Try) and up.handlers:
                guarded = True
                break
        found.append((node.lineno, guarded))
    return found


class AppIconOptional(unittest.TestCase):
    def test_every_pinned_icon_step_may_fail(self):
        seen = 0
        for name in CALLERS:
            for line, guarded in source_spec_calls(REPO / name):
                seen += 1
                self.assertTrue(guarded, f'{name}:{line}: the pinned-image icon step must not stop the build (#2401)')
        self.assertGreaterEqual(seen, 2, 'the icon steps of build_app.py (Bananza, Pokopia) and games/oot/build.py')

    def test_the_build_record_reads_the_icon_only_when_there_is_one(self):
        text = (REPO / 'build_app.py').read_text()
        self.assertIn("if icon_spec.is_file() and (tree / 'build/AppIcon.icns').is_file():", text)


if __name__ == '__main__':
    unittest.main()
