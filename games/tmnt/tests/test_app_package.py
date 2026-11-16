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
                            '"$TOTK_GAME_SLOT_DIR" "$TMNT_STRICT_FRAME_DEPENDENCIES" "$TOTK_ROMFS_PATCHES" "$1" "$2"\n'
                            'printf proof > "$TOTK_ROMFS_PROOF_DIR/.romfs-verified-v1"\n')
            stub.chmod(0o755)
            write_app_entry(app, 'a' * 64)
            write_app_entry(app, 'b' * 64)  # runtime updates rewrite an existing entry
            entry = app / 'Contents/MacOS/PlayTMNT'
            info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
            self.assertEqual(info['CFBundleIdentifier'], APP_ID)
            self.assertEqual(info['CFBundleExecutable'], entry.name)
            subprocess.run(['/bin/sh', '-n', str(entry)], check=True)
            result = subprocess.run([str(entry)], cwd=root, env={**os.environ, 'HOME': str(root)},
                                    text=True, capture_output=True, check=True)
            self.assertEqual(result.stdout.splitlines(), [
                str(game), str(root / 'Library/Application Support/MacNativeGames/TMNT'),
                str(root / 'Library/Application Support/MacNativeGames/game-slot'),
                '1', str(game / 'romfs-patches'), str(game / 'rewritten'), 'boot'])
            self.assertEqual((root/'Library/Application Support/MacNativeGames/TMNT/.romfs-verified-v1').read_text(),'proof')
            self.assertFalse(list(app.rglob('.romfs-verified-v1')))


    def test_runtime_update_preserves_migrated_save_and_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            base = home / 'Library/Application Support/MacNativeGames'
            old = base / 'TMNT-aaaaaaaaaaaa'
            (old / 'save/user').mkdir(parents=True)
            (old / 'account.uid').write_bytes(bytes(range(16)))
            (old / 'save/user/progress').write_bytes(b'player progress')
            helper = Path(__file__).resolve().parents[1] / 'scripts/profile_launcher.sh'
            env = dict(os.environ, HOME=str(home))
            subprocess.run(['/bin/sh', '-eu', str(helper)], env=env, check=True)
            stable = base / 'TMNT'
            self.assertEqual((stable / 'save/user/progress').read_bytes(), b'player progress')
            self.assertEqual((stable / 'account.uid').read_bytes(), bytes(range(16)))
            (stable / 'save/user/progress').write_bytes(b'new progress')
            for value in ('a', 'b'):
                app = home / (value + '.app')
                write_app_entry(app, value * 64)
                entry = (app / 'Contents/MacOS/PlayTMNT').read_text()
                self.assertIn('PROFILE="$BASE/TMNT"', entry)
                self.assertNotIn('TMNT-' + value * 12, entry)
                subprocess.run(['/bin/sh', '-eu', str(helper)], env=env, check=True)
            self.assertEqual((stable / 'save/user/progress').read_bytes(), b'new progress')
            self.assertEqual((old / 'save/user/progress').read_bytes(), b'player progress')

if __name__ == '__main__':
    unittest.main()
