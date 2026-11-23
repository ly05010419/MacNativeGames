"""A game file's game, kind and version come from its contents, never from its name (user 2026-11-21: "经常装rom 缺识别
成其他游戏，比如宝可梦的rom 识别出 塞尔达 … 去nxnative 看看 如何正确识别"): with the keys, from its Meta NCAs' content metas
(tools/builder_jobs/identify.py, as nxnative's scan_nsps); files of more than one game stop the plan, a game this
repository does not build included, each file named with what it is. Before this a card's game came from its name, a
card named without one counted for no game, and a Pokémon card dropped beside Zelda files kept from before was built as
Zelda. The game files are made by tests/switch_packages.py with made-up keys."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))

from tools.builder_jobs import classify, identify  # noqa: E402
import switch_packages as sp  # noqa: E402

TOTK, TOTK_UPDATE = '0100F2C0115B6000', '0100F2C0115B6800'
BOTW, BOTW_UPDATE, BOTW_DLC = '01007EF00011E000', '01007EF00011E800', '01007EF00011F001'
ACNH, ACNH_UPDATE = '01006F8002326000', '01006F8002326800'
SCARLET = '0100A3D008C5C000'          # Pokémon Scarlet: a game this repository does not build
TRANSFER_TOOL = '0100F38011CFE000'    # Animal Crossing's Island Transfer Tool, an application of its own
POKOPIA = '0400A3A01EE4C000'          # a Switch 2 game, built from its dump folder only


class Identify(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.keys = self.dir / 'prod.keys'
        self.keys.write_text(sp.keys_text())
        self.env = mock.patch.dict(os.environ, {'MNG_LANG': 'en'})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def items(self, *paths, keys=True):
        got = classify.classify([*paths, *([self.keys] if keys else [])])
        return {Path(i.path).name: i for i in got if i.role != 'keys'}

    def plan(self, *paths, keys=True):
        return classify.plan(classify.classify([*paths, *([self.keys] if keys else [])]))

    def test_the_content_metas_of_an_nsp_and_a_card(self):
        sp.nsp(self.dir / 'u.nsp', [(TOTK_UPDATE, 'update', 720896)])
        sp.xci(self.dir / 'c.xci', [(BOTW, 'base', 0), (BOTW_UPDATE, 'update', 786432), (BOTW_DLC, 'dlc', 196608)])
        self.assertEqual(identify.contents(self.dir / 'u.nsp', self.keys),
                         ([identify.Meta('update', TOTK_UPDATE, 720896)], None))
        metas, problem = identify.contents(self.dir / 'c.xci', self.keys)
        self.assertIsNone(problem)
        self.assertEqual(sorted(metas, key=lambda m: m.title), [identify.Meta('base', BOTW, 0), identify.Meta('update', BOTW_UPDATE, 786432),
                                                                identify.Meta('dlc', BOTW_DLC, 196608)])

    def test_a_name_that_says_another_game_is_not_read(self):
        sp.nsp(self.dir / 'Breath of the Wild [01007EF00011E000][v0].nsp', [(TOTK, 'base', 0)])
        sp.xci(self.dir / 'Tears of the Kingdom [0100F2C0115B6000].xci', [(SCARLET, 'base', 0)])
        got = self.items(self.dir / 'Breath of the Wild [01007EF00011E000][v0].nsp')
        self.assertEqual((got['Breath of the Wild [01007EF00011E000][v0].nsp'].role, got['Breath of the Wild [01007EF00011E000][v0].nsp'].game), ('base', 'totk'))
        card = self.items(self.dir / 'Tears of the Kingdom [0100F2C0115B6000].xci')['Tears of the Kingdom [0100F2C0115B6000].xci']
        self.assertEqual((card.role, card.game, card.title), ('ignored', None, SCARLET))

    def test_a_card_without_an_id_in_its_name_is_told_by_its_contents(self):
        sp.xci(self.dir / 'The Legend of Zelda.xci', [(BOTW, 'base', 0)])
        p = self.plan(self.dir / 'The Legend of Zelda.xci')
        self.assertEqual(p.game, 'botw')

    def test_a_pokemon_card_beside_zelda_files_stops_the_plan(self):
        """The player's report: Zelda files kept from before (the Launcher keeps the chosen files) and a Pokémon card
        dropped onto them. Before, the plan was Tears of the Kingdom, ready to build."""
        sp.xci(self.dir / 'TOTK.xci', [(TOTK, 'base', 0)])
        sp.xci(self.dir / 'Pokemon Scarlet.xci', [(SCARLET, 'base', 0)])
        p = self.plan(self.dir / 'TOTK.xci', self.dir / 'Pokemon Scarlet.xci')
        self.assertIsNone(p.game)
        self.assertIn('several games', p.blocks[0])
        self.assertIn('TOTK.xci: Tears of the Kingdom', p.blocks[0])
        self.assertIn(f'Pokemon Scarlet.xci: a game this repository does not build ({SCARLET})', p.blocks[0])

    def test_two_games_this_repository_builds_stop_the_plan_too(self):
        sp.nsp(self.dir / 'u.nsp', [(TOTK_UPDATE, 'update', 720896)])
        sp.xci(self.dir / 'botw.xci', [(BOTW, 'base', 0)])
        p = self.plan(self.dir / 'u.nsp', self.dir / 'botw.xci')
        self.assertIsNone(p.game)
        self.assertIn('u.nsp: Tears of the Kingdom', p.blocks[0])
        self.assertIn('botw.xci: Breath of the Wild', p.blocks[0])

    def test_a_game_this_repository_does_not_build_is_said_as_what_it_is(self):
        sp.xci(self.dir / 'game.xci', [(SCARLET, 'base', 0)])
        p = self.plan(self.dir / 'game.xci')
        self.assertIsNone(p.game)
        self.assertEqual(len(p.blocks), 1)
        self.assertIn(f'{SCARLET} (base game) is not a game this repository builds', p.blocks[0])

    def test_another_application_inside_a_dropped_folder_is_left_out(self):
        folder = self.dir / 'Animal Crossing'
        (folder / 'Island Transfer Tool').mkdir(parents=True)
        sp.nsp(folder / 'base.nsp', [(ACNH, 'base', 0)])
        sp.nsp(folder / 'update.nsp', [(ACNH_UPDATE, 'update', 2228224)])
        sp.nsp(folder / 'Island Transfer Tool' / 'tool.nsp', [(TRANSFER_TOOL, 'base', 0)])
        p = self.plan(folder)
        self.assertEqual((p.game, p.update_version), ('acnh', 2228224))
        self.assertFalse(any('several games' in b for b in p.blocks))

    def test_the_version_is_the_contents_not_the_name(self):
        sp.nsp(self.dir / 'TOTK.nsp', [(TOTK, 'base', 0)])
        sp.nsp(self.dir / 'old [v720896].nsp', [(TOTK_UPDATE, 'update', 393216)])   # its name says 1.4.3, it is 1.2.1
        sp.nsp(self.dir / '1.4.3.nsp', [(TOTK_UPDATE, 'update', 720896)])
        got = self.items(self.dir / 'TOTK.nsp', self.dir / 'old [v720896].nsp', self.dir / '1.4.3.nsp')
        self.assertEqual(got['old [v720896].nsp'].version, 393216)
        self.assertIn('its name says v720896, its contents v393216', got['old [v720896].nsp'].note)
        p = self.plan(self.dir / 'TOTK.nsp', self.dir / 'old [v720896].nsp', self.dir / '1.4.3.nsp')
        self.assertEqual((p.update, p.update_version), (str((self.dir / '1.4.3.nsp').resolve()), 720896))

    def test_a_combined_card_names_the_version_of_its_update(self):
        sp.xci(self.dir / 'BotW v1.6.0.xci', [(BOTW, 'base', 0), (BOTW_UPDATE, 'update', 786432)])
        card = self.items(self.dir / 'BotW v1.6.0.xci')['BotW v1.6.0.xci']
        self.assertEqual((card.role, card.game, card.holds_update), ('base', 'botw', 786432))
        self.assertNotIn('its name says', card.note)

    def test_an_animal_crossing_card_with_its_update_takes_that_update_s_version(self):
        sp.xci(self.dir / 'acnh.xci', [(ACNH, 'base', 0), (ACNH_UPDATE, 'update', 2228224)])
        with mock.patch.object(classify, 'card_titles', return_value=2):
            p = self.plan(self.dir / 'acnh.xci')
        self.assertEqual((p.game, p.update_version), ('acnh', 2228224))

    def test_keys_too_old_still_tell_the_game(self):
        sp.xci(self.dir / 'new.xci', [(TOTK, 'base', 0, 0x10)])   # generation 0x10: newer than these keys (0)
        item = self.items(self.dir / 'new.xci')['new.xci']
        self.assertEqual((item.game, item.version), ('totk', None))
        p = self.plan(self.dir / 'new.xci')
        self.assertEqual(p.game, 'totk')
        self.assertTrue(any('titlekek_10' in b for b in p.keys_problems))

    def test_without_keys_a_ticket_tells_the_game_and_a_card_waits(self):
        sp.nsp(self.dir / 'g.nsp', [(TOTK, 'base', 0)], tickets=[TOTK])
        sp.xci(self.dir / 'c [0100F2C0115B6000].xci', [(TOTK, 'base', 0)])
        got = self.items(self.dir / 'g.nsp', self.dir / 'c [0100F2C0115B6000].xci', keys=False)
        self.assertEqual((got['g.nsp'].game, got['g.nsp'].version), ('totk', None))
        card = got['c [0100F2C0115B6000].xci']
        self.assertEqual((card.role, card.game), ('base', None))   # its name's ID is not read
        self.assertIn('read with your keys', card.note)
        self.assertIn('no keys', self.plan(self.dir / 'c [0100F2C0115B6000].xci', keys=False).blocks[0])

    def test_keys_that_cannot_open_the_file_are_said(self):
        sp.xci(self.dir / 'c.xci', [(TOTK, 'base', 0)])
        self.keys.write_text('header_key = ' + '11' * 32 + '\n')
        p = self.plan(self.dir / 'c.xci')
        self.assertIsNone(p.game)
        self.assertTrue(any('cannot read c.xci' in b for b in p.keys_problems))

    def test_a_switch_2_package_says_to_use_its_dump_folder(self):
        sp.xci(self.dir / 'pokopia.xci', [(POKOPIA, 'base', 0)])
        item = self.items(self.dir / 'pokopia.xci')['pokopia.xci']
        self.assertEqual(item.role, 'ignored')
        self.assertIn('Switch 2 dump folder', item.note)

    def test_a_package_of_no_game_content(self):
        sp.nsp(self.dir / 'delta.nsp', [(TOTK_UPDATE, 'delta', 720896)])
        self.assertEqual(self.items(self.dir / 'delta.nsp')['delta.nsp'].role, 'ignored')


if __name__ == '__main__':
    unittest.main()
