"""Public local builds must make relocatable ZIPs without developer metadata."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

from tools.builder_app import make_app


@unittest.skipUnless(sys.platform == 'darwin', 'requires macOS ditto and xattrs')
class Archive(unittest.TestCase):
    def test_zip_preserves_links_and_resources_without_appledouble(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / 'Launcher.app'
            binary = app / 'Contents/Resources/python/bin/python3.14'
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b'python fixture')
            link = binary.with_name('python3')
            link.symlink_to(binary.name)
            ticket = app / 'Contents/CodeResources'
            ticket.write_bytes(b'ordinary bundle resource')
            for path in (binary, link, ticket):
                subprocess.run(['xattr', '-s', '-w', 'com.macnativegames.fixture', 'metadata', path], check=True)
            archive = root / 'Launcher.zip'
            make_app.zip_app(app, archive)
            with zipfile.ZipFile(archive) as contents:
                self.assertFalse(any(Path(p).name.startswith('._') for p in contents.namelist()))
                self.assertEqual(contents.read('Launcher.app/Contents/CodeResources'), ticket.read_bytes())
                info = contents.getinfo('Launcher.app/Contents/Resources/python/bin/python3')
                self.assertEqual((info.external_attr >> 16) & 0o170000, 0o120000)
                self.assertEqual(contents.read(info), b'python3.14')
            self.assertEqual(archive.with_name('Launcher.zip.sha256').read_text(),
                             make_app.sha256(archive) + '  Launcher.zip\n')
            subprocess.run(['ditto', '-x', '-k', archive, root / 'unpacked'], check=True)
            self.assertEqual((root / 'unpacked/Launcher.app/Contents/Resources/python/bin/python3').readlink(),
                             Path('python3.14'))


if __name__ == '__main__':
    unittest.main()
