"""A .nup's game comes from its package table, never from the extension (issue #2333: a Mario Kart World 1.8.0 update was
shown as Ocarina of Time and 下一步 stayed enabled). The packages here are made up: a table and no game data."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))

import build_app  # noqa: E402
from nup_packages import KART_WORLD, OOT, POKOPIA, fmt, make_nup  # noqa: E402
from tools.builder_jobs import classify  # noqa: E402


class NupGame(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name).resolve()
        env = mock.patch.dict(os.environ, {'MNG_LANG': 'en'})
        env.start()
        self.addCleanup(env.stop)

    def plan(self, *paths):
        return classify.plan(classify.classify(list(paths)))

    def test_ocarina_of_time_still_builds(self):
        path = make_nup(self.dir / 'any name.nup', (OOT + 0x800, fmt.KIND_UPDATE, 131072))
        plan = self.plan(path)
        self.assertEqual((plan.game, plan.base), ('oot', str(path)))
        self.assertFalse([b for b in plan.blocks if 'nup' in b.lower()])
        with mock.patch.object(build_app, 'build_oot') as oot:
            build_app.main([str(path)])
        self.assertEqual(oot.call_args.args[1], path)

    def test_a_base_and_its_update_in_one_nup_are_one_game(self):
        path = make_nup(self.dir / 'both.nup', (OOT, fmt.KIND_BASE, 0), (OOT + 0x800, fmt.KIND_UPDATE, 131072))
        self.assertEqual(self.plan(path).game, 'oot')

    def test_issue_2333_another_game_named_like_zelda_stops_at_the_plan_and_the_build(self):
        path = make_nup(self.dir / 'Ocarina of Time.nup', (KART_WORLD, fmt.KIND_UPDATE, 917504))
        plan = self.plan(path)
        self.assertIsNone(plan.game)
        self.assertIsNone(plan.base)
        self.assertIn(f'{KART_WORLD:016X}', plan.blocks[0])
        with mock.patch.object(build_app, 'build_oot') as oot:
            with self.assertRaisesRegex(build_app.BuildError, f'{KART_WORLD:016X}'):
                build_app.main([str(path)])
        oot.assert_not_called()

    def test_a_game_built_from_its_folder_says_so(self):
        path = make_nup(self.dir / 'p.nup', (POKOPIA + 0x800, fmt.KIND_UPDATE, 655360))
        plan = self.plan(path)
        self.assertIsNone(plan.base)
        self.assertIn('dump folder', plan.blocks[0])
        with mock.patch.object(build_app, 'build_oot') as oot:
            with self.assertRaises(build_app.BuildError):
                build_app.main([str(path)])
        oot.assert_not_called()

    def test_another_version_of_ocarina_of_time_is_named(self):
        path = make_nup(self.dir / 'old.nup', (OOT + 0x800, fmt.KIND_UPDATE, 65536))
        plan = self.plan(path)
        self.assertIsNone(plan.base)
        self.assertIn('1.0.0', plan.blocks[0])
        self.assertIn('2.0.0', plan.blocks[0])
        with mock.patch.object(build_app, 'build_oot') as oot:
            with self.assertRaisesRegex(build_app.BuildError, '2.0.0'):
                build_app.main([str(path)])
        oot.assert_not_called()

    def test_two_games_in_one_nup_stop(self):
        path = make_nup(self.dir / 'two.nup', (OOT, fmt.KIND_BASE, 0), (KART_WORLD, fmt.KIND_BASE, 0))
        plan = self.plan(path)
        self.assertIsNone(plan.base)
        self.assertTrue(plan.blocks)

    def test_a_damaged_nup_is_said_to_be_damaged(self):
        path = make_nup(self.dir / 'bad.nup', (OOT + 0x800, fmt.KIND_UPDATE, 131072))
        data = bytearray(path.read_bytes())
        data[fmt.HEADER_SIZE] ^= 0xFF   # the table no longer matches its SHA-256
        path.write_bytes(data)
        plan = self.plan(path)
        self.assertIsNone(plan.base)
        self.assertIn('cannot be read', plan.blocks[0])
        with mock.patch.object(build_app, 'build_oot') as oot:
            with self.assertRaises(build_app.BuildError):
                build_app.main([str(path)])
        oot.assert_not_called()

    def test_another_games_nup_in_a_dropped_folder_is_left_out(self):
        folder = self.dir / 'games'
        folder.mkdir()
        make_nup(folder / 'kart.nup', (KART_WORLD, fmt.KIND_UPDATE, 917504))
        oot = make_nup(folder / 'zelda.nup', (OOT + 0x800, fmt.KIND_UPDATE, 131072))
        plan = self.plan(folder)
        self.assertEqual((plan.game, plan.base), ('oot', str(oot)))

    def test_two_dropped_nups_of_two_games_stop(self):
        kart = make_nup(self.dir / 'kart.nup', (KART_WORLD, fmt.KIND_UPDATE, 917504))
        zelda = make_nup(self.dir / 'zelda.nup', (OOT + 0x800, fmt.KIND_UPDATE, 131072))
        plan = self.plan(kart, zelda)
        self.assertTrue(any('several games' in b for b in plan.blocks))


if __name__ == '__main__':
    unittest.main()
