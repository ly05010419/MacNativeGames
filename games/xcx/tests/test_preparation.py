"""A damaged relocation/gate table must never reach the native loader."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
from unittest.mock import patch
import unittest

HERE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("xcx_prepare", HERE / "scripts/prepare_runtime.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class RewriteMetadataTest(unittest.TestCase):
    def test_changed_gate_table_is_rejected(self):
        original = b"0x10 svc 1 0xd4000021\n"
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            folder = work / "rewritten/v100"
            folder.mkdir(parents=True)
            table = folder / "sdk.gates.tsv"
            table.write_bytes(original)
            expected = {"rewrite_metadata_sha256": {table.name: hashlib.sha256(original).hexdigest()}}
            prepare.verify_rewrite_metadata(work, "v100", expected)
            table.write_bytes(original.replace(b"svc 1", b"svc 2"))
            with self.assertRaisesRegex(ValueError, "SHA256 differs"):
                prepare.verify_rewrite_metadata(work, "v100", expected)

    def test_versions_keep_independent_input_pins(self):
        old_dir, old = prepare.version_inputs("1.0.0")
        new_dir, new = prepare.version_inputs("2.0.0")
        self.assertEqual((old_dir, new_dir), ("v100", "v200"))
        self.assertNotEqual(old["exefs_sha256"]["main"], new["exefs_sha256"]["main"])
        with self.assertRaisesRegex(ValueError, "Unsupported XCX version"):
            prepare.version_inputs("1.0.2")

    def test_mislabeled_version_manifest_stops(self):
        with patch.object(prepare.json, "loads", return_value={"display_version": "1.0.0", "title_id": "0100453019AA8000"}):
            with self.assertRaisesRegex(ValueError, "manifest identity differs"):
                prepare.version_inputs("2.0.0")


if __name__ == "__main__":
    unittest.main()
