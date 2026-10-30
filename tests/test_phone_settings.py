"""The phone switches' Settings schema and native defaults/environment precedence."""
import plistlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PhoneSettings(unittest.TestCase):
    def test_settings_exposes_two_default_on_boolean_switches(self):
        settings = plistlib.loads((ROOT / 'games/totk/ios/Settings.bundle/Root.plist').read_bytes())
        for key in ('TOTK_PHONE_GYRO', 'TOTK_PHONE_HAPTICS'):
            matches = [s for s in settings['PreferenceSpecifiers'] if s.get('Key') == key]
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]['Type'], 'PSToggleSwitchSpecifier')
            self.assertIs(matches[0]['DefaultValue'], True)

    @unittest.skipUnless(sys.platform == 'darwin', 'needs Apple Foundation')
    def test_native_preference_precedence(self):
        with tempfile.TemporaryDirectory() as d:
            probe = Path(d) / 'phone-settings-probe'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/test_phone_settings.m'), '-framework', 'Foundation',
                            '-o', str(probe)], check=True, capture_output=True, text=True)
            subprocess.run([str(probe)], check=True, capture_output=True, text=True)


if __name__ == '__main__':
    unittest.main()
