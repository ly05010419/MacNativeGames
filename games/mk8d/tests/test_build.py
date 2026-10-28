"""Mario Kart 8 Deluxe's build without game files: the 4.0.1 pins, the work folder rules and the folder's independence.

Run: python -m unittest discover -s games/mk8d/tests (scripts/build_mk8d.py's own imports only, no Python packages).
"""
from pathlib import Path
import argparse
import ast
import re
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / "scripts"))
import build_mk8d  # noqa: E402
import mk8d  # noqa: E402

HEX64 = re.compile(r"[0-9a-f]{64}")


class Pins(unittest.TestCase):
    def test_expected_names_4_0_1_and_pins_every_module(self):
        pins = mk8d.expected()
        self.assertEqual((pins["title_id"], pins["version"], pins["update_version"]), (mk8d.TITLE, mk8d.VERSION, 1507328))
        self.assertEqual(sorted(pins["exefs_sha256"]), sorted(build_mk8d.MODULES))
        self.assertEqual(sorted(pins["rewritten_sha256"]), sorted(f"{m}.patched.bin" for m in build_mk8d.MODULES))
        for digest in [*pins["exefs_sha256"].values(), *pins["rewritten_sha256"].values(), pins["romfs"]["sha256"]]:
            self.assertRegex(digest, HEX64)
        self.assertRegex(pins["main_build_id"], r"[0-9a-f]{64}")

    def test_runtime_names_the_same_game_and_version(self):
        header = (GAME / "runtime/game_version.h").read_text()
        self.assertIn(f'#define TOTK_GAME_VERSION "{mk8d.VERSION}"', header)
        self.assertIn(f"#define GAME_TITLE_ID 0x{mk8d.TITLE}ULL", header)


class WorkFolder(unittest.TestCase):
    def problems(self, work):
        with tempfile.TemporaryDirectory() as folder:
            game = Path(folder) / "game.nsp"
            game.write_bytes(b"")
            args = argparse.Namespace(game=game, update=game, keys=game, firmware=Path(folder), work=work,
                                      developer_dir="/Applications/Xcode.app/Contents/Developer")
            try:
                build_mk8d.preflight(args, sys.executable)
            except SystemExit as stop:
                return str(stop)
            return ""

    def test_inside_the_checkout_is_refused(self):
        self.assertIn("--work must be outside the checkout", self.problems(GAME / "work"))
        self.assertIn("--work must be outside the checkout", self.problems(GAME.parents[1] / "mk8d-work"))

    def test_an_ignored_build_folder_of_the_game_is_allowed(self):
        self.assertNotIn("--work must be outside the checkout", self.problems(GAME / "MK8D_build"))

    def test_a_space_in_the_path_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertIn("a space", self.problems(Path(folder) / "mk8d work"))

    def test_icloud_drive_is_refused(self):
        self.assertTrue(build_mk8d.icloud_synced(Path.home() / "Library/Mobile Documents/com~apple~CloudDocs/mk8d"))
        self.assertFalse(build_mk8d.icloud_synced(Path("/private/tmp/mk8d-work")))


OTHER_GAME = re.compile(r"\.\./(totk|botw|by3|odyssey|ssbu|wonder)\b|games/(totk|botw|by3|odyssey|ssbu|wonder)\b")


def other_game_paths(name, text):
    """Places a file would read another game's folder: Python string literals that are a path (not docstrings, and not
    text such as a list's header line that names where another game's list lives), `#include` lines, and Makefile lines
    that are not comments. Comments and docstrings may name a folder (where a copy came from)."""
    if name.endswith(".py"):
        tree = ast.parse(text)
        docstrings = {id(node.body[0].value) for node in ast.walk(tree)
                      if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef))
                      and node.body and isinstance(node.body[0], ast.Expr) and isinstance(node.body[0].value, ast.Constant)}
        return [f"{name}:{node.lineno}: {node.value!r}" for node in ast.walk(tree)
                if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docstrings
                and not any(c.isspace() for c in node.value) and OTHER_GAME.search(node.value)]
    lines = enumerate(text.splitlines(), 1)
    if name == "Makefile" or name.endswith(".mk"):
        return [f"{name}:{n}: {line.strip()}" for n, line in lines
                if OTHER_GAME.search(line.split("#", 1)[0])]
    return [f"{name}:{n}: {line.strip()}" for n, line in lines
            if line.lstrip().startswith(("#include", "#import")) and OTHER_GAME.search(line)]


class OwnFolder(unittest.TestCase):
    def test_no_other_games_folder_is_read(self):
        """CONTRIBUTING.md: the build and scripts never read another game's folder."""
        files = subprocess.run(["git", "ls-files", "--", "Makefile", "scripts", "third_party", "headless", "runtime"],
                               cwd=GAME, capture_output=True, text=True, check=True).stdout.split()
        found = []
        for name in files:
            if Path(name).suffix in {".py", ".c", ".h", ".m", ".S", ".inc"} or name == "Makefile":
                found += other_game_paths(name, (GAME / name).read_text(errors="replace"))
        self.assertEqual(found, [])

    def test_the_check_finds_a_read(self):
        self.assertTrue(other_game_paths("x.py", 'def f(source="games/totk/assets/list.tsv"):\n    """games/totk is fine here"""\n'))
        self.assertFalse(other_game_paths("x.py", '"""Copied from games/totk/scripts."""\n# games/odyssey too\n'))
        self.assertFalse(other_game_paths("x.py", "HEADER = '# TOTK shader list v1 (games/totk/scripts/shader_list.py)'\n"))
        self.assertTrue(other_game_paths("x.py", "LIST = Path('../totk/assets/shader_list.tsv')\n"))
        self.assertTrue(other_game_paths("Makefile", "LIST := ../odyssey/assets/x.tsv  # copied\n"))
        self.assertFalse(other_game_paths("Makefile", "# from games/odyssey at ff63a16\n"))
        self.assertTrue(other_game_paths("x.c", '#include "../../totk/runtime/runtime.h"\n'))


if __name__ == "__main__":
    unittest.main()
