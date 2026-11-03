"""patch_sites.py: the sites the runtime and the rewriter patch themselves, read out of the C sources (no game data).

The known offsets below are the anchors: if a table moves in game_version.h / game_patches*.c / patch_nso.py and the
extraction stops following it, one of these fails instead of the build silently accepting a mod patch that lands on
a site the runtime writes over.
"""
import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
import patch_sites            # noqa: E402

BUILD_ID_V0 = 'db0507f36b84200ac05f75d76ed5026f'      # the build ID patch_nso.py's tables are keyed on
SIGNED_BUILD_ID = '277178b7dba1b6d4949a84778d4abc58'  # the one SIGNED_TABLES is keyed on
OTHER = '00' * 16


def offsets(version, build_id=None):
    return {site.offset for site in patch_sites.sites(version, build_id)}


class Extraction(unittest.TestCase):
    def test_every_version_resolves_all_of_its_game_addr_names(self):
        patch_sites._unresolved.clear()
        for version in patch_sites.VERSIONS:
            patch_sites.sites(version, BUILD_ID_V0)
        self.assertEqual(sorted(patch_sites._unresolved), [], 'game_version.h no longer defines these sites')

    def test_v0_sites(self):
        found = offsets('v0', BUILD_ID_V0)
        self.assertIn(0x8f6c60, found)                  # 60 FPS, clamp (game_patches.c fps60_sites)
        self.assertIn(0x7a83f4, found)                  # real-time frame step
        self.assertIn(0xe996ac, found)                  # output resolution
        self.assertIn(0xc630c4, found)                  # render resolution
        self.assertIn(0x7b5988, found)                  # cloth/hair clock, the 1.0.0 table
        self.assertIn(0x7b6358, found)                  # ... and its shared update
        self.assertNotIn(0x82192c, found)               # a 1.2.1 cloth address: not a site of 1.0.0
        self.assertNotIn(0x9d1e64, found)               # a 1.1.2 cloth address
        self.assertNotIn(0xce3fec + 1, found)           # only the instruction itself is a site

    def test_the_20_byte_build_id_the_build_passes_is_the_same_list(self):
        # mod_exefs.py reads the 20 bytes the runtime compares; the tables are keyed on the first 16, and the
        # rewriter's cuts must still arrive (a 40-hex id used to match nothing and silently drop them).
        for version in patch_sites.VERSIONS:
            self.assertEqual(offsets(version, BUILD_ID_V0), offsets(version, BUILD_ID_V0 + '00000000'))
        self.assertIn(0x8524c4, offsets('v0', BUILD_ID_V0 + '00000000'))      # CUTS: the video-memory budget word
        self.assertIn(0x1d53e80, offsets('v0', BUILD_ID_V0 + '00000000'))     # CUTS: a 17-byte cut

    def test_the_signed_jump_tables_are_reserved(self):
        # patch_nso.py 1.6.0/1.0.0 signed-table dispatch: a mod patch on the dispatch instruction or on the table it
        # reads moves a jump the rewriter relies on, and the rewriter's own assert would fire mid-rewrite.
        found = {(site.offset, site.size) for site in patch_sites.sites('v0', SIGNED_BUILD_ID)}
        # Every table in 1.4.3's block, not just the first: the dispatch with the 16 bytes patch_nso.py asserts with it,
        # and the offsets it jumps through (which may be negative - one table has -7264 among them).
        self.assertEqual(len(patch_sites.signed_table_sites(SIGNED_BUILD_ID)), 10)
        self.assertIn((0x393ba0, 0x14), found)          # IMG[dispatch-0x10:dispatch+4] is what the rewriter asserts
        self.assertIn((0x2d8626c, 24), found)           # the six signed offsets it jumps through
        self.assertIn((0x2d88e7c, 28), found)           # the table with negative offsets, 7 entries

    def test_another_versions_block_is_not_reserved(self):
        # game_patches.c's ultrawide list is in the 1.2.1 branch only: a 1.0.0 / 1.1.2 mod patch there is unrelated
        # code and must not be refused with "the app patches this".
        self.assertNotIn(0xcb4b10, offsets('v0'))
        self.assertNotIn(0x12521cc, offsets('v0'))
        self.assertIn(0xcb4b10, offsets('v121'))        # ... and its own version still has it
        self.assertIn(0x19b36f0, offsets('v121'))

    def test_v121_sites_include_the_ultrawide_and_cloth_sites(self):
        found = offsets('v121', BUILD_ID_V0)
        self.assertIn(0x8f273c, found)                  # 0x8f6c60 in 1.2.1
        self.assertIn(0xcb4b10, found)                  # ultrawide stub site (aspect_install)
        self.assertIn(0x82192c, found)                  # the 1.2.1 cloth table
        self.assertIn(0x89822c, found)                  # its shared update
        self.assertIn(0xbda798, found)                  # the slow-motion bounce

    def test_v143_uses_its_own_patch_source(self):
        found = offsets('v143', BUILD_ID_V0)
        self.assertIn(0x2db204, found)                  # 1.4.3's 60 FPS table
        self.assertIn(0x246eac, found)                  # 1.4.3's cloth veneer site
        self.assertIn(0x585e48, found)                  # render resolution
        self.assertIn(0x628c74, found)                  # ultrawide
        self.assertNotIn(0x8f6c60, found)               # game_patches.c is not compiled for 1.4.3

    def test_rewriter_tables_need_the_build_id(self):
        without = offsets('v0')
        with_id = offsets('v0', BUILD_ID_V0)
        self.assertNotIn(0x8524c4, without)
        self.assertIn(0x8524c4, with_id)                # CUTS: the video-memory budget word
        self.assertIn(0x1d53e80, with_id)               # CUTS: a 17-byte cut
        self.assertIn(0xb1dc60, with_id)                # OBS: an observation gate
        self.assertIn(0xe12d8c, with_id)                # PARAMS: an output-resolution parameter site
        self.assertNotIn(0x8524c4, offsets('v0', OTHER))

    def test_a_cut_keeps_its_length(self):
        found = {site.offset: site for site in patch_sites.sites('v0', BUILD_ID_V0)}
        self.assertEqual(found[0x1d53e80].size, 17)
        self.assertEqual(found[0x8524c4].size, 4)

    def test_instruction_words_are_not_sites(self):
        for version in patch_sites.VERSIONS:
            found = offsets(version, BUILD_ID_V0)
            for word in (0x1e220020, 0x6b08015f, 0x7100055f, 0xd280f008):
                self.assertNotIn(word, found, f'{version}: an instruction word was read as an address')

    def test_sites_are_sorted_and_unique(self):
        found = patch_sites.sites('v121', BUILD_ID_V0)
        self.assertEqual(found, sorted(found, key=lambda site: (site.offset, site.size)))
        self.assertEqual(len({(site.offset, site.size) for site in found}), len(found))

    def test_overlaps(self):
        reserved = patch_sites.sites('v121', BUILD_ID_V0)
        self.assertTrue(patch_sites.overlaps([(0x8f273c, 4)], reserved))
        self.assertTrue(patch_sites.overlaps([(0x8f273a, 4)], reserved))        # half on each side
        self.assertFalse(patch_sites.overlaps([(0x8f2744, 4)], reserved))
        self.assertFalse(patch_sites.overlaps([], reserved))

    def test_the_command_line_prints_json(self):
        done = subprocess.run([sys.executable, str(ROOT/'games/totk/scripts/patch_sites.py'),
                               '--version', 'v121', '--json'], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)
        listed = json.loads(done.stdout)
        self.assertIn('0x8f273c', [site['offset'] for site in listed])
        self.assertTrue(all(site['size'] > 0 for site in listed))


if __name__ == '__main__':
    unittest.main()
