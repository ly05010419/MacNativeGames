"""Reject mismatched speed-correction inputs and exercise FPS mode transitions."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from games.acnh.tools.prepare_fps import apply_ips, check_replacement, parse_pchtxt

GAME = Path(__file__).resolve().parents[1]


class InputTests(unittest.TestCase):
    def test_ips_records_and_bounds(self):
        patch = b"PATCH\x00\x00\x01\x00\x02ab\x00\x00\x04\x00\x00\x00\x03xEOF"
        self.assertEqual(apply_ips(b"01234567", patch), b"0ab3xxx7")
        for bad in (patch[:-1], patch + b"\0\0\0", b"PATCH\0\0\xff\0\1xEOF", b"PATCH\0\0\0\0\2x"):
            with self.subTest(patch=bad), self.assertRaises(ValueError):
                apply_ips(b"01234567", bad)

    def test_patch_identity_directives_and_alignment(self):
        bid = "1" * 32
        text = f"@nsobid-{bid}\n@flag offset_shift 0x100\n@enabled\n00000008 1F2003D5\n@stop\n"
        self.assertEqual(parse_pchtxt(text, bid), [(8, bytes.fromhex("1f2003d5"))])
        for bad in (text.replace(bid, "2" * 32), text.replace("00000008", "00000009"),
                    text.replace("@stop", ""), text.replace("1F2003D5", "1F20"), text + "00000000 1F2003D5\n"):
            with self.subTest(text=bad), self.assertRaises(ValueError):
                parse_pchtxt(bad, bid)

    def test_replacements_obey_rewriter_rules(self):
        original = bytes(64)
        rewritten = original[:32] + b"\x01\x00\x00\x14" + original[36:]   # word 32 became a gate branch
        check_replacement(0, 0xD503201F, original, rewritten, 64)        # nop
        check_replacement(0, 0x52800308, original, rewritten, 64)        # mov w8, #0x18 is not x18
        check_replacement(0, 0x14000004, original, rewritten, 64)        # b +16 into original code
        check_replacement(80, 0xD4000001, original, rewritten, 64)       # data words are not executed
        for word in (0xD4000001, 0xD53BD040, 0xD51BD040, 0xD53BE020, 0xAA1203E0, 0x14000008, 0x14000040):
            with self.subTest(word=hex(word)), self.assertRaises(ValueError):
                check_replacement(0, word, original, rewritten, 64)

@unittest.skipUnless(shutil.which("xcrun"), "needs the Xcode command line tools")
class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.assets = self.root / "assets"
        (self.assets / "romfs").mkdir(parents=True)
        (self.assets / "manifest.json").write_bytes(b"synthetic identity\n")
        (self.assets / "patches.tsv").write_text("MNG_ACNH_FPS1\n0 00000001 0000000a\n8 00000003 0000001e\n")
        (self.assets / "romfs/manifest.tsv").write_bytes(b"synthetic overlay identity\n")
        self.settings = self.root / "frame-rate.txt"
        self.binary = self.root / "fps_probe"
        self.env = dict(os.environ, ACNH_FPS_ASSETS=str(self.assets), TEST_SETTINGS=str(self.root))
        self.env.pop("ACNH_FPS", None)
        header = "#define ACNH_FPS_ENABLED 1\n#define ACNH_FPS_PATCH_COUNT 2\n"
        for key, file in (("MANIFEST", "manifest.json"), ("PATCH", "patches.tsv"), ("ROMFS", "romfs/manifest.tsv")):
            sha = hashlib.sha256((self.assets / file).read_bytes()).hexdigest()
            header += f'#define ACNH_FPS_{key}_SHA "{sha}"\n'
        (self.root / "fps_config.h").write_text(header)

    def compile(self, version=2097152):
        result = subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                 "-Wno-deprecated-declarations", f"-DACNH_VERSION_RAW={version}",
                                 "-I", str(self.root), "-I", str(GAME / "runtime"),
                                 str(GAME / "tests/fps_probe.c"), str(GAME / "runtime/frame_rate.c"),
                                 "-o", str(self.binary)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def run_probe(self, rate, operation="check", expected=0):
        result = subprocess.run([str(self.binary), str(rate), operation], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, expected, result.stderr)
        return result

    def test_choice_is_persisted_and_restarts_both_modes(self):
        self.compile()
        self.run_probe(30, "choose")
        self.assertEqual(self.settings.read_text(), "60\n")
        self.run_probe(60, "choose")
        self.assertEqual(self.settings.read_text(), "30\n")
        self.run_probe(30)

    def test_every_original_is_checked_before_any_write(self):
        self.compile()
        self.settings.write_text("60\n")
        result = self.run_probe(60, "mismatch", 77)
        self.assertIn("original instruction mismatch", result.stderr)

    def test_corrupt_patch_fails_integrity_check(self):
        self.compile()
        self.settings.write_text("60\n")
        with (self.assets / "patches.tsv").open("a") as f:
            f.write("c 00000004 00000028\n")
        self.assertIn("integrity mismatch", self.run_probe(60, expected=77).stderr)

    def test_invalid_choice_and_missing_assets_keep_original_mode(self):
        self.compile()
        self.settings.write_text("120\n")
        self.run_probe(30)
        self.settings.write_text("60\n")
        self.env.pop("ACNH_FPS_ASSETS")
        self.run_probe(30, "unlocked")
        self.env["ACNH_FPS"] = "120"
        self.assertIn("must be 30 or 60", self.run_probe(30, expected=77).stderr)

    def test_121_never_installs_301_corrections(self):
        self.compile(458752)
        self.settings.write_text("60\n")
        self.run_probe(30, "unlocked")


if __name__ == "__main__":
    unittest.main()
