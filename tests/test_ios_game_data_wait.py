"""The iPhone / iPad game waits for its game data and shaders while install_device.py copies them, shows how far the copy
is and starts by itself when they are there (user 2026-11-19: "必须 能自己刷新 而且 传输的时候 有进度"; the boot screen
said "still being copied" and stayed so, the game had to be closed and opened again).

Checked on games/totk's iOS entry (Tears of the Kingdom, Breath of the Wild, Bananza, Pokopia) and on the markers
install_device.py writes for it.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / 'games/totk/ios/Sources/main.m'
INSTALL = ROOT / 'games/totk/ios/tools/install_device.py'


class WaitsForTheCopy(unittest.TestCase):
    def setUp(self):
        self.main = MAIN.read_text()

    def body(self, signature):
        start = self.main.index(signature)
        return self.main[start:self.main.index('\n}\n', start)]

    def test_one_check_at_launch_and_again_every_second(self):
        launch = self.body('- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:')
        self.assertIn('g_no_game_data = game_data_ready(docs, res);', launch)
        self.assertNotIn('romfs.sha256', launch)   # the check lives in game_data_ready only
        appear = self.body('- (void)viewDidAppear:(BOOL)animated {')
        self.assertIn('if (g_wait_for_data) [self waitForGameData];', appear)
        self.assertIn('[self startAfterChecks];', appear)
        wait = self.body('- (void)waitForGameData {')
        self.assertIn('scheduledTimerWithTimeInterval:1.0 repeats:YES', wait)
        self.assertIn('game_data_ready(docs, res)', wait)
        self.assertIn('runtime_boot_progress(', wait)   # the bar and "x / y GB"
        self.assertIn('[weakSelf startAfterChecks];', wait)
        self.assertIn('[timer invalidate];', wait)

    def test_ready_sets_every_path_that_names_the_shaders(self):
        ready = self.body('static NSString *game_data_ready(NSString *docs, NSString *res) {')
        for env in ('TOTK_ROMFS_DATA', 'TOTK_SHADER_SEED_DIR', 'DM_NVN_SHMAP', 'DM_MSL_DIR', 'TOTK_SHADER_SEED_PUSHED'):
            self.assertIn(f'"{env}"', ready)
        self.assertIn('g_wait_for_data = YES;', ready)
        # the copy's size from the markers install_device.py writes while copying
        self.assertIn('words.count >= 3 ? strtoull(words[2].UTF8String', ready)
        self.assertIn('[words.firstObject isEqualToString:@"stale"]', ready)

    def test_the_markers_carry_the_size(self):
        install = INSTALL.read_text()
        self.assertIn("copying = f'copying {sha} {size}'", install)
        self.assertIn("got.write_text(f'stale {pack + image}\\n')", install)
        # the hash alone is still written last, and the seed's folder and fingerprint last for the shaders
        self.assertIn("(Path(d)/'romfs.sha256').write_text(sha + '\\n')", install)
        self.assertIn("got.write_text(f'{folder}\\n{fp}\\n')", install)

    def test_no_screen_asks_to_open_the_game_again(self):
        self.assertNotIn('Open the game again when it is done', self.main)
        self.assertNotIn('复制完再打开游戏', self.main)


if __name__ == '__main__':
    unittest.main()
