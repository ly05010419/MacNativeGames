"""A damaged relocation/gate table must never reach the native loader."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

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
            with patch.object(prepare, "EXPECTED", expected):
                prepare.verify_rewrite_metadata(work)
                table.write_bytes(original.replace(b"svc 1", b"svc 2"))
                with self.assertRaisesRegex(ValueError, "SHA256 differs"):
                    prepare.verify_rewrite_metadata(work)


if __name__ == "__main__":
    unittest.main()
