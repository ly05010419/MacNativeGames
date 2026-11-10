"""Run the production iOS Photos save path with enforced read/add permission boundaries."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation probe')
class PhotosPermissions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = (ROOT/'games/totk/runtime/frontend_ios.m').read_text()
        functions = [re.search(r'static int screenshot_photos_allowed\(void\).*?\n}', source, re.S).group(),
                     re.search(r'__attribute__\(\(unused\)\) static int screenshot_save\(.*?\n}', source, re.S).group()]
        (folder/'photos-production.inc').write_text('\n'.join(functions))
        cls.exe = folder/'photos'
        built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-I'+str(folder),
                        str(ROOT/'tests/ios_screenshot_photos_probe.m'), '-framework', 'Foundation', '-o', str(cls.exe)],
                       capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def test_bundle_declares_read_and_add_purposes(self):
        spec = (ROOT/'games/totk/ios/project.yml').read_text()
        for key in ('NSPhotoLibraryUsageDescription', 'NSPhotoLibraryAddUsageDescription'):
            self.assertRegex(spec, rf'(?m)^\s+{key}:\s*\S.+$')

    def test_permission_and_album_paths(self):
        for mode in ('full', 'limited', 'add-only', 'denied', 'request', 'album-failure'):
            with self.subTest(mode=mode):
                run = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
                self.assertIn('PASS:', run.stdout)
