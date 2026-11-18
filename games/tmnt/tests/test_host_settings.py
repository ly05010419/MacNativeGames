"""Exercise real AppKit settings actions, persistence and invalid preference recovery."""
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

@unittest.skipUnless(sys.platform == 'darwin', 'AppKit requires macOS')
class HostSettings(unittest.TestCase):
    def test_actions_and_persistence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'host-settings.plist').write_bytes(plistlib.dumps({'frameRate': 'invalid', 'syncFolder': 5}))
            exe = root / 'probe'
            subprocess.run(['clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            '-Wno-deprecated-declarations', '-Wno-unused-function',
                            str(Path(__file__).with_name('tmnt_settings_probe.m')),
                            '-framework', 'Cocoa', '-o', str(exe)], check=True, timeout=60)
            env = dict(os.environ, TOTK_PROFILE_DIR=tmp, TOTK_AUDIO_OFF='1')
            env.pop('TOTK_CLOUD_SAVE_FOLDER', None)
            subprocess.run([str(exe)], env=env, check=True, timeout=30)
            saved = plistlib.loads((root / 'host-settings.plist').read_bytes())
            self.assertEqual(saved['frameRate'], 60)
            self.assertEqual(saved['controller'], 3)
            self.assertNotIn('syncFolder', saved)
