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
        self.assertNotIn(0xcb4b10, offsets('v112'))
        self.assertIn(0xcb4b10, offsets('v121'))        # ... and its own version still has it
        self.assertIn(0x19b36f0, offsets('v121'))

    def test_v112_sites_follow_game_version_h(self):
        found = offsets('v112', BUILD_ID_V0)
        self.assertIn(0x8f66e4, found)                  # 0x8f6c60 in 1.1.2 (the GAME_ADDR table)
        self.assertIn(0x7f7a38, found)                  # 0x7a83f4 in 1.1.2
        self.assertIn(0x9d1e64, found)                  # the 1.1.2 cloth table
        self.assertNotIn(0x82192c, found)
        self.assertNotIn(0x8f6c60, found)               # that address is 1.0.0's, not 1.1.2's

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

    def test_botw190_sites_follow_game_patches_botw_c(self):
        # Breath of the Wild 1.9.0: the rows {offset, word, ...} of the branch GAME_BOTW190 compiles, and nothing of 1.6.0's.
        found = offsets('botw190')
        for site in (0x151d484, 0x151d490,              # the VFR ratio (real-time frame step), first and last word
                     0x125f118, 0x1280ecc, 0x128106c,   # the fade, the UI step, the physics ratio
                     0xeb0f78, 0xeb0f88,                # the pad repeat count
                     0x12660c8,                         # the docked render size
                     0xc617e8, 0x15a01a0, 0x1d78198):   # GAME_ADDR sites of game_version.h's 1.9.0 block
            self.assertIn(site, found, hex(site))
        for word in (0x1266114, 0x1266118, 0x126611c):   # the phone's handheld render size, three instructions
            self.assertTrue(patch_sites.overlaps([(word, 4)], patch_sites.sites('botw190')), hex(word))
        self.assertNotIn(0x1c0120c, found)              # 1.6.0's VFR site: that branch is not compiled for 1.9.0
        self.assertNotIn(0x126e490, found)              # 1.6.0's pad loop
        self.assertNotIn(0x1821674, found)              # 1.6.0's render size
        self.assertNotIn(0x151d484 + 1, found)
        self.assertNotIn(0x12c1010, found)              # TOTK_JQ_CHECK's table: 1.6.0 addresses only
        self.assertNotIn(0x13a0508, found)

    def test_botw190_has_no_totk_sites(self):
        found = offsets('botw190')
        self.assertNotIn(0x8f6c60, found)               # TOTK 1.0.0 60 FPS
        self.assertNotIn(0xce3fec, found)               # the high-FPS bounce
        self.assertEqual(sorted(patch_sites._unresolved), [])

    def test_botw160_sites_follow_game_patches_botw_c(self):
        # Breath of the Wild 1.6.0: the branch GAME_BOTW160 compiles, its multi-word sequences whole, nothing of 1.9.0's.
        reserved = patch_sites.sites('botw160')
        found = {site.offset for site in reserved}
        for site in (0x1c0120c, 0x1c01218,              # the VFR ratio (real-time frame step), first and last word
                     0x1821564, 0x18522ec, 0x1618480,   # the fade, the UI step, the physics substeps
                     0x1214474,                         # the pause menu's countdown
                     0x1821674,                         # the docked render size
                     0xe53f8c, 0x2128690, 0x2c7ef90,    # GAME_ADDR sites of game_version.h's 1.6.0 block
                     0x182167c, 0x18216ac, 0x1804c44, 0xe53fa0,   # the output size's display-mode and heap checks
                     0x18216c8):                        # the phone's handheld render size (three instructions)
            self.assertIn(site, found, hex(site))
        sizes = {site.offset: site.size for site in reserved}
        self.assertEqual(sizes[0x15c2118], 4 * 14)       # the ultrawide aspect writer, 14 instructions rewritten in place
        self.assertTrue(patch_sites.overlaps([(0x15c2118 + 4 * 13, 4)], reserved))   # its last word, the aspect store
        self.assertFalse(patch_sites.overlaps([(0x15c2118 + 4 * 14, 4)], reserved))
        for word in (0x18216cc, 0x18216d0):
            self.assertTrue(patch_sites.overlaps([(word, 4)], reserved), hex(word))
        self.assertEqual(sizes[0x126e490], 4 * 38)      # the pad loop, 38 instructions rewritten in place
        self.assertEqual(sizes[0x1214474], 4 * 9)       # the menu countdown, 9 instructions
        for site in (0xe563dc, 0x1805028):              # the output size's initialisers: movz, and the movk 12 bytes on
            self.assertEqual(sizes[site], 16, hex(site))
        self.assertTrue(patch_sites.overlaps([(0x126e490 + 4 * 37, 4)], reserved))   # its last word
        self.assertFalse(patch_sites.overlaps([(0x126e490 + 4 * 38, 4)], reserved))
        for site in (0x151d484, 0x12660c8, 0xeb0f78, 0xc617e8):   # 1.9.0's VFR, render size, pad count, GAME_ADDR
            self.assertNotIn(site, found, hex(site))
        self.assertNotIn(0x8f6c60, found)               # TOTK 1.0.0 60 FPS

    def test_botw_sites_skip_the_runtime_copy_of_the_list(self):
        # g_pchtxt_reserved is TOTK_PCHTXT's copy of this list: its rows are not sites, or the parity test checks itself.
        text = 'static const struct { uint32_t off, size; } g_pchtxt_reserved[] = {\n    {0x40, 0x4},\n};\nx = {0x80u, 1};\n'
        self.assertEqual([site.offset for site in patch_sites.botw_sites(text, 't.c', set())], [0x80])

    def test_the_botw_branch_is_chosen_by_the_macros(self):
        text = '#ifdef A\none\n#else\ntwo\n#endif\n#ifndef A\nthree\n#endif\n#if defined(B)\nfour\n#elif C\nfive\n#endif\n#if X > 1\nsix\n#endif\n'
        kept = lambda defined: {line for line in patch_sites.compiled_text(text, defined).split() if line.isalpha()}
        self.assertEqual(kept({'A'}), {'one', 'five', 'six'})      # an #elif is not evaluated: its sites are reserved
        self.assertEqual(kept({'B'}), {'two', 'three', 'four', 'five', 'six'})
        self.assertEqual(kept(set()), {'two', 'three', 'five', 'six'})
        self.assertEqual(patch_sites.compiled_text(text, {'A'}).count('\n'), text.count('\n'))      # line numbers stay

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
