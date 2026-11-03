"""The add-on manifest (assets/expected-aoc.json): its shape, and that every entry is one of this game's add-on titles."""
import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "scripts"))
import extract_aoc  # noqa: E402


class AddOnManifestTests(unittest.TestCase):
    def test_every_addon_is_pinned_completely(self):
        addons = json.loads((HERE / "assets/expected-aoc.json").read_text())["addons"]
        self.assertEqual(sorted(addons, key=int), ["1", "2", "3", "4"])
        for index, pinned in addons.items():
            with self.subTest(index=index):
                self.assertEqual(int(pinned["title"], 16), extract_aoc.AOC_BASE + int(index))
                self.assertRegex(pinned["data_nca"]["name"], r"^[0-9a-f]{32}\.nca$")
                self.assertGreater(pinned["data_nca"]["size"], pinned["romfs"]["size"])
                self.assertRegex(pinned["romfs"]["sha256"], r"^[0-9a-f]{64}$")
                self.assertGreater(pinned["romfs"]["files"], 0)

    def test_addon_titles_follow_the_base_game(self):
        self.assertEqual(extract_aoc.AOC_BASE, 0x010074F013263000)


if __name__ == "__main__":
    unittest.main()
