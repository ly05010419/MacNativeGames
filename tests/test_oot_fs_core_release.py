"""Ocarina of Time: a game file read gives its virtual core up while it waits on the drive, as an nn::fs read through the
file system service does on the Switch (2026-11-17: a thread allowed core 1 only held it 35-88 ms in pread under
runtime_romfs_read while other core-1 threads waited and five cores stood idle)."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
FS = ROOT / 'games/oot/runtime/fs_host.c'
THREADS = ROOT / 'games/oot/runtime/threads.c'


def body(src, head):
    start = src.index(head)
    return src[start:src.index('\n}\n', start)]


class ReadGivesCoreUp(unittest.TestCase):
    def test_the_macro_gives_the_core_up_until_return(self):
        self.assertIn('#define FS_AWAY __attribute__((cleanup(runtime_core_back))) int fs_away_ = runtime_core_away()',
                      FS.read_text())

    def test_read_file_is_away_from_its_core_for_every_path(self):
        read = body(FS.read_text(), 'static unsigned read_file(')
        away = read.index('FS_AWAY;')
        for path in ('runtime_romfs_read(', 'BOTW_AOC_READ(', 'pread('):
            self.assertLess(away, read.index(path), f'{path} runs before the core is given up')

    def test_writes_size_changes_flushes_and_commits_too(self):
        src = FS.read_text()
        for head, io in (('static unsigned write_file(', 'pwrite('), ('static unsigned set_file_size(', 'ftruncate('),
                         ('static unsigned flush_file(', 'F_FULLFSYNC'), ('static unsigned commit(', 'F_FULLFSYNC')):
            b = body(src, head)
            self.assertIn('FS_AWAY;', b, head)
            self.assertLess(b.index('FS_AWAY;'), b.index(io), head)

    def test_the_helpers_take_no_core_from_a_thread_that_has_none(self):
        src = THREADS.read_text()
        away = body(src, 'int runtime_core_away(void) {')
        self.assertRegex(away, r'cur_core < 0\) return 0;')
        back = body(src, 'void runtime_core_back(int *away) {')
        self.assertTrue(re.search(r'if \(!\*away \|\| !t\) return;', back), 'back only after a real give-up')


if __name__ == '__main__':
    unittest.main()
