import os
import plistlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from package_private import APP_ID, write_app_entry


class AppEntryTests(unittest.TestCase):
    def test_app_entry_from_another_working_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = root / 'TMNT test.app'
            game = app / 'Contents/Resources/game'
            (game / 'build').mkdir(parents=True)
            (game / 'rewritten').mkdir()
            stub = game / 'build/tmnt-native'
            stub.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$TOTK_PROFILE_DIR" '
                            '"$TOTK_GAME_SLOT_DIR" "$TMNT_STRICT_FRAME_DEPENDENCIES" "$1" "$2"\n')
            stub.chmod(0o755)
            write_app_entry(app, 'a' * 64)
            entry = app / 'Contents/MacOS/PlayTMNT'
            info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
            self.assertEqual(info['CFBundleIdentifier'], APP_ID)
            self.assertEqual(info['CFBundleExecutable'], entry.name)
            subprocess.run(['/bin/sh', '-n', str(entry)], check=True)
            result = subprocess.run([str(entry)], cwd=root, env={**os.environ, 'HOME': str(root)},
                                    text=True, capture_output=True, check=True)
            self.assertEqual(result.stdout.splitlines(), [
                str(game), str(root / 'Library/Application Support/MacNativeGames/TMNT-aaaaaaaaaaaa'),
                str(root / 'Library/Application Support/MacNativeGames/game-slot'),
                '1', str(game / 'rewritten'), 'boot'])


if __name__ == '__main__':
    unittest.main()
