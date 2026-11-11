"""Screenshot sharing (games/totk/runtime/shot_upload.h, user 2026-11-11): the upload, the pending folder, the list, the
pictures and the player's delete, against the shader inbox's own handlers on 127.0.0.1 (test/local-server.js); and the
hook in screenshot.h that the Mac front ends point at it."""
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAMES = ROOT / 'games'
# The inbox is a project of its own (not in this repository): $SHADER_INBOX_DIR, else MacNativeGamesWeb/shader-inbox beside it.
INBOX = Path(os.environ.get('SHADER_INBOX_DIR') or ROOT.parent / 'MacNativeGamesWeb/shader-inbox')


class ShotShareSourceTests(unittest.TestCase):
    """The three Zelda games share the code: games/oot keeps byte-identical copies, and only the Mac front ends share."""

    def test_oot_copies_are_identical(self):
        for name in ('shot_upload.h', 'shot_tab.h', 'screenshot.h', 'board_upload.h'):
            with self.subTest(name=name):
                self.assertEqual((GAMES / 'totk/runtime' / name).read_bytes(), (GAMES / 'oot/runtime' / name).read_bytes())

    def test_mac_front_ends_share_and_the_ios_one_does_not(self):
        for game in ('totk', 'oot'):
            with self.subTest(game=game):
                front = (GAMES / game / 'runtime/frontend.m').read_text(encoding='utf-8')
                self.assertIn('#include "shot_upload.h"', front)
                self.assertIn('totk_screenshot_shared=totk_shot_queue', front)
                self.assertIn('totk_shot_start();', front)
                ios = GAMES / game / 'runtime/frontend_ios.m'
                if ios.exists():
                    self.assertNotIn('shot_upload.h', ios.read_text(encoding='utf-8'))
        upload = (GAMES / 'totk/runtime/shot_upload.h').read_text(encoding='utf-8')
        self.assertIn('#if !TARGET_OS_IPHONE', upload)

    def test_the_hook_runs_after_a_save_and_before_the_release(self):
        text = (GAMES / 'totk/runtime/screenshot.h').read_text(encoding='utf-8')
        self.assertIn('int (*totk_screenshot_shared)(CGImageRef img) __attribute__((weak)) = NULL;', text)
        save = text.index('int ok = TOTK_SCREENSHOT_SAVE(')
        hook = text.index('totk_screenshot_shared(taken)', save)
        release = text.index('if (taken) CGImageRelease(taken);', save)
        self.assertLess(hook, release)


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('node') and (INBOX / 'node_modules/@electric-sql/pglite').is_dir(),
                     'needs the Xcode command line tools, node and the shader inbox (SHADER_INBOX_DIR) with npm install')
class ShotShareUploadTests(unittest.TestCase):
    """tests/shot_share_probe.m: a 2560x1440 frame through totk_shot_queue to the local inbox and back."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.probe = Path(cls.tmp.name) / 'shot_share_probe'
        build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/shot_share_probe.m'),
                                '-framework', 'Foundation', '-framework', 'AppKit', '-framework', 'ImageIO',
                                '-framework', 'CoreGraphics', '-framework', 'IOKit', '-o', str(cls.probe)],
                               capture_output=True, text=True, timeout=120)
        if build.returncode:
            cls.tmp.cleanup()
            raise AssertionError(build.stderr[-2000:])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_probe(self, mode, board, url):
        env = dict(os.environ, TOTK_BOARD_DIR=str(board), TOTK_SHOT_URL=url, TOTK_BOARD_URL=url.replace('/v1/shots', '/v1/board'),
                   TOTK_SHOT_TEST='1')
        run = subprocess.run([str(self.probe), mode], capture_output=True, text=True, timeout=120, env=env)
        self.assertEqual(run.returncode, 0, f'{mode}: {run.stdout}{run.stderr[-3000:]}')
        result = json.loads(run.stdout)
        self.assertEqual((result['status'], result['mode']), ('pass', mode))
        return result, run.stderr

    def server(self):
        server = subprocess.Popen(['node', str(INBOX / 'test/local-server.js')], cwd=INBOX, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (server.stdin.close(), server.wait(timeout=30), server.stdout.close()))
        return f'http://127.0.0.1:{int(server.stdout.readline().split()[1])}/v1/shots'

    def board(self):
        folder = Path(tempfile.mkdtemp(prefix='shots-'))
        self.addCleanup(shutil.rmtree, folder, True)
        game = folder / 'TOTK'   # the game's folder; the board id lives in the one above (board_upload.h)
        game.mkdir()
        return game

    def test_shared_listed_read_and_deleted(self):
        url, board = self.server(), self.board()
        result, log = self.run_probe('up', board, url)
        self.assertEqual(result['game'], 'totk')
        self.assertIn('[shots] dropped 1.json: the server refused the picture', log)
        self.assertIn('[shots] deleted ', log)
        self.assertEqual(list((board / 'shot-pending').iterdir()), [], 'nothing waits once the server took it')
        self.assertEqual((board / 'screenshot-share.txt').read_text(), 'on\n')
        self.assertRegex((board.parent / 'leaderboard-id.txt').read_text().strip(), r'^[0-9a-f-]{36}$')

    def test_without_a_server_the_picture_waits_then_goes_at_start(self):
        board = self.board()
        down, log = self.run_probe('down', board, 'http://127.0.0.1:9/v1/shots')   # the discard port: nothing listens
        self.assertIn('kept for a later try (no answer)', log)
        self.assertLessEqual(down['image_bytes'], 2_500_000)
        self.assertLessEqual(down['thumb_bytes'], 80_000)
        self.assertEqual(down['board_id'], (board.parent / 'leaderboard-id.txt').read_text().strip())
        self.assertEqual([p.name for p in (board / 'shot-pending').iterdir()], [down['pending']])
        self.run_probe('start', board, self.server())
        self.assertEqual(list((board / 'shot-pending').iterdir()), [])

    def test_switching_off_drops_what_waits(self):
        board = self.board()
        self.run_probe('down', board, 'http://127.0.0.1:9/v1/shots')
        self.run_probe('off', board, 'http://127.0.0.1:9/v1/shots')
        self.assertFalse((board / 'shot-pending').exists())
        self.assertEqual((board / 'screenshot-share.txt').read_text(), 'off\n')


if __name__ == '__main__':
    unittest.main()
