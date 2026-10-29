"""Tests for Animal Crossing: New Horizons app packaging and launcher."""
import importlib.util
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from games.acnh.prepare import profile

GAME_DIR = Path(__file__).resolve().parents[1]
ROOT = Path(__file__).resolve().parents[3]

spec_pkg = importlib.util.spec_from_file_location("acnh_pkg", GAME_DIR / "scripts/package_native_app.py")
acnh_pkg = importlib.util.module_from_spec(spec_pkg)
spec_pkg.loader.exec_module(acnh_pkg)

spec_pre = importlib.util.spec_from_file_location("acnh_pre", GAME_DIR / "scripts/precompile_cached_shaders.py")
acnh_pre = importlib.util.module_from_spec(spec_pre)
spec_pre.loader.exec_module(acnh_pre)


class PackagingTests(unittest.TestCase):
    def test_packager_rejects_old_runtime_with_new_version_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            prepared, work = root / "prepared", root / "host"
            prepared.mkdir()
            (work / "build").mkdir(parents=True)
            expected = profile(2097152)
            summary = {"status": "prepared_for_host_bringup", "title_id": expected["title_id"],
                       "inputs": {"update": {key: expected[key] for key in
                                  ("title_id", "package_kind", "cnmt_version_raw")}}}
            (prepared / "preparation.json").write_text(json.dumps(summary))
            (work / "host-info.json").write_text(json.dumps({"display_version": "3.0.1", "prepared": str(prepared)}))
            runtime = work / "build/acnh-native"
            runtime.write_bytes(b"MNG_ACNH_VERSION:1.2.1")
            runtime.chmod(0o755)
            with self.assertRaisesRegex(ValueError, "another ACNH version"):
                acnh_pkg.verify_inputs(work, prepared, root / "fonts")

    def test_pinned_translator(self):
        self.assertTrue(acnh_pkg.TRANSLATOR.is_relative_to(GAME_DIR))
        wanted = acnh_pkg.TRANSLATOR_SHA256.read_text().split()[0]
        self.assertEqual(acnh_pkg.pinned_translator()["sha256"], wanted)

    def test_translator_mismatch_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            fake = Path(temp) / "totk_xlate"
            fake.write_bytes(b"not the translator")
            fake.chmod(0o755)
            original = acnh_pkg.TRANSLATOR
            acnh_pkg.TRANSLATOR = fake
            try:
                with self.assertRaisesRegex(ValueError, "differs from the pinned"):
                    acnh_pkg.pinned_translator()
            finally:
                acnh_pkg.TRANSLATOR = original

    def test_font_rows_parsing(self):
        with tempfile.TemporaryDirectory() as temp:
            header = Path(temp) / "shared_fonts_config.h"
            header.write_text(
                '#define SHARED_FONT_COUNT 6\n'
                '#define SHARED_FONT_ROWS '
                '{"/path/to/0-standard.bfttf",2053636ULL,2053628ULL,"823913c9d80807979381e327846466f561f31ba1842cea8f7fedc5ad7d45d2ad"},'
                '{"/path/to/1-chinese-simplified.bfttf",7814752ULL,7814744ULL,"7e95d45f43c6f35ee0c7bd8ea446675d8ab0390e18fd2844abcac11d2bff7389"},'
                '{"/path/to/2-chinese-simplified-extension.bfttf",106328ULL,106320ULL,"0f90d45b741e9da7700c416eb2b51996b458c3e4fb3c3291740b758c298e7a25"},'
                '{"/path/to/3-chinese-traditional.bfttf",3580268ULL,3580260ULL,"9d59b74e59a2dc92296d6bebad7cce791ba94688bd436304aaae174a58bd7457"},'
                '{"/path/to/4-korean.bfttf",3672876ULL,3672868ULL,"5a4e694782ead5ddcbc0a226aab24362e2d641ca33bac7bdf8cda36ff157c9e8"},'
                '{"/path/to/5-nintendo-extension.bfttf",180244ULL,180236ULL,"f9f62e025c9bbe0e9ae25a54ba5011202b1e3a3f3c33b2d876c40aff6174af68"}\n'
            )
            rows = acnh_pkg.font_rows(header)
            self.assertEqual(len(rows), 6)
            self.assertIn("0-standard.bfttf", rows)
            self.assertEqual(rows["0-standard.bfttf"]["size"], 2053636)
            self.assertEqual(rows["0-standard.bfttf"]["sha256"], "823913c9d80807979381e327846466f561f31ba1842cea8f7fedc5ad7d45d2ad")

    def test_launcher_compilation_and_check(self):
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            app = temp_path / "Animal Crossing.app"
            contents = app / "Contents"
            macos = contents / "MacOS"
            resources = contents / "Resources"
            game = resources / "game"
            macos.mkdir(parents=True)
            game.mkdir(parents=True)

            launcher_bin = macos / "Animal Crossing"
            launcher_src = GAME_DIR / "scripts/acnh_app_launcher.c"
            subprocess.run(
                ["xcrun", "clang", "-O2", "-Wall", "-Wextra", str(launcher_src), "-o", str(launcher_bin)],
                check=True,
            )
            self.assertTrue(launcher_bin.is_file())

            # Without runtime, --check fails
            res = subprocess.run([str(launcher_bin), "--check"], capture_output=True, text=True)
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("missing runtime", res.stderr)

            # Create dummy runtime executable
            runtime = macos / "acnh-native"
            runtime.write_text("#!/bin/sh\nexit 0\n")
            runtime.chmod(0o755)

            # With runtime, --check succeeds
            res = subprocess.run([str(launcher_bin), "--check"], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0)
            self.assertIn("runtime exists; game was not launched", res.stdout)

    def test_valid_metallib_detector(self):
        with tempfile.TemporaryDirectory() as temp:
            lib = Path(temp) / "test.metallib"
            lib.write_bytes(b"MTLB\x01\x00\x00\x00")
            self.assertTrue(acnh_pre.valid_library(lib))

            not_lib = Path(temp) / "fake.metallib"
            not_lib.write_bytes(b"FAKE\x00\x00\x00\x00")
            self.assertFalse(acnh_pre.valid_library(not_lib))

            empty_lib = Path(temp) / "empty.metallib"
            empty_lib.write_bytes(b"")
            self.assertFalse(acnh_pre.valid_library(empty_lib))


if __name__ == "__main__":
    unittest.main()
