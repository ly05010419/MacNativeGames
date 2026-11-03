"""tools/prepare_shader_seed.py: shaders found in game containers keyed as the runtime keys them, and translation
requests and variant names the runtime would make (totk_shader_rt.m totk_rt_variant). Synthetic data only."""
import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / "tools"))
import prepare_shader_seed as seed  # noqa: E402


def control(stage, c1=0x40, length=0x80, key_size=0):
    c = bytearray(0x88c)   # some archives carry 0x88c bytes; the key hashes 0x880 of them
    struct.pack_into("<I", c, 0x20, key_size)
    struct.pack_into("<II", c, 0x700, c1, length)
    struct.pack_into("<I", c, 0x714, seed.STAGES[stage])
    struct.pack_into("<III", c, 0x748, 8, 4, 1)
    c[0x100] = 0x5a   # something to tell programs apart
    return bytes(c)


def bnsh(programs):
    """A BNSH file with one program per (stage, control, code): header, variation table, programs, stage records, data."""
    out = bytearray(0x200)
    out[0:8] = seed.BNSH; out[12:14] = b"\xff\xfe"; out[96:100] = b"grsc"
    variations = len(out); out += bytes(64 * len(programs))
    struct.pack_into("<IQ", out, 124, len(programs), variations)
    for i, (stage, ctl, code) in enumerate(programs):
        program = len(out); out += bytes(56)
        struct.pack_into("<Q", out, variations + i * 64 + 16, program)
        record = len(out); out += bytes(32)
        struct.pack_into("<Q", out, program + {"vs": 8, "ps": 40, "cs": 48}[stage], record)
        ctl_at = len(out); out += ctl
        code_at = len(out); out += code
        struct.pack_into("<QQII", out, record + 8, ctl_at, code_at, len(code), len(ctl))
    struct.pack_into("<I", out, 28, len(out))
    return bytes(out)


class ShaderSeedTests(unittest.TestCase):
    def test_keys_are_the_runtimes(self):
        vs, ps = control("vs"), control("ps", key_size=0x848)
        code = bytes(range(256)) * 2
        data = b"junk" * 9 + bnsh([("vs", vs, code), ("ps", ps, code)]) + b"tail"
        found = {}
        seed.scan(data, found, "Shader")
        self.assertEqual(found[hashlib.sha256(vs[:0x880] + code[:0x80]).hexdigest()][0], "vs")
        self.assertEqual(found[hashlib.sha256(ps[:0x848] + code[:0x80]).hexdigest()][0], "ps")   # the section's own size
        self.assertEqual(len(found), 2)

    def test_a_wrong_stage_is_not_a_shader(self):
        found = {}
        seed.scan(bnsh([("ps", control("vs"), bytes(256))]), found, "Shader")
        self.assertEqual(found, {})

    def test_variant_names_and_requests_follow_the_runtime(self):
        key = "ab" * 32
        default = json.dumps(dict(attr={}, fout={}, img={}, tex={}), separators=(",", ":"))
        vertex = json.dumps(dict(attr={}, depth_minus_w=True, fout={}, img={}, tex={}), separators=(",", ":"))
        self.assertEqual(seed.variant_name(key, "ps"), key + "-" + hashlib.sha256(default.encode()).hexdigest()[:12])
        self.assertEqual(seed.variant_name(key, "vs"), key + "-" + hashlib.sha256(vertex.encode()).hexdigest()[:12])
        self.assertRegex(seed.variant_name(key, "cs") + "_b.metal", seed.SEED_FILE)
        code = bytes(range(256))
        with tempfile.TemporaryDirectory() as tmp:
            rq = seed.request("v", "vs", control("vs"), code, Path(tmp) / "v.code")
            self.assertEqual(Path(rq["code"]).read_bytes(), code[0x30:0x80])   # graphics code without the NVN header
            self.assertEqual(rq["c1"], code[0x40:0x80].hex())
            self.assertIs(rq["depth_minus_w"], True)
            self.assertNotIn("local", rq)
            rq = seed.request("c", "cs", control("cs"), code, Path(tmp) / "c.code")
            self.assertEqual(Path(rq["code"]).read_bytes(), code[:0x80])   # compute keeps it
            self.assertEqual(rq["local"], [8, 4, 1])
            self.assertIs(rq["depth_minus_w"], False)

    def test_shadow_map_variant(self):
        answers = seed.depth_answers([16, 4])
        self.assertEqual(answers["tex"], {"16": "2d_depth", "4": "2d_depth"})
        self.assertEqual(seed.ANSWERS["tex"], {})   # the default is left alone
        named = json.dumps(dict(attr={}, fout={}, img={}, tex={"16": "2d_depth", "4": "2d_depth"}), sort_keys=True, separators=(",", ":"))
        self.assertEqual(seed.variant_name("cd" * 32, "ps", answers), "cd" * 32 + "-" + hashlib.sha256(named.encode()).hexdigest()[:12])
        with tempfile.TemporaryDirectory() as tmp:
            rq = seed.request("v", "ps", control("ps"), bytes(256), Path(tmp) / "v.code", answers)
            self.assertEqual(rq["tex"], {"16": "2d_depth", "4": "2d_depth"})

    def test_chain_is_the_runtimes(self):
        self.assertIn(f'"{seed.CHAIN}"', (GAME / "third_party/d2r_gfx/gfx/nvn/dmsh_chain.h").read_text())


if __name__ == "__main__":
    unittest.main()
