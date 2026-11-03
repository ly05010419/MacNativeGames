"""games/totk/ios/Sources/save_backup.m: the Settings › Save backup switches and the folder layout (2026-11-03)."""
import plistlib
import unittest
from pathlib import Path

IOS = Path(__file__).resolve().parents[1]/'games/totk/ios'


class PaneSwitches(unittest.TestCase):
    """The pane is the only place these switches are turned on. A key the code reads but the pane does not offer can
    never be set (the backup code read getenv("TOTK_BACKUP") while no row existed, so the backup could not be enabled)."""
    def setUp(self):
        specs = plistlib.loads((IOS/'Settings.bundle/Root.plist').read_bytes())['PreferenceSpecifiers']
        self.rows = {s['Key']: s for s in specs if 'Key' in s}
        self.code = (IOS/'Sources/save_backup.m').read_text()

    def test_the_pane_offers_every_switch_the_code_reads(self):
        for key in ('TOTK_BACKUP', 'TOTK_SAVE_IMPORT', 'TOTK_BACKUP_RECONFIGURE'):
            self.assertIn(key, self.rows, key)
            self.assertIs(self.rows[key]['DefaultValue'], False, key)
            self.assertIn(f'@"{key}"', self.code, key)

    def test_the_backup_switch_is_read_from_the_pane_and_a_launch_variable_still_wins(self):
        # Both routes stay: the pane (NSUserDefaults) is the player's, a devicectl launch sets TOTK_BACKUP directly.
        self.assertIn('static NSString *const kBackupEnabled = @"TOTK_BACKUP";', self.code)
        self.assertIn('boolForKey:kBackupEnabled', self.code)
        self.assertIn('getenv("TOTK_BACKUP")', self.code)

    def test_the_pane_describes_a_folder_the_player_picks(self):
        for name in ('Sources/save_backup.h', 'Sources/save_backup.m'):
            text = (IOS/name).read_text()
            self.assertNotIn('iCloud', text, name)

    def test_the_backup_folder_does_not_share_files_with_the_save_sync(self):
        # cloud_save.m (save sync between devices) keeps <picked>/Saves; this backup keeps its own <picked>/<game>
        # folder, so choosing the same folder for both never writes the same file from the two features.
        self.assertIn('URLByAppendingPathComponent:game_folder()', self.code)
        self.assertIn('@"save", @"game-slot"', self.code)


if __name__ == '__main__':
    unittest.main()
