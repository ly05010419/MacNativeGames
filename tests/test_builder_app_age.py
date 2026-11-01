"""tools/builder/app_age.py and its notice (S8.1, revision 2): a real git repository in a temporary folder with commits
at chosen times, a fake app whose Info.plist has a chosen mtime; the controller with its own InlineJobs; the Build
tab's line on a real withdrawn Tk root against the FakeController."""
import os
import plistlib
from pathlib import Path
import subprocess
import tempfile
import unittest

from builder_fake_process import Clock, drain
from builder_tk_base import Base as TkBase
from tools.builder import app_age
from tools.builder.checks import CheckContext
from tools.builder.config import SettingsStore
from tools.builder.controller import BuildController
from tools.builder.games import BOTW, TOTK
from tools.builder.jobs import InlineJobs
from tools.builder.ports import CommandRunner, RealClock
from tools.builder.session import AppAge

APP_TIME = 1_800_000_000


class Repo(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name).resolve()
        self.repo = self.tmp / 'repo'
        self.repo.mkdir()
        self.git('init', '-q')
        self.commit('build_app.py', APP_TIME - 100)
        self.app = self.tmp / 'games/Tears of the Kingdom.app'
        (self.app / 'Contents').mkdir(parents=True)
        (self.app / 'Contents/Info.plist').write_text('plist')
        os.utime(self.app / 'Contents/Info.plist', (APP_TIME, APP_TIME))
        self.runner = CommandRunner(RealClock())

    def git(self, *args, when=None):
        env = {**os.environ, 'GIT_AUTHOR_NAME': 't', 'GIT_AUTHOR_EMAIL': 't@t', 'GIT_COMMITTER_NAME': 't',
               'GIT_COMMITTER_EMAIL': 't@t'}
        if when is not None:
            env.update(GIT_AUTHOR_DATE=f'{when} +0000', GIT_COMMITTER_DATE=f'{when} +0000')
        subprocess.run(['git', '-C', str(self.repo), *args], check=True, capture_output=True, env=env)

    def commit(self, path, when):
        (self.repo / path).parent.mkdir(parents=True, exist_ok=True)
        (self.repo / path).write_text(str(when))
        self.git('add', '-A')
        self.git('commit', '-q', '-m', path, when=when)

    def older(self, spec=TOTK, **fields):
        fields = fields or {'out': str(self.app)}
        return app_age.older(self.runner, self.repo, spec, tuple(fields.items()))


class Model(Repo):
    def test_only_commits_to_what_a_build_uses_count(self):
        self.assertIs(self.older(), False)                              # the app is newer than the code
        self.commit('docs/notes.md', APP_TIME + 10)
        self.assertIs(self.older(), False)                              # documentation does not change the game
        self.commit('games/botw/assets/x.json', APP_TIME + 20)
        self.assertEqual((self.older(TOTK), self.older(BOTW)), (False, True))   # BotW's own folder
        self.commit('games/totk/runtime/x.c', APP_TIME + 30)
        self.assertEqual((self.older(TOTK), self.older(BOTW)), (True, True))    # the runtime both games share

    def test_the_apps_path_is_the_builds_own(self):
        game = self.tmp / 'games/totk.xci'
        self.commit('vendor/x', APP_TIME + 10)
        self.assertIs(self.older(game=str(game)), True)                 # next to the game file, as build_app.py
        self.assertIs(self.older(game=str(game), out=str(self.tmp / 'elsewhere.app')), None)   # out wins: no app

    def test_the_apps_own_commit_wins_over_times(self):
        built = subprocess.run(['git', '-C', str(self.repo), 'rev-parse', '--short=7', 'HEAD'], capture_output=True,
                               text=True, check=True).stdout.strip()
        plist = self.app / 'Contents/Info.plist'
        plist.write_bytes(plistlib.dumps({'MNGSourceCommit': built}))
        os.utime(plist, (APP_TIME, APP_TIME))
        self.commit('games/totk/merged.c', APP_TIME - 50)          # merged later, with its older date
        self.assertIs(self.older(), True)                           # times alone would say no
        plist.write_bytes(plistlib.dumps({'MNGSourceCommit': 'abcdef0'}))   # a commit this checkout lacks
        os.utime(plist, (APP_TIME, APP_TIME))
        self.assertIs(self.older(), False)                          # then the times decide

    def test_nothing_is_said_when_nothing_can_be_known(self):
        self.commit('vendor/x', APP_TIME + 10)
        self.assertIsNone(self.older(keys=str(self.tmp / 'k')))         # no game file and no output
        self.assertIsNone(self.older(out=str(self.tmp / 'missing.app')))
        self.assertIsNone(app_age.older(self.runner, self.tmp / 'not-a-repo', TOTK, (('out', str(self.app)),)))


class Controller(Repo):
    def controller(self, age_jobs):
        return BuildController(base_context=CheckContext(runner=None), system_checks=(), input_checks=(),
                               store=SettingsStore(self.tmp / 'settings.json', Clock()), process_factory=None,
                               jobs=InlineJobs(), runner=self.runner, clock=Clock(), repo_root=self.repo,
                               logs_dir=self.tmp / 'logs', age_jobs=age_jobs)

    def test_each_selection_is_answered_and_a_stale_answer_dropped(self):
        jobs = InlineJobs()
        ctl = self.controller(jobs)
        self.commit('games/totk/x', APP_TIME + 10)
        ctl.select_game('totk')
        ctl.set_paths(out=str(self.app))
        drain(jobs, ctl.tick)
        self.assertEqual([e for e in ctl.take_events() if isinstance(e, AppAge)], [AppAge('totk', True)])
        ctl.set_paths(out=str(self.tmp / 'other.app'))
        jobs.run_next()
        ctl.set_paths(out=str(self.app))                                # changed before the answer was read
        ctl.tick()
        drain(jobs, ctl.tick)
        self.assertEqual([e for e in ctl.take_events() if isinstance(e, AppAge)], [AppAge('totk', True)])

    def test_no_queue_asks_nothing(self):
        ctl = self.controller(None)
        ctl.select_game('totk')
        ctl.set_paths(out=str(self.app))
        ctl.tick()
        self.assertEqual([e for e in ctl.take_events() if isinstance(e, AppAge)], [])


class Notice(TkBase):
    def test_the_build_tab_says_when_the_app_is_older(self):
        self.start()
        self.loaded('totk')
        older = self.view.checks.older
        self.view.on_event(AppAge('totk', True))
        self.assertEqual(older.winfo_manager(), 'pack')
        self.view.checks.clear()                                        # Check or a changed path leaves it
        self.assertEqual(older.winfo_manager(), 'pack')
        for answer in (False, None):
            self.view.on_event(AppAge('totk', True))
            self.view.on_event(AppAge('totk', answer))
            self.assertEqual(older.winfo_manager(), '', answer)


if __name__ == '__main__':
    unittest.main()
