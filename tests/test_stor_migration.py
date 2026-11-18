"""Run the production Metal heap-growth probe and its disabled-fix controls.

Set MNG_OOT_PROBE, MNG_OOT_GAME_DATA and MNG_OOT_REWRITTEN to an externally
built oot-native and the player's verified data. No game data lives in tests.
"""
import json
import os
from pathlib import Path
import subprocess
import unittest


NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class MigrationDefault(unittest.TestCase):
    """2026-11-15: growth migration is on by default wherever the alias heaps are (Ocarina of Time): without it a few
    live render targets kept every earlier heap generation (1,197 vs 758 MiB of heaps on one route)."""
    def test_on_unless_turned_off(self):
        text = (NVN / 'nvn_stor_alias.h').read_text()
        self.assertIn('const char *e = getenv("OOT_STOR_MIGRATE"); on = e ? atoi(e) != 0 : 1;', text)
        self.assertIn('if (mt && stor_alias_migrate_on() && g_alias_heaps_made != before)', text,
                      'a new heap triggers the migration')

    def test_entries_without_a_holder_move_too(self):
        """2026-11-17: entering the world, entries no serial held stayed on earlier generations (681 of 687 textures
        there) and kept 1.5 GB of earlier heaps; they move with the held ones now (OOT peak 11.9 -> 11.2 GB). Detached
        entries without a holder are on their way out and stay."""
        text = (NVN / 'nvn_stor_migrate.h').read_text()
        self.assertIn('if (!entry || !entry->tex || (detached && !entry->nser)) continue;', text)
        self.assertNotIn('if (!entry || !entry->tex || !entry->nser) continue;', text)


@unittest.skipUnless(all(os.environ.get(k) for k in
                         ('MNG_OOT_PROBE', 'MNG_OOT_GAME_DATA', 'MNG_OOT_REWRITTEN')),
                     'requires the built OOT Metal runtime and verified local data')
class HeapMigrationTests(unittest.TestCase):
    def run_probe(self, *, enabled, exact=False, probe='probe-gpu-stor-alias'):
        env = dict(os.environ, OOT_STOR_MIGRATE=str(int(enabled)), OOT_STOR_ALIAS='2' if exact else '1',
                   OOT_STOR_MIGRATE_TEST='1', TOTK_GAME_DATA=os.environ['MNG_OOT_GAME_DATA'],
                   TOTK_SLOT_OWNER='codex', TOTK_SLOT_PURPOSE='heap-migration-regression', TOTK_SLOT_EXPECT_S='30')
        env.pop('OOT_STOR_MIGRATE_SUBRESOURCES_ONLY', None)
        if exact:
            env['OOT_STOR_MIGRATE_SUBRESOURCES_ONLY'] = '1'
        result = subprocess.run([os.environ['MNG_OOT_PROBE'], os.environ['MNG_OOT_REWRITTEN'], probe],
                                env=env, capture_output=True, text=True, timeout=90)
        return result, result.stdout + result.stderr

    def assert_pass(self, result, log, checked, probe='probe-gpu-stor-alias'):
        self.assertEqual(result.returncode, 0, log[-10000:])
        records = [json.loads(line) for line in log.splitlines() if line.startswith('{"status":')]
        self.assertTrue(records, log)
        self.assertEqual(records[-1]['status'], 'pass')
        self.assertEqual(records[-1]['probe'], probe)
        self.assertEqual(records[-1]['checked'], checked)

    def test_regions_preserve_pixels_and_release_old_heaps(self):
        result, log = self.run_probe(enabled=True)
        self.assert_pass(result, log, 77)
        self.assertIn('heap growth migration probe passed:', log)
        self.assertIn('heap migration subresources passed: 45', log)

    def test_all_depth_color_mips_and_layers_migrate(self):
        result, log = self.run_probe(enabled=True, exact=True)
        self.assert_pass(result, log, 45)
        for mode in range(10):
            self.assertIn(f'migration subresource mode {mode}: moved', log)

    def test_disabled_fix_retains_old_heap(self):
        result, log = self.run_probe(enabled=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('FATAL migration probe: live owners still retain old heaps', log)

    def test_disabled_fix_fails_subresource_migration(self):
        result, log = self.run_probe(enabled=False, exact=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('FATAL migration subresources not moved mode 0', log)


if __name__ == '__main__':
    unittest.main()
