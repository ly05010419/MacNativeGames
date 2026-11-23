"""The Arceus seed wrapper: what it asks the translator for, and the seed folder the iOS importer accepts."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import prepare_pla_shader_seed as wrapper   # noqa: E402

KEY = "a" * 64


def control_for(stage_number, c1=0x40, length=0x60, local=(8, 8, 1)):
    control = bytearray(0x880)
    struct.pack_into("<II", control, 0x700, c1, length)
    struct.pack_into("<I", control, 0x714, stage_number)
    struct.pack_into("<III", control, 0x748, *local)
    control[0x718] = 1   # the early-fragment-tests flag the copy reads; this runtime never asks for it
    return bytes(control)


class Requests(unittest.TestCase):
    def setUp(self):
        self.seed = wrapper.load_upstream()
        self.folder = tempfile.TemporaryDirectory(prefix="pla-seed-")
        self.code = bytes(range(0x60))

    def tearDown(self):
        self.folder.cleanup()

    def request(self, stage, number):
        path = Path(self.folder.name) / "x.code"
        answers = dict(tex={}, img={}, attr={}, fout={})
        return self.seed.translation_request(KEY + "-" + "0" * 12, stage, control_for(number), self.code, answers, path), path

    def test_fragment_request_matches_the_runtime(self):
        request, path = self.request("ps", 1)
        self.assertIs(request["depth_minus_w"], False)
        self.assertNotIn("early_fragment_tests", request)
        self.assertNotIn("local", request)
        self.assertEqual(request["c1"], self.code[0x40:0x60].hex())
        self.assertEqual(path.read_bytes(), self.code[0x30:0x60])   # graphics code without its NVN header

    def test_compute_request_keeps_the_header_and_group_size(self):
        request, path = self.request("cs", 5)
        self.assertEqual(request["local"], [8, 8, 1])
        self.assertEqual(path.read_bytes(), self.code[:0x60])

    def test_profile(self):
        self.assertEqual(self.seed.CHAIN, wrapper.runtime_chain())
        self.assertTrue(self.seed.startup_package("anything/at/all.bin"))
        self.assertIsNone(self.seed.GAMES["totk"]["folders"])
        self.assertEqual(self.seed.decoder(None, None)(b"BNSH raw"), b"BNSH raw")


class Folder(unittest.TestCase):
    def test_finish_leaves_only_importable_files(self):
        seed = wrapper.load_upstream()
        with tempfile.TemporaryDirectory(prefix="pla-seed-") as temporary:
            out = Path(temporary) / "shader-seed"
            out.mkdir()
            variant = KEY + "-" + "b" * 12
            (out / f"{KEY}.variants").write_text(json.dumps([dict(variant=variant, deps={})]))
            (out / f"{variant}_b.metal").write_text("// msl")
            (out / f"{variant}_b.json").write_text("{}")
            (out / "pso_manifest.tsv").write_text("# x\n")
            (out / "shader-keys.json").write_text("{}")
            (out / "seed-manifest.json").write_text(json.dumps(dict(schema=1, chain=seed.CHAIN, shaders=1, files={})))
            wrapper.finish(seed, out)
            manifest = wrapper.check_importable(out, seed.CHAIN)
            self.assertFalse((out / "shader-keys.json").exists())
            self.assertEqual(manifest["files"][f"{variant}_b.metal"], hashlib.sha256(b"// msl").hexdigest())
            self.assertEqual(len(manifest["files"]), 4)
            (out / "notes.txt").write_text("x")
            with self.assertRaises(SystemExit):
                wrapper.check_importable(out, seed.CHAIN)


if __name__ == "__main__":
    unittest.main()
