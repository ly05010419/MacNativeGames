"""No lock or unlock call inside a // comment in the shared Zelda graphics layer and Ocarina of Time's runtime.

2026-11-18: a diagnostic statement was put in front of the end of a line, and its trailing "// ..." comment swallowed the
rest of that line: `pthread_mutex_unlock(&g_pq_mu)` in the present command buffer's completion handler (nvn_mtl.m). The
lock was never given back, the present thread and the display link waited for it, and Ocarina of Time froze at its second
frame. Every syntax check passed: the code was still valid C. A lock call in a comment is how that looks in the source."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOLDERS = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn', ROOT / 'games/totk/runtime', ROOT / 'games/oot/runtime')
CALL = re.compile(r'//.*\b(pthread_mutex_(?:un)?lock|pthread_rwlock_(?:un|rd|wr)lock|os_unfair_lock_(?:un)?lock|dispatch_semaphore_signal)\s*\(')


class NoCommentedOutLocks(unittest.TestCase):
    def test_no_lock_call_in_a_line_comment(self):
        found = []
        for folder in FOLDERS:
            for path in sorted(folder.rglob('*')):
                if path.suffix not in ('.c', '.m', '.h', '.inc') or path.is_symlink() or not path.is_file():
                    continue
                for n, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
                    if CALL.search(line):
                        found.append(f'{path.relative_to(ROOT)}:{n}: {line.strip()[:160]}')
        self.assertEqual(found, [], 'a lock or unlock call sits inside a // comment (a comment that swallowed the rest '
                                    'of a line?): put the statement on its own line\n' + '\n'.join(found))


if __name__ == '__main__':
    unittest.main()
