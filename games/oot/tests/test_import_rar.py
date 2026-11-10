"""Check that archive extents can arrive in a different order than the RomFS index."""
import hashlib
from io import BytesIO
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import import_rar


def digest(data):
    return hashlib.sha256(data).digest()


class Reader:
    super = {"toc_sha256": bytes(32)}
    files = [
        (b"second", 1, digest(b"B"), 128, 1, 0),
        (b"first", 1, digest(b"A"), 129, 1, 0),
        (b"main", 1, digest(b"M"), 130, 1, 0),
    ]

    def __init__(self, path):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass


class StreamImport(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / "assets").mkdir()
        self.image = b"A" + bytes(15) + b"B"
        (self.root / "assets/expected-v131072.json").write_text(json.dumps({
            "exefs_sha256": {"main": digest(b"M").hex()},
            "romfs": {"size": len(self.image), "sha256": digest(self.image).hex()},
        }))
        self.output = self.root / "out"
        self.scratch = self.root / "scratch"
        self.scratch.mkdir()

    def run_import(self, tail, finish=lambda: None):
        with (
            patch.object(import_rar, "ROOT", self.root),
            patch.object(import_rar, "NupReader", Reader),
            patch.object(import_rar.nup_format, "parse_super",
                         return_value={"toc_offset": 128, "toc_size": 0, "file_size": 131}),
            patch.object(import_rar, "package_files",
                         return_value=([("exefs/main", 2)], [("first", 1), ("second", 0)], [])),
        ):
            import_rar.copy_archive(BytesIO(bytes(128) + tail), Path("fixture.part1.rar"),
                                    self.output, self.scratch, finish)

    def test_publishes_verified_image_in_index_order(self):
        self.run_import(b"BAM")
        self.assertEqual((self.output / "romfs/romfs.bin").read_bytes(), self.image)
        self.assertEqual((self.output / "exefs/main").read_bytes(), b"M")
        index = json.loads((self.output / "romfs/index.json").read_text())
        self.assertEqual([row["offset"] for row in index["files"]], [0, 16])

    def test_truncation_leaves_no_output(self):
        with self.assertRaisesRegex(ValueError, "ended before"):
            self.run_import(b"BA")
        self.assertFalse(self.output.exists())

    def test_failed_archive_crc_leaves_no_output(self):
        def failed():
            raise ValueError("archive checksum failed")

        with self.assertRaisesRegex(ValueError, "archive checksum failed"):
            self.run_import(b"BAM", failed)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
