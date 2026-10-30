"""S4.5 (U1'-U4'): updates come from MacNativeGames only, found by URL. Real git repositories in a temporary folder:
bare ones standing for MacNativeGames, a fork and a private remote; a recording runner shows which remotes are
fetched."""
from unittest.mock import patch

from test_builder_updates import Base, git
from tools.builder.updates import PROJECT, UpdateState, apply_update, check_updates, normalise, project_remote


class Remotes(Base):
    def setUp(self):
        super().setUp()
        self.mng = self.project                                         # the base's bare remote is MacNativeGames
        for name in ('fork.git', 'private.git'):
            git(self.tmp, 'clone', '-q', '--bare', self.mng, name)
        self.fork, self.private = (str((self.tmp / n).resolve()) for n in ('fork.git', 'private.git'))
        self.fetched = []
        real = self.runner.run

        def run(argv, timeout_s):
            if 'fetch' in argv:
                self.fetched.append(argv[argv.index('fetch') + 1:])
            return real(argv, timeout_s=timeout_s)
        patcher = patch.object(self.runner, 'run', run)
        patcher.start()
        self.addCleanup(patcher.stop)

    def push_to(self, remote, message, branch='main'):
        """A commit that reaches only that bare repository."""
        other = self.tmp / f'other-{len(list(self.tmp.iterdir()))}'
        git(self.tmp, 'clone', '-q', remote, other.name)
        if branch != 'main':
            git(other, 'checkout', '-q', branch)
        (other / 'notes.txt').write_text(message + '\n')
        git(other, 'add', 'notes.txt')
        git(other, 'commit', '-q', '-m', message)
        git(other, 'push', '-q', 'origin', f'HEAD:{branch}')

    def test_a_fork_as_origin_and_macnativegames_as_upstream(self):
        git(self.tmp, 'clone', '-q', self.fork, 'forked')
        player = self.tmp / 'forked'
        git(player, 'remote', 'add', 'upstream', self.mng)
        self.publish('on MacNativeGames')
        self.push_to(self.fork, 'only on the fork')
        status = check_updates(self.runner, player, self.mng)
        self.assertEqual((status.state, status.upstream, status.waiting), (UpdateState.AVAILABLE, 'upstream/main', 1))
        self.assertEqual([s.split(' ', 1)[1] for s in status.subjects], ['on MacNativeGames'])
        self.assertTrue(apply_update(self.runner, player, self.mng).ok)
        self.assertEqual(git(player, 'rev-parse', 'HEAD'), git(self.seed, 'rev-parse', 'HEAD'))
        self.assertEqual(self.fetched, [['--quiet', 'upstream']] * 2)

    def test_a_branch_following_a_private_remote_is_compared_with_macnativegames(self):
        git(self.player, 'remote', 'add', 'mine', self.private)
        git(self.player, 'checkout', '-q', '-b', 'friendly')
        git(self.player, 'push', '-q', '-u', 'mine', 'friendly')
        pushed = git(self.player, 'rev-parse', 'mine/friendly')
        self.push_to(self.private, 'only on the private remote', 'friendly')
        self.publish('second')
        self.publish('third')
        status = check_updates(self.runner, self.player, self.mng)
        self.assertEqual((status.state, status.branch, status.upstream, status.waiting),
                         (UpdateState.OTHER_BRANCH, 'friendly', 'origin/main', 2))
        self.assertEqual(git(self.player, 'rev-parse', 'mine/friendly'), pushed)       # not fetched
        self.assertEqual(self.fetched, [['--quiet', 'origin']])
        result = apply_update(self.runner, self.player, self.mng)                      # not on main: no update
        self.assertEqual((result.ok, result.old, result.new), (False, result.old, result.old))
        self.assertEqual(self.fetched, [['--quiet', 'origin']])

    def test_no_remote_for_macnativegames_fetches_nothing(self):
        git(self.tmp, 'clone', '-q', self.fork, 'forked')
        status = check_updates(self.runner, self.tmp / 'forked', self.mng)
        self.assertEqual((status.state, status.upstream), (UpdateState.NO_PROJECT, None))
        self.assertFalse(apply_update(self.runner, self.tmp / 'forked', self.mng).ok)
        self.assertEqual(self.fetched, [])

    def test_two_remotes_for_macnativegames(self):
        git(self.player, 'remote', 'add', 'zz', self.mng.removesuffix('.git'))
        git(self.player, 'remote', 'add', 'aa', self.mng + '/')
        self.assertEqual(project_remote(self.runner, self.player, self.mng), 'origin')   # the one main follows
        git(self.player, 'branch', '-q', '--unset-upstream')
        self.assertEqual(project_remote(self.runner, self.player, self.mng), 'aa')       # else the first by name


class Normalise(Base):
    def test_the_forms_of_the_project_url(self):
        same = ('https://github.com/ly05010419/MacNativeGames.git', 'git@github.com:ly05010419/MacNativeGames',
                'ssh://git@github.com/ly05010419/MacNativeGames/', 'https://GitHub.com/ly05010419/MacNativeGames',
                'ssh://git@github.com:22/ly05010419/MacNativeGames.git',
                'https://user@github.com/ly05010419/MacNativeGames')
        for url in same:
            with self.subTest(url=url):
                self.assertEqual(normalise(url), PROJECT)
        for url in ('https://github.com/someone/MacNativeGames',
                    'https://dev.example.com/someone/project/_git/project'):
            with self.subTest(url=url):
                self.assertNotEqual(normalise(url), PROJECT)
