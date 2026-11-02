import importlib.util
import hashlib
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/prepare_runtime.py'
spec = importlib.util.spec_from_file_location('prepare_runtime', SCRIPT)
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class AddonsConfigTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.out = self.work / 'build'
        self.out.mkdir()
        self.expected = prepare.EXPECTED
        self.addCleanup(setattr, prepare, 'EXPECTED', self.expected)
        prepare.EXPECTED = {'aoc': {}}
        for index in ('1', '2'):
            package = self.work / f'{index}.nsp'
            package.write_bytes(f'package-{index}'.encode())
            image = f'image-{index}'.encode()
            prepare.EXPECTED['aoc'][index] = {
                'title_id': f'010054B01AD9300{index}',
                'package_sha256': prepare.sha(package),
                'size': len(image),
                'sha256': hashlib.sha256(image).hexdigest(),
            }

    def install(self, index):
        folder = self.work / f'extracted/{prepare.VERSION}/aoc/{index}'
        folder.mkdir(parents=True)
        (folder / 'romfs.bin').write_bytes(f'image-{index}'.encode())
        (folder / 'index.json').write_text(json.dumps({
            'status': 'pass',
            'title_id': prepare.EXPECTED['aoc'][index]['title_id'],
            'source': str(self.work / f'{index}.nsp'),
        }))
        return folder

    def test_optional_installations(self):
        for indices in ((), ('1',), ('2',), ('1', '2')):
            with self.subTest(indices=indices), tempfile.TemporaryDirectory() as tmp:
                for index in indices:
                    self.install(index)
                self.assertEqual(prepare.addons_config(self.work, self.out), len(indices))
                header = (self.out / 'addons_config.h').read_text()
                self.assertIn(f'#define UNICORN_AOC_COUNT {len(indices)}\n', header)
                for index in indices:
                    self.assertIn(f'aoc/{index}/romfs.bin', header)
                if not indices:
                    self.assertIn('#define UNICORN_AOC_ROWS {0}\n', header)
                for index in indices:
                    (self.work / f'extracted/{prepare.VERSION}/aoc/{index}').rename(Path(tmp) / index)

    def test_incomplete_installation_is_rejected(self):
        folder = self.install('1')
        (folder / 'romfs.bin').unlink()
        with self.assertRaises(FileNotFoundError):
            prepare.addons_config(self.work, self.out)

    def test_corrupted_image_is_rejected(self):
        folder = self.install('1')
        (folder / 'romfs.bin').write_bytes(b'changed')
        with self.assertRaises(AssertionError):
            prepare.addons_config(self.work, self.out)

    def test_changed_package_is_rejected(self):
        self.install('2')
        (self.work / '2.nsp').write_bytes(b'changed')
        with self.assertRaises(AssertionError):
            prepare.addons_config(self.work, self.out)


if __name__ == '__main__':
    unittest.main()
