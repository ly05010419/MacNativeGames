"""Pinned .sha256 files of the prebuilt tools (issue #2184): a file may hold the bare digest or `shasum -a 256` output
(`<digest>  <name>`, as games/oot/prebuilt/macos-arm64 has since PR #2166). Every pinned file names its binary by its
first word, and the builds compare that word, never the whole line, so the format change cannot stop a build again."""
from pathlib import Path
import hashlib
import re
import subprocess
import unittest

REPO = Path(__file__).resolve().parents[1]


def pinned():
    out = subprocess.run(['git', 'ls-files', 'games/*/prebuilt/*.sha256'], cwd=REPO, capture_output=True, text=True,
                         check=True).stdout.split()
    return [REPO / p for p in out]


class PrebuiltChecksums(unittest.TestCase):
    def test_every_pinned_digest_is_its_binary(self):
        files = pinned()
        self.assertTrue(files)
        for sha_file in files:
            binary = sha_file.with_name(sha_file.name[:-len('.sha256')])
            words = sha_file.read_text().split()
            with self.subTest(file=str(sha_file.relative_to(REPO))):
                self.assertRegex(words[0], r'^[0-9a-f]{64}$')
                if len(words) > 1:
                    self.assertEqual(words[1:], [binary.name])
                self.assertEqual(hashlib.sha256(binary.read_bytes()).hexdigest(), words[0])

    def test_builds_compare_the_digest_word(self):
        # A comparison of the whole line (read_text().strip()) refuses `<digest>  <name>`.
        whole_line = re.compile(r"sha256['\"]?\)*\.read_text\(\)\.strip\(\)")
        for rel in ('build_app.py', 'games/oot/build.py'):
            with self.subTest(file=rel):
                self.assertEqual([n for n, line in enumerate((REPO / rel).read_text().splitlines(), 1)
                                  if whole_line.search(line)], [])


if __name__ == '__main__':
    unittest.main()
