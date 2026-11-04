"""Synthetic packaging checks: failures must not remove the installed app."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from games.tomodachi import package


class PackageTransaction(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.work = self.root / 'work'
        self.output = self.root / 'apps/Tomodachi.app'
        self.prepared = self.root / 'prepared'
        for path in (self.work/'build', self.work/'build/spirv-cross', self.prepared/'control', self.output/'Contents/Resources'):
            path.mkdir(parents=True, exist_ok=True)
        (self.output/'original').write_text('preserve this package')
        (self.output/'Contents/Resources/language.txt').write_text('ja\n')
        (self.work/'build/tomodachi-native').write_bytes(b'synthetic host')
        (self.work/'build/spirv-cross/spirv-cross').write_bytes(b'synthetic tool')
        (self.prepared/'control/romfs.bin').write_bytes(b'synthetic control')
        (self.work/'host-info.json').write_text(json.dumps({'title_id':'010051F0207B2000','prepared':str(self.prepared), 'spirv_cross_sha256':hashlib.sha256(b'synthetic tool').hexdigest()}))

    def test_icon_failure_preserves_installed_app(self):
        with patch.object(package,'package_icon',side_effect=ValueError('synthetic icon failure')):
            with self.assertRaisesRegex(ValueError,'synthetic icon failure'):
                package.package(self.work,self.output,replace=True)
        self.assertEqual((self.output/'original').read_text(),'preserve this package')
        self.assertEqual(list(self.output.parent.glob('tomodachi-package-*')),[])

    def test_success_preserves_language_and_final_relative_paths(self):
        with patch.object(package,'package_icon'), patch.object(package.subprocess,'run') as run:
            run.return_value.returncode=1
            package.package(self.work,self.output,replace=True)
        resources=self.output/'Contents/Resources'
        self.assertEqual((resources/'language.txt').read_text(),'ja\n')
        self.assertEqual((resources/(resources/'host-path.txt').read_text().strip()).resolve(),self.work.resolve())
        self.assertEqual((resources/(resources/'romfs-path.txt').read_text().strip()).resolve(),(self.prepared/'romfs').resolve())
        self.assertEqual(len(list((self.work/'tests').glob('app-*/*.backup/original'))),1)
        self.assertEqual(list(self.output.parent.glob('tomodachi-package-*')),[])

if __name__=='__main__':
    unittest.main()
