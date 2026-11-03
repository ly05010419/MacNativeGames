import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

from compression import zstd


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/prepare_shader_seed.py"
SPEC = importlib.util.spec_from_file_location("kirby_seed", SCRIPT)
SEED = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SEED)


def control(nvn_stage, length, c1=0x30):
    data = bytearray(0x880)
    struct.pack_into("<II", data, 0x700, c1, length)
    struct.pack_into("<I", data, 0x714, nvn_stage)
    return bytes(data)


def bnsh(stages):
    """A BNSH with one program whose stages (name -> (control, code)) are laid out after the header."""
    blob = bytearray(0x200)
    blob[0:4] = b"BNSH"
    blob[12:14] = b"\xff\xfe"
    blob[96:100] = b"grsc"
    struct.pack_into("<IQ", blob, 124, 1, 0x100)          # one variation at 0x100
    struct.pack_into("<Q", blob, 0x100 + 16, 0x140)       # its program at 0x140
    for name, (ctl, code) in stages.items():
        record = len(blob)
        blob += bytes(0x20)
        struct.pack_into("<Q", blob, 0x140 + {"vs": 8, "ps": 40, "cs": 48}[name], record)
        struct.pack_into("<QQII", blob, record + 8, len(blob), len(blob) + len(ctl), len(code), len(ctl))
        blob += ctl + code
    struct.pack_into("<I", blob, 28, len(blob))
    return bytes(blob)


class FindingShaders(unittest.TestCase):
    def test_keys_are_the_runtimes(self):
        vs = control(0, 0x40) + b"tail beyond the key"
        code = bytes(range(0x40))
        blob = bnsh({"vs": (vs, code)})
        found = list(SEED.bnsh_shaders(b"pad" + blob, 3))
        self.assertEqual(len(found), 1)
        key, stage, ctl, got = found[0]
        self.assertEqual(stage, "vs")
        self.assertEqual(ctl, vs[:0x880])
        self.assertEqual(key, hashlib.sha256(vs[:0x880] + code).hexdigest())

    def test_control_size_word_bounds_the_key(self):
        ctl = bytearray(control(1, 0x40))
        struct.pack_into("<I", ctl, 0x20, 0x848)
        self.assertEqual(SEED.control_key_len(bytes(ctl)), 0x848)
        struct.pack_into("<I", ctl, 0x20, 0x10)
        self.assertEqual(SEED.control_key_len(bytes(ctl)), 0x880)

    def test_wrong_stage_is_an_error(self):
        blob = bnsh({"ps": (control(0, 0x40), bytes(0x40))})
        with self.assertRaises(ValueError):
            list(SEED.bnsh_shaders(blob, 0))

    def test_size_prefixed_zstd_container(self):
        payload = b"BNSH inside" * 20
        self.assertEqual(SEED.unpacked(struct.pack("<I", len(payload)) + zstd.compress(payload)), payload)
        self.assertEqual(SEED.unpacked(zstd.compress(payload)), payload)
        with self.assertRaises(ValueError):
            SEED.unpacked(struct.pack("<I", len(payload) + 1) + zstd.compress(payload))
        self.assertEqual(SEED.unpacked(b"plain file"), b"plain file")


class BindStates(unittest.TestCase):
    def test_variant_name_matches_the_runtime(self):
        # totk_rt_variant: SHA-256 of the sorted, compact answers, with depth_minus_w for vertex shaders.
        key = "ab" * 32
        asked = '{"attr":{},"depth_minus_w":true,"fout":{},"img":{},"tex":{}}'
        self.assertEqual(SEED.variant_name(key, "vs", SEED.EMPTY), f"{key}-{hashlib.sha256(asked.encode()).hexdigest()[:12]}")
        self.assertNotEqual(SEED.variant_name(key, "ps", SEED.EMPTY), SEED.variant_name(key, "vs", SEED.EMPTY))

    def test_deps_match(self):
        self.assertTrue(SEED.deps_match({"tex": {"0": "*"}, "fout": {"0": "float"}}, {"tex": {"0": "2d"}}))
        self.assertFalse(SEED.deps_match({"tex": {"0": "*"}}, {"tex": {"0": "2d_depth"}}))
        self.assertTrue(SEED.deps_match({"tex": {"0": "*_depth"}}, {"tex": {"0": "2darray_depth"}}))
        self.assertFalse(SEED.deps_match({"fout": {"5": "float"}}, {"fout": {"5": "uint"}}))
        self.assertTrue(SEED.deps_match({"attr": {"3": "uint"}}, {"attr": {}}))   # an unbound input is not compared

    def test_follow_ups(self):
        gbuffer = {"deps": {"fout": {str(i): "float" for i in range(6)}}, "compare_slots": [4]}
        extra = SEED.follow_ups("k", "ps", gbuffer)
        self.assertEqual(len(extra), 3)
        self.assertIn({"tex": {"4": "2d_depth"}, "img": {}, "attr": {}, "fout": {"5": "uint"}}, extra)
        self.assertEqual(SEED.follow_ups("k", "ps", {"deps": {"fout": {"0": "float"}}}), [])
        self.assertEqual(SEED.follow_ups("k", "vs", {"deps": {"fout": {"5": "float"}}}), [])

    def test_listed_answers(self):
        answers = SEED.answers_for("ps", {"tex": {"1": "*_depth", "2": "*", "3": "3d"}})
        self.assertEqual(answers["tex"], {"1": "2d_depth", "2": "2d", "3": "3d"})
        self.assertEqual(SEED.answers_for("vs", {})["attr"]["15"], "float")


class ListFromPlayedCache(unittest.TestCase):
    def test_adds_only_unserved_states(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            seed, played, listed = root / "seed", root / "played", root / "list.tsv"
            seed.mkdir(); played.mkdir()
            key, other = "a" * 64, "b" * 64
            (seed / f"{key}.variants").write_text(json.dumps([{"variant": key + "-0", "deps": {"tex": {"0": "*"}}}]))
            (played / f"{key}.variants").write_text(json.dumps([
                {"variant": key + "-1", "deps": {"tex": {"0": "*"}}},            # served by the seed
                {"variant": key + "-2", "deps": {"tex": {"0": "*_depth"}}}]))    # not served: listed
            (played / f"{other}.variants").write_text(json.dumps([{"variant": other + "-1", "deps": {}}]))   # not in the game
            SEED.add_listed(seed, played, listed)
            self.assertEqual(SEED.read_list(listed), [(key, {"tex": {"0": "*_depth"}})])
            SEED.add_listed(seed, played, listed)   # idempotent
            self.assertEqual(len(SEED.read_list(listed)), 1)


if __name__ == "__main__":
    unittest.main()
