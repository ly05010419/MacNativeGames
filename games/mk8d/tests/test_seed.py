"""The whole-game shader seed's own parts (scripts/prepare_mk8d_shader_seed.py), without game files: SARC members without a
name, the fix-ups after the shared tool, when a prepared seed is reused, and the chain the runtime checks.

Run: python -m unittest discover -s games/mk8d/tests
"""
from pathlib import Path
import hashlib
import json
import struct
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / "scripts"))
import prepare_mk8d_shader_seed as mk8d_seed  # noqa: E402

KEY = "ab" * 32


def sarc(members, named=True):
    """A little-endian SARC holding members (bytes), each with a name or (named=False) none, as the UI layouts are."""
    count = len(members)
    names = b"".join(f"m{i}.bin".encode().ljust(8, b"\0") for i in range(count)) if named else b""
    header, sfat = 20, 12 + 16 * count
    start = header + sfat + 8 + len(names)
    data, nodes = b"", b""
    for i, member in enumerate(members):
        attribute = (1 << 24 | i * 2) if named else 0
        nodes += struct.pack("<4I", 0, attribute, len(data), len(data) + len(member))
        data += member
    size = start + len(data)
    return (b"SARC" + struct.pack("<HHIIHH", header, 0xFEFF, size, start, 0x100, 0) +
            b"SFAT" + struct.pack("<HHI", sfat, count, 0x65) + nodes + b"SFNT" + struct.pack("<HH", 8, 0) + names + data)


class Sarc(unittest.TestCase):
    def test_unnamed_members_are_read_by_index(self):
        self.assertEqual(mk8d_seed.shared().sarc(sarc([b"one", b"two"], named=False)), {"#0": b"one", "#1": b"two"})

    def test_named_members_are_the_shared_readers(self):
        self.assertEqual(mk8d_seed.shared().sarc(sarc([b"one", b"two"])), {"m0.bin": b"one", "m1.bin": b"two"})

    def test_not_a_sarc_still_fails(self):
        with self.assertRaises(ValueError):
            mk8d_seed.shared().sarc(b"BNSH" + bytes(60))


class Finish(unittest.TestCase):
    def seed(self, folder, chain=None, extra=()):
        variants = {"vs": f"{KEY}-{'1' * 12}", "ps": f"{'cd' * 32}-{'2' * 12}"}
        for stage, variant in variants.items():
            (folder / f"{variant}_b.json").write_text(json.dumps({"stage": stage}))
            (folder / f"{variant}_b.metal").write_text("// msl")
            (folder / f"{variant.split('-')[0]}.variants").write_text(json.dumps([{"variant": variant, "deps": {}}]))
        (folder / "shader-keys.json").write_text("{}")
        (folder / "pso_manifest.tsv").write_text("# rows\n")
        (folder / "seed-manifest.json").write_text(json.dumps({"chain": chain or mk8d_seed.runtime_chain(), "files": {}}))
        for name in extra:
            (folder / name).write_text("x")
        return variants

    def test_vertex_rows_record_the_depth_mode_and_the_manifest_lists_every_file(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            variants = self.seed(folder)
            vertex, files = mk8d_seed.finish(folder, mk8d_seed.runtime_chain(), "p" * 64)
            self.assertEqual((vertex, files), (1, 7))
            self.assertIs(json.loads((folder / f"{KEY}.variants").read_text())[0]["depth_minus_w"], True)
            self.assertNotIn("depth_minus_w", json.loads((folder / f"{'cd' * 32}.variants").read_text())[0])
            self.assertFalse((folder / "shader-keys.json").exists(), "the importer rejects a seed holding it")
            manifest = json.loads((folder / "seed-manifest.json").read_text())
            name = f"{variants['vs']}_b.metal"
            self.assertEqual(manifest["files"][name], hashlib.sha256((folder / name).read_bytes()).hexdigest())
            self.assertEqual(manifest["mk8d_preparation_sha256"], "p" * 64)

    def test_another_chain_or_a_stray_file_stops(self):
        with tempfile.TemporaryDirectory() as folder:
            self.seed(Path(folder), chain="totk-native-v7-unverified")
            with self.assertRaises(SystemExit):
                mk8d_seed.finish(Path(folder), mk8d_seed.runtime_chain(), "p" * 64)
        with tempfile.TemporaryDirectory() as folder:
            self.seed(Path(folder), extra=["notes.txt"])
            with self.assertRaises(SystemExit):
                mk8d_seed.finish(Path(folder), mk8d_seed.runtime_chain(), "p" * 64)

    def test_a_seed_is_reused_only_from_the_same_inputs_with_its_files(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            self.seed(folder)
            mk8d_seed.finish(folder, mk8d_seed.runtime_chain(), "p" * 64)
            manifest = json.loads((folder / "seed-manifest.json").read_text())
            manifest.update(translator_sha256="t" * 64)
            (folder / "seed-manifest.json").write_text(json.dumps(manifest))
            same = dict(chain=mk8d_seed.runtime_chain(), translator_sha256="t" * 64, mk8d_preparation_sha256="p" * 64)
            self.assertTrue(mk8d_seed.up_to_date(folder, same))
            self.assertFalse(mk8d_seed.up_to_date(folder, {**same, "translator_sha256": "u" * 64}))
            next(folder.glob("*_b.metal")).unlink()
            self.assertFalse(mk8d_seed.up_to_date(folder, same))


class Chain(unittest.TestCase):
    def test_the_seed_and_the_list_use_the_runtimes_chain(self):
        chain = mk8d_seed.runtime_chain()
        self.assertEqual(mk8d_seed.shared().CHAIN, chain)
        self.assertIn(f"Translation chain {chain} (games/mk8d)",
                      (GAME / "third_party/totk-scripts/shader_list.py").read_text())


if __name__ == "__main__":
    unittest.main()
