"""Game build outputs must not enter any linked source checkout through a symlink."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ArtifactPaths(unittest.TestCase):
    def test_external_output_and_all_source_aliases(self):
        result = subprocess.check_output(["git", "-C", str(ROOT), "worktree", "list", "--porcelain"], text=True)
        roots = [Path(line[9:]) for line in result.splitlines() if line.startswith("worktree ")]
        for game in ("pla", "xcx", "mkw"):
            path = ROOT / f"games/{game}/scripts/{game}_artifact_paths.py"
            spec = importlib.util.spec_from_file_location(game + "_paths", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            with self.subTest(game=game), tempfile.TemporaryDirectory() as temporary:
                outside = Path(temporary)
                self.assertEqual(module.outside_source(outside / "new/work"), outside.resolve() / "new/work")
                for index, checkout in enumerate(roots):
                    with self.assertRaisesRegex(ValueError, "outside all source worktrees"):
                        module.outside_source(checkout / "generated")
                    alias = outside / str(index)
                    alias.symlink_to(checkout, target_is_directory=True)
                    with self.assertRaisesRegex(ValueError, "outside all source worktrees"):
                        module.outside_source(alias / "generated")
