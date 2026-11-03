"""NSP script --romfs bundle|separate (scripts/build_ipa_from_nsp.py): the option and setting, and the push_romfs.sh command printed for a separate build. No game files or Xcode needed."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', REPO / 'scripts/build_ipa_from_nsp.py')
assert spec is not None and spec.loader is not None
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
spec = importlib.util.spec_from_file_location('romfs_image', REPO / 'dm/tools/romfs_image.py')
assert spec is not None and spec.loader is not None
romfs_image = importlib.util.module_from_spec(spec); spec.loader.exec_module(romfs_image)


class RomfsOption(unittest.TestCase):
    def test_option_and_setting(self):
        self.assertEqual(nsp.ROMFS, 'bundle')   # the default build is the one that always was
        self.assertIsNone(nsp.parse_args([]).romfs)
        self.assertEqual(nsp.parse_args(['--romfs', 'separate']).romfs, 'separate')
        with self.assertRaises(SystemExit), mock.patch('sys.stderr'): nsp.parse_args(['--romfs', 'maybe'])

    def test_separate_cannot_be_a_mac_build(self):
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp, 'apple_silicon', return_value=True):
            with self.assertRaises(nsp.BuildError) as e: nsp.main(nsp.parse_args(['--platform', 'mac', '--romfs', 'separate', '-k', t, t]))
            self.assertIn('--romfs separate', str(e.exception))

    def test_separate_requires_keeping_the_image_for_deployment(self):
        for options, settings in ((['--romfs', 'separate', '--no-keep-extracted'], {'ROMFS': 'bundle', 'KEEP_EXTRACTED': True}),
                                  ([], {'ROMFS': 'separate', 'KEEP_EXTRACTED': False})):
            with self.subTest(options=options, settings=settings), tempfile.TemporaryDirectory() as t, mock.patch.multiple(nsp, **settings):
                with self.assertRaises(nsp.BuildError) as e:
                    nsp.main(nsp.parse_args([*options, '-k', t, t]))
                self.assertIn('--romfs separate requires --keep-extracted', str(e.exception))

    def test_push_command_names_the_image(self):
        said = []
        with tempfile.TemporaryDirectory() as t, mock.patch.object(nsp.PROG, 'say', side_effect=said.append):
            src = Path(t) / 'src'; (src / 'Data').mkdir(parents=True); (src / 'Data' / 'a').write_bytes(b'x')
            romfs_image.pack(src, Path(t) / 'RomFS.img')
            image = nsp.rfs.source(t)   # as main passes it: a romfs.Path, which sees the image's root as a folder (GitHub issue #530)
            nsp.say_push_romfs(image, 'org.nxnative.01007ef00011e000')
            text = '\n'.join(said)
            self.assertIn(f'scripts/push_romfs.sh <device UDID> org.nxnative.01007ef00011e000 "{image}"', text)
            self.assertIn('needed again only when the image changed', text)
            self.assertNotIn('[RED]', text)
            said.clear(); nsp.say_push_romfs(nsp.rfs.Path(src), 'org.x'); self.assertIn('[RED]', '\n'.join(said))   # a folder has no identity


if __name__ == '__main__':
    unittest.main()
