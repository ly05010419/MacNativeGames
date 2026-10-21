"""build_app.data_bundle / place_data_bundle: the RomFS image signed once in its own bundle, reused by every packaging.
A few bytes stand in for the 16 GB image; codesign is the real one."""
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import build_app


@unittest.skipUnless(subprocess.run(['xcrun', '--find', 'clang'], capture_output=True).returncode == 0, 'needs Xcode tools')
class DataBundle(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.tree, self.logs = self.root / 'tree', self.root / 'logs'
        self.image = self.tree / 'extracted/v143/romfs/romfs.bin'
        self.image.parent.mkdir(parents=True)
        self.image.write_bytes(b'romfs image')

    def make(self):
        signs = []
        real = build_app.run
        def run(cmd, log, **kw):
            signs.append(cmd); return real(cmd, log, **kw)
        with patch.object(build_app, 'run', side_effect=run), patch.object(build_app, 'say'):
            bundle = build_app.data_bundle(self.tree, 'v143', self.logs)
        return bundle, len(signs)

    def test_signed_once_and_reused_until_the_image_changes(self):
        bundle, signed = self.make()
        self.assertEqual(signed, 1)
        self.assertEqual((bundle / 'Contents/Resources/romfs.bin').read_bytes(), b'romfs image')
        info = plistlib.loads((bundle / 'Contents/Info.plist').read_bytes())
        self.assertEqual((info['CFBundlePackageType'], info['CFBundleShortVersionString']), ('BNDL', '1.4.3'))
        self.assertEqual(subprocess.run(['codesign', '--verify', '--strict', bundle]).returncode, 0)
        self.assertEqual(self.make()[1], 0)   # the same image: reused
        self.image.unlink(); self.image.write_bytes(b'another image')   # extracted again
        bundle, signed = self.make()
        self.assertEqual(signed, 1)
        self.assertEqual((bundle / 'Contents/Resources/romfs.bin').read_bytes(), b'another image')
        self.assertFalse(bundle.with_name(bundle.name + '.partial').exists())

    def test_app_opens_the_image_by_its_usual_path_and_its_seal_verifies(self):
        bundle, _ = self.make()
        app = self.root / 'Game.app'
        (app / 'Contents/MacOS').mkdir(parents=True)
        subprocess.run(['xcrun', 'clang', '-x', 'c', '-', '-o', app / 'Contents/MacOS/Game'], input=b'int main(void){return 0;}',
                       check=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(CFBundleExecutable='Game', CFBundleIdentifier='test.data.app',
                                                                      CFBundlePackageType='APPL')))
        res = app / 'Contents/Resources/totk'
        build_app.place_data_bundle(bundle, app, res, 'v143')
        link = res / 'extracted/v143/romfs/romfs.bin'
        self.assertTrue(link.is_symlink())
        self.assertFalse(os.path.isabs(os.readlink(link)))   # inside the app wherever it goes
        self.assertEqual(link.read_bytes(), b'romfs image')
        build_app.sign_app(app, self.logs)   # codesign --verify --deep --strict passes


if __name__ == '__main__':
    unittest.main()
