"""The work-folder copy (build_app.prepare_work) of a game folder whose files link into the checkout's shared/ (b7048211):
rsync -l kept the links as they were, so from <work>/totk they pointed beside the work folder and every build and
express update of Tears of the Kingdom and Breath of the Wild stopped at "No rule to make target
'runtime/account.c'" (#2270). macOS's rsync (openrsync) cannot follow such links while it compares by --checksum
("hash_file_by_path: Too many levels of symbolic links", for --copy-unsafe-links and -L alike), so a link out of the
game folder is copied as the file it names by build_app itself; a link inside stays a link."""
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import build_app  # noqa: E402


class WorkCopyLinks(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        root = Path(self.d.name)
        self.repo, self.work, self.log = root / 'repo', root / 'elsewhere/work', root / 'logs/sources.log'
        (self.repo / 'shared/runtime').mkdir(parents=True)
        (self.repo / 'shared/runtime/account.c').write_text('int account;\n')
        self.game = self.repo / 'games/totk'
        (self.game / 'runtime').mkdir(parents=True)
        os.symlink('../../../shared/runtime/account.c', self.game / 'runtime/account.c')   # as in the checkout
        (self.game / 'runtime/own.c').write_text('int own;\n')
        os.symlink('own.c', self.game / 'runtime/alias.c')                                 # a link inside the game folder
        self.copied = self.work / 'totk/runtime'

    def tearDown(self):
        self.d.cleanup()

    def sync(self):
        build_app.sync_folder(self.game, self.work / 'totk', [], self.log)

    def test_a_link_into_shared_arrives_as_its_file(self):
        self.sync()
        self.assertFalse((self.copied / 'account.c').is_symlink())
        self.assertEqual((self.copied / 'account.c').read_text(), 'int account;\n')
        self.assertTrue((self.copied / 'alias.c').is_symlink())                           # still a link, and it resolves
        self.assertEqual((self.copied / 'alias.c').read_text(), 'int own;\n')

    def test_a_dangling_link_left_by_an_earlier_copy_is_mended(self):
        self.copied.mkdir(parents=True)                                                   # every player who met #2270
        os.symlink('../../../shared/runtime/account.c', self.copied / 'account.c')
        self.sync()
        self.assertFalse((self.copied / 'account.c').is_symlink())
        self.assertEqual((self.copied / 'account.c').read_text(), 'int account;\n')

    def test_an_unchanged_file_keeps_its_time_and_a_changed_one_is_copied(self):
        self.sync()
        old = time.time() - 3600
        os.utime(self.copied / 'account.c', (old, old))
        self.sync()                                                                       # make rebuilds nothing
        self.assertEqual((self.copied / 'account.c').stat().st_mtime, old)
        (self.repo / 'shared/runtime/account.c').write_text('int account2;\n')
        self.sync()
        self.assertEqual((self.copied / 'account.c').read_text(), 'int account2;\n')
        self.assertGreater((self.copied / 'account.c').stat().st_mtime, old)

    def test_prepare_work_copies_every_folder_with_it(self):
        source = (ROOT / 'build_app.py').read_text()
        body = source[source.index('def prepare_work('):source.index('\n\n\n', source.index('def prepare_work('))]
        self.assertNotIn("'rsync'", body)
        self.assertEqual(body.count('sync_folder('), 4)   # the game, zeldaVR, games/botw, the Switch 2 games


if __name__ == '__main__':
    unittest.main()
