"""tools/builder/updates.py (S4.1, U1-U2): the update check and the fast-forward, against real git repositories in a
temporary folder: a bare "remote", a clone that publishes commits, and the player's clone. S4.5 rev 2: the bare remote
stands for MacNativeGames (the project is given by its path); the fork and private-remote cases are in
test_builder_updates_project.py."""
import os
from unittest.mock import patch
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tools.builder.ports import CommandResult, CommandRunner, Outcome, RealClock
from tools.builder.updates import UpdateState, apply_update, check_updates

ENV = {**os.environ, 'GIT_TERMINAL_PROMPT': '0', 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_SYSTEM': os.devnull}


def git(cwd, *args):
    return subprocess.run(['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
                           '-c', 'init.defaultBranch=main', *args], cwd=cwd, env=ENV, check=True, capture_output=True,
                          text=True, timeout=30).stdout.strip()


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp)
        git(self.tmp, 'init', '--bare', '-q', 'remote.git')
        git(self.tmp, 'clone', '-q', 'remote.git', 'seed')
        self.seed, self.player = self.tmp / 'seed', self.tmp / 'player'
        self.publish('first', 'build_app.py', 'v1\n')
        git(self.tmp, 'clone', '-q', 'remote.git', 'player')
        self.runner = CommandRunner(RealClock())
        self.project = str((self.tmp / 'remote.git').resolve())        # git records the real path

    def publish(self, message, name='notes.txt', content=None):
        (self.seed / name).write_text(content if content is not None else message + '\n')
        git(self.seed, 'add', name)
        git(self.seed, 'commit', '-q', '-m', message)
        git(self.seed, 'push', '-q', 'origin', 'HEAD:main')

    def check(self, repo=None):
        return check_updates(self.runner, repo or self.player, self.project)

    def apply(self, repo=None):
        return apply_update(self.runner, repo or self.player, self.project)


class Check(Base):
    def test_up_to_date(self):
        status = self.check()
        self.assertEqual((status.state, status.branch, status.upstream, status.waiting),
                         (UpdateState.UP_TO_DATE, 'main', 'origin/main', 0))

    def test_commits_waiting_with_their_subjects_newest_first(self):
        self.publish('second')
        self.publish('third')
        status = self.check()
        self.assertEqual((status.state, status.waiting), (UpdateState.AVAILABLE, 2))
        self.assertEqual([s.split(' ', 1)[1] for s in status.subjects], ['third', 'second'])

    def test_untracked_files_do_not_stop_it(self):
        self.publish('second')
        (self.player / 'my-notes.txt').write_text('mine\n')
        self.assertEqual(self.check().state, UpdateState.AVAILABLE)

    def test_local_changes(self):
        self.publish('second')
        (self.player / 'build_app.py').write_text('edited here\n')
        self.assertEqual(self.check().state, UpdateState.LOCAL_CHANGES)

    def test_diverged(self):
        self.publish('second')
        (self.player / 'mine.txt').write_text('mine\n')
        git(self.player, 'add', 'mine.txt')
        git(self.player, 'commit', '-q', '-m', 'my own commit')
        self.assertEqual(self.check().state, UpdateState.DIVERGED)

    def test_not_a_git_clone(self):
        folder = self.tmp / 'from-a-zip'
        folder.mkdir()
        self.assertEqual(self.check(folder).state, UpdateState.NOT_GIT)

    def test_a_branch_without_upstream_compares_with_the_project(self):      # S4.5 rev 2, U4'
        git(self.player, 'checkout', '-q', '-b', 'local-only')
        status = self.check()
        self.assertEqual((status.state, status.branch, status.upstream),
                         (UpdateState.UP_TO_DATE, 'local-only', 'origin/main'))
        self.publish('second')
        status = self.check()
        self.assertEqual((status.state, status.waiting), (UpdateState.OTHER_BRANCH, 1))

    def test_an_unreachable_remote(self):
        git(self.player, 'remote', 'set-url', 'origin', str(self.tmp / 'moved.git'))
        self.project = str(self.tmp / 'moved.git')                       # still the project's URL, now unreachable
        status = self.check()
        self.assertEqual(status.state, UpdateState.OFFLINE)
        self.assertTrue(status.detail)


