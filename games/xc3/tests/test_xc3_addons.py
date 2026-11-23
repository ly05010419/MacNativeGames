"""The add-on manifest and verified DLC fingerprint matching."""

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "scripts"))

import extract_aoc  # noqa: E402


class AddOnManifestTests(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.addons = json.loads(
            (HERE / "assets/expected-aoc.json").read_text()
        )["addons"]

    def fingerprints(self, index):
        pinned = self.addons[index]
        return [pinned] + pinned.get("alternates", [])

    def test_every_addon_is_pinned_completely(self):
        self.assertEqual(sorted(self.addons, key=int), ["1", "2", "3", "4"])

        for index, pinned in self.addons.items():
            with self.subTest(index=index):
                self.assertEqual(
                    int(pinned["title"], 16),
                    extract_aoc.AOC_BASE + int(index)
                )

                for fingerprint in self.fingerprints(index):
                    with self.subTest(
                        fingerprint=fingerprint["data_nca"]["name"]
                    ):
                        self.assertRegex(
                            fingerprint["data_nca"]["name"],
                            r"^[0-9a-f]{32}\.nca$"
                        )
                        self.assertGreater(
                            fingerprint["data_nca"]["size"],
                            fingerprint["romfs"]["size"]
                        )
                        self.assertRegex(
                            fingerprint["romfs"]["sha256"],
                            r"^[0-9a-f]{64}$"
                        )
                        self.assertGreater(
                            fingerprint["romfs"]["files"], 0
                        )

        self.assertEqual(len(self.addons["3"]["alternates"]), 1)
        self.assertEqual(len(self.addons["4"]["alternates"]), 1)

    def test_primary_and_alternate_fingerprints_are_accepted(self):
        for index in ("3", "4"):
            fingerprints = self.fingerprints(index)

            for fingerprint in fingerprints:
                record = fingerprint["data_nca"]
                measured = {"romfs": fingerprint["romfs"]}

                with self.subTest(
                    addon=index, nca=record["name"]
                ):
                    self.assertTrue(
                        extract_aoc.matches_nca(record, fingerprints)
                    )
                    self.assertTrue(
                        extract_aoc.matches_fingerprint(
                            record, measured, fingerprints
                        )
                    )

    def test_cross_version_fingerprint_pairing_is_rejected(self):
        # DLC4 新舊版 RomFS 指紋不同，不能交叉配對。
        fingerprints = self.fingerprints("4")
        primary, alternate = fingerprints

        self.assertFalse(
            extract_aoc.matches_fingerprint(
                primary["data_nca"],
                {"romfs": alternate["romfs"]},
                fingerprints
            )
        )

        self.assertFalse(
            extract_aoc.matches_fingerprint(
                alternate["data_nca"],
                {"romfs": primary["romfs"]},
                fingerprints
            )
        )

    def test_unknown_nca_and_romfs_are_rejected(self):
        for index in ("3", "4"):
            fingerprints = self.fingerprints(index)
            primary = fingerprints[0]

            unknown_nca = dict(
                primary["data_nca"],
                name="0" * 32 + ".nca"
            )

            with self.subTest(addon=index):
                self.assertFalse(
                    extract_aoc.matches_nca(
                        unknown_nca, fingerprints
                    )
                )

                unknown_romfs = dict(
                    primary["romfs"],
                    sha256="0" * 64
                )

                self.assertFalse(
                    extract_aoc.matches_fingerprint(
                        primary["data_nca"],
                        {"romfs": unknown_romfs},
                        fingerprints
                    )
                )

    def test_addon_titles_follow_the_base_game(self):
        self.assertEqual(
            extract_aoc.AOC_BASE,
            0x010074F013263000
        )


if __name__ == "__main__":
    unittest.main()
