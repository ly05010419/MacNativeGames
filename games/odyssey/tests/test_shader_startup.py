"""Odyssey audio smoothing, HUD modes and packaged render-pipeline warm-up regressions."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
NVN = GAME / "third_party/d2r_gfx/gfx/nvn"


class WarmLists(unittest.TestCase):
    def test_packaged_pipeline_recipes_and_seed_aliases(self):
        spec = importlib.util.spec_from_file_location("odyssey_packager", GAME / "scripts/package_native_app.py")
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        first = next(line for line in (GAME / "assets/shader_list.tsv").read_text().splitlines() if line.startswith("R\t"))
        key = bytes.fromhex(first.split("\t")[1])
        old = key[:80].split(b"\0")[0].decode()
        new = old[:65] + ("f" * 12 if old[65:77] != "f" * 12 else "e" * 12) + "_b"
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            package.BUILD = root / "work"
            build = package.BUILD / "build"
            build.mkdir(parents=True)
            (build / "shader-seed.aliases.tsv").write_text(f"{old[:-2]}\t{new[:-2]}\n")
            # A locally recorded title list takes priority over shared title rows.
            (build / "pso_title.tsv").write_text("R\t" + first.split("\t")[1] + "\n")
            macos, game = root / "Contents/MacOS", root / "Contents/Resources/game"
            macos.mkdir(parents=True)
            package.package_warm_lists(macos, game)
            for name in ("pso_manifest.tsv", "pso_title.tsv"):
                linked = macos / name
                self.assertTrue(linked.is_file(), f"runtime cannot find {name} beside executable")
                rows = [line for line in linked.read_text().splitlines() if line.startswith("R\t")]
                self.assertTrue(rows, "shader libraries alone leave render pipelines to compile during play")
                names = [bytes.fromhex(line.split("\t")[1])[:80].split(b"\0")[0].decode() for line in rows]
                self.assertIn(new, names)
                self.assertNotIn(old, names)



if __name__ == "__main__":
    unittest.main()
