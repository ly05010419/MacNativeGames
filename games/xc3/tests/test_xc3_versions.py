"""Xenoblade Chronicles 3: versions are chosen by content and anything unpinned stops. Needs no game files."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

GAME_DIR = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("xc3_versions", GAME_DIR / "scripts" / "versions.py")
versions = importlib.util.module_from_spec(spec)
spec.loader.exec_module(versions)


class ManifestTests(unittest.TestCase):
    def test_every_manifest_is_complete(self):
        for version, pinned in versions.VERSIONS.items():
            with self.subTest(version=version):
                self.assertEqual(pinned["title_id"], "010074F013262000")
                self.assertEqual(pinned["modules"][0], "main")
                for name in pinned["modules"]:
                    self.assertRegex(pinned["exefs_sha256"][name], "^[0-9a-f]{64}$")
                self.assertIn("main.npdm", pinned["exefs_sha256"])
                self.assertRegex(pinned["main_build_id"], "^[0-9A-F]{40}$")
                self.assertGreater(pinned["romfs"]["files"], 0)
                self.assertRegex(pinned["romfs"]["sha256"], "^[0-9a-f]{64}$")
                control = pinned["control"]
                self.assertTrue(control["supported_languages"])
                self.assertGreater(control["user_save_size"], 0)
                for name in pinned.get("rewritten_sha256", {}):
                    self.assertIn(name.removesuffix(".patched.bin"), pinned["modules"])

    def test_v100_is_pinned(self):
        self.assertEqual(versions.expected("v100")["display_version"], "1.0.0")
        with self.assertRaises(ValueError):
            versions.expected("v999")

    def test_v221_is_an_update_of_the_same_base(self):
        v100, v221 = versions.expected("v100"), versions.expected("v221")
        self.assertEqual(v221["display_version"], "2.2.1")
        self.assertEqual(v221["base_program_nca"], v100["base_program_nca"])
        self.assertEqual(v221["update_title_id"], "010074F013262800")
        # The SDK modules did not change in 2.2.1; main did.
        for name in ("sdk", "subsdk0", "rtld"):
            self.assertEqual(v221["exefs_sha256"][name], v100["exefs_sha256"][name])
        self.assertNotEqual(v221["exefs_sha256"]["main"], v100["exefs_sha256"]["main"])


class IdentifyTests(unittest.TestCase):
    def test_base_program_nca_by_name_and_size(self):
        nca = versions.expected("v100")["base_program_nca"]
        self.assertEqual(versions.identify_base({"name": nca["name"], "size": nca["size"]}), "v100")
        with self.assertRaises(ValueError):
            versions.identify_base({"name": nca["name"], "size": nca["size"] + 1})
        with self.assertRaises(ValueError):
            versions.identify_update({"name": nca["name"], "size": nca["size"]})

    def test_update_program_nca_by_name_and_size(self):
        nca = versions.expected("v221")["update_program_nca"]
        self.assertEqual(versions.identify_update({"name": nca["name"], "size": nca["size"]}), "v221")
        with self.assertRaises(ValueError):
            versions.identify_update({"name": nca["name"], "size": nca["size"] - 1})
        # The base NCA alone still means 1.0.0, never the update that shares it.
        base = versions.expected("v221")["base_program_nca"]
        self.assertEqual(versions.identify_base({"name": base["name"], "size": base["size"]}), "v100")

    def test_exefs_that_differs_in_one_file_stops(self):
        # Stand-in files whose hashes are patched into a copy of the manifest: the check itself is what is tested.
        pinned = versions.expected("v100")
        with tempfile.TemporaryDirectory() as folder:
            exefs = Path(folder)
            fake = {}
            for name in pinned["exefs_sha256"]:
                (exefs / name).write_bytes(name.encode())
                fake[name] = hashlib.sha256(name.encode()).hexdigest()
            saved = pinned["exefs_sha256"]
            try:
                pinned["exefs_sha256"] = fake
                self.assertEqual(versions.identify_exefs(exefs), "v100")
                (exefs / "main").write_bytes(b"changed")
                with self.assertRaises(ValueError):
                    versions.identify_exefs(exefs)
                (exefs / "main").write_bytes(b"main")
                (exefs / "subsdk1").write_bytes(b"extra")
                with self.assertRaises(ValueError):
                    versions.identify_exefs(exefs)
            finally:
                pinned["exefs_sha256"] = saved


if __name__ == "__main__":
    unittest.main()
