"""Compile and run the launcher's real settings model on macOS; no simulator or UI required."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import settings


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs the macOS Swift toolchain')
class LauncherStorageTests(unittest.TestCase):
    def test_only_changed_choices_are_stored(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'patched').mkdir()
            (tmp / 'patched/main.fixups.tsv').write_text('# RENDER_SCALE\t1\n')   # the render-scale row, a range
            with patch.dict(os.environ, {'GFXAPI': 'nvn'}), patch.object(settings.nacp, 'languages', return_value=['en-US', 'ja']):
                (tmp / 'launcher.json').write_text(json.dumps(settings.build('01007EF00011E000', tmp / 'patched', tmp / 'control.nacp')))
                with patch.dict(os.environ, {'SETTINGS_PLATFORM': 'mac'}):   # the Mac window's list: Display mode, the render scale's base size
                    (tmp / 'launcher_mac.json').write_text(json.dumps(settings.build('01007EF00011E000', tmp / 'patched', tmp / 'control.nacp')))
            exe = tmp / 'launcher_storage_check'
            compile_result = subprocess.run(['xcrun', '-sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library', '-module-cache-path', str(tmp / 'module-cache'),
                            '-o', str(exe), str(REPO / 'dm/ios/Sources/LauncherSettings.swift'), str(REPO / 'dm/ios/Sources/LauncherRow.swift'), str(REPO / 'dm/mac/Launcher.swift'),
                            str(Path(__file__).with_name('launcher_layout_check.swift')), str(Path(__file__).with_name('launcher_storage_check.swift'))], capture_output=True, text=True, timeout=300)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            r = subprocess.run([str(exe), str(tmp / 'launcher.json'), str(tmp / 'launcher_mac.json')], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('launcher storage: 0 failed', r.stdout)


if __name__ == '__main__':
    unittest.main()
