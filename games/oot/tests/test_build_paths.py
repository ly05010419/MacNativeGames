"""#2045: a work folder whose path has spaces (the default sits next to the dump, named after the game) still builds the
runtime; #2055: the app records what the Update page needs (the checkout, the work folder, the Python, the commit)."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('oot_build_paths', GAME / 'build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)


class WorkPathTests(unittest.TestCase):
    def test_a_path_with_spaces_gets_an_alias_without_them(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / 'The Legend of Zelda Ocarina of Time [NUP]' / 'OcarinaOfTime_build'; work.mkdir(parents=True)
            alias = build.make_path(work)
            self.assertNotIn(' ', str(alias))
            self.assertEqual(alias.resolve(), work.resolve())
            self.assertEqual(build.make_path(work), alias)   # the same alias each time
            plain = Path(tmp) / 'plain'; plain.mkdir()
            self.assertEqual(build.make_path(plain), plain)

    def test_make_refuses_a_work_path_with_spaces_and_says_why(self):
        result = subprocess.run(['make', '-n', '-C', str(GAME), 'WORK=/tmp/a b'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('WORK must not contain spaces', result.stdout + result.stderr)

    def test_both_runtime_builds_use_the_alias(self):
        source = (GAME / 'build.py').read_text()
        self.assertEqual(source.count("'WORK='+str(make_path(work))"), 2)
        self.assertNotIn("'WORK='+str(work)", source)


class AppRunningTests(unittest.TestCase):
    """#1784 (Ocarina of Time, after 577e4576 fixed the other apps): the Update window runs the game's executable with
    --update-install <pid> or --update-check and waits for the installer, so the installer must not wait for it."""
    APP = Path('/Apps/Ocarina of Time.app')

    def running(self, *commands):
        exe = str((self.APP / 'Contents/MacOS/OcarinaOfTime').resolve())
        table = '\n'.join(f'{100 + i} {exe}' for i in range(len(commands)))
        args = {str(100 + i): f'{exe} {c}'.rstrip() for i, c in enumerate(commands)}
        def fake(cmd, **kw):
            out = table if cmd[:3] == ['ps', '-axo', 'pid=,comm='] else args[cmd[cmd.index('-p') + 1]]
            return subprocess.CompletedProcess(cmd, 0, out, '')
        from unittest import mock
        with mock.patch.object(build.subprocess, 'run', fake):
            return build.app_running(self.APP)

    def test_the_update_window_is_not_the_game(self):
        self.assertFalse(self.running('--update-install 4242'))
        self.assertFalse(self.running('--update-check'))

    def test_the_game_is_running(self):
        self.assertTrue(self.running(''))
        self.assertTrue(self.running('--update-check', ''))
        self.assertTrue(self.running('--update-install x'))   # not the helper's form: treated as the game


class UpdateInfoTests(unittest.TestCase):
    def test_the_app_records_its_checkout_work_folder_python_and_commit(self):
        source = (GAME / 'build.py').read_text()
        self.assertIn('build_app.record_update_source(info,work)', source)
        self.assertIn("info['MNGSourceCommit']=commit", source)


if __name__ == '__main__':
    unittest.main()