class Apply(Base):
    def test_a_fast_forward(self):
        self.publish('second', 'build_app.py', 'v2\n')
        result = self.apply()
        self.assertTrue(result.ok, result.detail)
        self.assertNotEqual(result.old, result.new)
        self.assertEqual(((self.player / 'build_app.py').read_text(), self.check().state), ('v2\n', UpdateState.UP_TO_DATE))

    def test_local_changes_are_refused_and_kept(self):
        self.publish('second', 'build_app.py', 'v2\n')
        (self.player / 'build_app.py').write_text('edited here\n')
        result = self.apply()
        self.assertEqual((result.ok, result.old, result.new), (False, result.old, result.old))
        self.assertEqual((self.player / 'build_app.py').read_text(), 'edited here\n')

    def test_pull_rebase_set_by_the_player_changes_nothing(self):
        git(self.player, 'config', 'pull.rebase', 'true')
        self.publish('second', 'build_app.py', 'v2\n')
        self.assertTrue(self.apply().ok)
        self.assertEqual(git(self.player, 'rev-list', '--merges', '--count', 'HEAD'), '0')    # no merge commit

    def test_a_diverged_branch_is_not_merged(self):
        self.publish('second')
        (self.player / 'mine.txt').write_text('mine\n')
        git(self.player, 'add', 'mine.txt')
        git(self.player, 'commit', '-q', '-m', 'my own commit')
        before = git(self.player, 'rev-parse', '--short', 'HEAD')
        result = self.apply()
        self.assertEqual((result.ok, result.new), (False, before))
        self.assertEqual(git(self.player, 'rev-parse', '--short', 'HEAD'), before)


class OwnRepositoryOnly(Base):                                                  # S4.4b, B2 and E2
    def test_a_folder_inside_another_work_tree_is_not_a_clone(self):
        self.publish('second', 'build_app.py', 'v2\n')
        unpacked = self.player / 'MacNativeGames-from-a-zip'
        unpacked.mkdir()
        before = git(self.player, 'rev-parse', 'HEAD')
        self.assertEqual(self.check(unpacked).state, UpdateState.NOT_GIT)
        result = self.apply(unpacked)
        self.assertFalse(result.ok)
        self.assertEqual((git(self.player, 'rev-parse', 'HEAD'), (self.player / 'build_app.py').read_text()),
                         (before, 'v1\n'))

    def test_a_symlinked_path_to_the_clone_still_works(self):
        link = self.tmp / 'link'
        link.symlink_to(self.player)
        self.publish('second')
        self.assertEqual(self.check(link).state, UpdateState.AVAILABLE)

    def test_a_differently_cased_path_to_the_clone_still_works(self):
        other = Path(str(self.player).swapcase())
        if not other.exists():
            self.skipTest('this volume tells letter case apart')
        self.publish('second')
        self.assertEqual(self.check(other).state, UpdateState.AVAILABLE)

    def failing(self, *words):
        """A runner whose git calls containing all words fail; the others run for real."""
        real = self.runner.run
        def run(argv, timeout_s):
            if all(w in argv for w in words):
                return CommandResult(Outcome.NONZERO, 128, '', 'fatal: simulated', 0.0, None, True)
            return real(argv, timeout_s=timeout_s)
        return run

    def test_a_failed_status_counts_as_local_changes(self):
        self.publish('second', 'build_app.py', 'v2\n')
        with patch.object(self.runner, 'run', self.failing('status')):
            self.assertEqual(self.check().state, UpdateState.LOCAL_CHANGES)
            self.assertFalse(self.apply().ok)
        self.assertEqual((self.player / 'build_app.py').read_text(), 'v1\n')

    def test_a_failed_count_is_offline_not_up_to_date(self):
        self.publish('second')
        with patch.object(self.runner, 'run', self.failing('rev-list', '--count')):
            status = self.check()
        self.assertEqual(status.state, UpdateState.OFFLINE)
        self.assertTrue(status.detail)


if __name__ == '__main__':
    unittest.main()
