""".githooks/pre-commit: a commit with keys, game containers or extracted ExeFS/RomFS files is stopped."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

HOOK = Path(__file__).resolve().parent.parent / '.githooks/pre-commit'


class PreCommitHook(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.repo = Path(tmp.name)
        self.git('init', '-q')

    def git(self, *args):
        env = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}
        return subprocess.run(['git', *args], cwd=self.repo, env=env, capture_output=True, text=True, check=True)

    def stage(self, path):
        file = self.repo / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(b'x')
        self.git('add', '-f', path)

    def hook(self):
        return subprocess.run([sys.executable, str(HOOK)], cwd=self.repo, capture_output=True, text=True)

    def test_source_files_pass(self):
        self.stage('runtime/loader.c')
        self.stage('docs/exefs-layout.md')
        self.stage('games/alc/mods/1.0.1/60fps/exefs/60fps.pchtxt')
        self.assertEqual(self.hook().returncode, 0)

    def test_game_files_are_stopped(self):
        for path in ('prod.keys', 'dump/game.XCI', 'main.nso', 'games/totk/extracted/romfs/Pack/a.bin', 'ExeFS/main', 'mods/x/exefs/main.npdm'):
            with self.subTest(path=path):
                self.git('rm', '-rq', '--cached', '--ignore-unmatch', '.')
                self.stage(path)
                result = self.hook()
                self.assertEqual(result.returncode, 1)
                self.assertIn(path, result.stderr)

    def test_deleting_a_game_file_is_allowed(self):
        self.stage('old.nca')
        self.git('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-qm', 'x', '--no-verify')
        self.git('rm', '-q', 'old.nca')
        self.assertEqual(self.hook().returncode, 0)


if __name__ == '__main__':
    unittest.main()
