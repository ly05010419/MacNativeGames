"""games/odyssey/scripts/make_mod_manifest.py (issue #1788): the RomFS of Odyssey.app (--app) or of a work folder
(--romfs), no Zelda version options, and the files the app's own overlay (romfs-patches) replaces left out."""
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_mod_manifest import fake_romfs

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'games/odyssey/scripts/make_mod_manifest.py'
FILES = {'LocalizedData/USen/MessageData/SystemMessage.szs': b'english text',
         'SystemData/ProjectGraphicsQuality.szs': b'graphics table'}


def run(*args):
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)], capture_output=True, text=True)


class OdysseyModManifest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.app = self.root/'Odyssey.app'
        self.game = self.app/'Contents/Resources/game'
        fake_romfs(self.game/'romfs', FILES)
        self.mod = self.root/'data/mods/v141'
        (self.mod/'LocalizedData/USen/MessageData').mkdir(parents=True)
        (self.mod/'LocalizedData/USen/MessageData/SystemMessage.szs').write_bytes(b'translated text')

    def tearDown(self):
        self.tmp.cleanup()

    def rows(self):
        return [line.split('\t') for line in (self.mod/'manifest.tsv').read_text().splitlines()[1:]]

    def test_app_writes_the_manifest_against_its_romfs(self):
        done = run(self.mod, '--app', self.app)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.rows(), [['LocalizedData/USen/MessageData/SystemMessage.szs', str(len(b'translated text')),
                                        hashlib.sha256(b'translated text').hexdigest(),
                                        hashlib.sha256(b'english text').hexdigest()]])

    def test_romfs_folder_works_too(self):
        done = run(self.mod, '--romfs', self.game/'romfs')
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual([r[0] for r in self.rows()], ['LocalizedData/USen/MessageData/SystemMessage.szs'])

    def test_a_file_the_apps_own_overlay_replaces_is_left_out(self):
        (self.game/'romfs-patches').mkdir()
        (self.game/'romfs-patches/manifest.tsv').write_text(
            '# path\tsize\tsha256\toriginal_sha256\nSystemData/ProjectGraphicsQuality.szs\t1\t' + '0'*64 + '\t' + '0'*64 + '\n')
        (self.mod/'SystemData').mkdir()
        (self.mod/'SystemData/ProjectGraphicsQuality.szs').write_bytes(b'mod graphics table')
        done = run(self.mod, '--app', self.app)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('left out (the app replaces it itself, romfs-patches): SystemData/ProjectGraphicsQuality.szs', done.stdout)
        self.assertEqual([r[0] for r in self.rows()], ['LocalizedData/USen/MessageData/SystemMessage.szs'])

    def test_zelda_version_option_is_gone(self):
        done = run(self.mod, '--app', self.app, '--version', 'v143')
        self.assertEqual(done.returncode, 2)
        self.assertIn('unrecognized arguments', done.stderr)

    def test_app_and_romfs_cannot_both_be_given(self):
        done = run(self.mod, '--app', self.app, '--romfs', self.game/'romfs')
        self.assertEqual(done.returncode, 2)
        self.assertFalse((self.mod/'manifest.tsv').exists())

    def test_an_app_without_romfs_names_the_fix(self):
        done = run(self.mod, '--app', self.root/'Other.app')
        self.assertEqual(done.returncode, 2)
        self.assertIn('--app', done.stderr)
        self.assertFalse((self.mod/'manifest.tsv').exists())


if __name__ == '__main__':
    unittest.main()
