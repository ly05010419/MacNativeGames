"""ACNH preparation must fail closed on wrong builds and unsafe output paths."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import struct
import unittest
from unittest.mock import patch

GAME_DIR = Path(__file__).resolve().parents[1]
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("acnh_prepare", GAME_DIR / "prepare.py")
acnh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acnh)
from games.acnh.tools.runtime_preflight import parse_dynamic


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.expected = acnh.profile(458752)
        self.identity = {key: self.expected[key] for key in
                         ("title_id", "package_kind", "cnmt_version_raw")}
        self.manifest = {"title_id": acnh.TITLE_ID, "files": [
            {"name": name, "sha256": digest} for name, digest in self.expected["exefs_sha256"].items()]}
        self.modules = [{"module": row["name"], "build_id": row["build_id"],
                         "segment_hashes_verified": True} for row in self.expected["modules"]]

    def test_accepts_exact_observed_identity(self):
        acnh.check_identity(self.identity, self.expected)
        acnh.check_exefs(self.manifest, self.expected)
        acnh.check_modules(self.modules, self.expected)

    def test_rejects_other_title_version_and_base_in_update_slot(self):
        for key, value in (("title_id", "01004D300C5AE000"), ("cnmt_version_raw", 0),
                           ("cnmt_version_raw", None), ("package_kind", "base")):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                acnh.check_identity({**self.identity, key: value}, self.expected)

    def test_rejects_changed_npdm_even_with_same_program(self):
        next(row for row in self.manifest["files"] if row["name"] == "main.npdm")["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "Unknown ExeFS"):
            acnh.check_exefs(self.manifest, self.expected)

    def test_rejects_missing_extra_or_duplicate_exefs_files(self):
        for rows in (self.manifest["files"][:-1],
                     self.manifest["files"] + [{"name": "subsdk9", "sha256": "0" * 64}],
                     self.manifest["files"] + [self.manifest["files"][0]]):
            with self.subTest(rows=len(rows)), self.assertRaises(ValueError):
                acnh.check_exefs({**self.manifest, "files": rows}, self.expected)

    def test_rejects_module_identity_and_integrity_mismatches(self):
        for key, value in (("build_id", "0" * 64), ("segment_hashes_verified", False)):
            modules = copy.deepcopy(self.modules)
            modules[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                acnh.check_modules(modules, self.expected)
        with self.assertRaises(ValueError):
            acnh.check_modules(self.modules[:-1], self.expected)

    def test_rejects_output_in_any_checkout(self):
        roots = acnh.outside_repository.__globals__["_git_worktree_roots"](ROOT)
        for root in roots:
            with self.subTest(root=root), self.assertRaisesRegex(ValueError, "outside the source"):
                acnh.outside_repository(root / "acnh-generated", ROOT)

    def test_rejects_symlink_back_into_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            link = Path(temporary) / "source"
            link.symlink_to(ROOT, target_is_directory=True)
            with self.assertRaises(ValueError):
                acnh.outside_repository(link / "generated", ROOT)

    def test_existing_work_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            marker = work / "keep.txt"
            marker.write_text("existing data")
            with self.assertRaisesRegex(ValueError, "already exists"):
                acnh.prepare(work / "missing.nsp", work / "missing.nsp", work / "keys", work)
            self.assertEqual(marker.read_text(), "existing data")

    def test_failed_stage_does_not_mark_prepared(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = [root / name for name in ("base.nsp", "update.nsp", "prod.keys")]
            for path in inputs:
                path.touch()
            identities = [{key: acnh.profile(version)[key] for key in self.identity} for version in (0, 458752)]
            work = root / "work"
            with patch.object(acnh, "select_program", side_effect=identities), \
                 patch.object(acnh, "run_step", side_effect=ValueError("extraction failed")), \
                 self.assertRaisesRegex(ValueError, "extraction failed"):
                acnh.prepare(*inputs, work)
            report = json.loads((work / "preparation.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["runtime"], "not_assessed")

    def test_repeatable_needed_tags_do_not_hide_singleton_duplicates(self):
        image = bytearray(256)
        struct.pack_into("<I", image, 4, 32)
        image[32:36] = b"MOD0"
        struct.pack_into("<i", image, 36, 32)
        for index, row in enumerate(((1, 1), (1, 15), (5, 192), (10, 32), (0, 0))):
            struct.pack_into("<qQ", image, 64 + index * 16, *row)
        layout = {"KEY_PAGE_OFF": 256, "DATA_OFF": 64}
        self.assertEqual(parse_dynamic(image, layout), {5: 192, 10: 32})
        struct.pack_into("<qQ", image, 64, 5, 192)
        with self.assertRaisesRegex(ValueError, "Duplicate dynamic tag: 5"):
            parse_dynamic(image, layout)


if __name__ == "__main__":
    unittest.main()
