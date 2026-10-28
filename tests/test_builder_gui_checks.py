"""tools/builder/gui_checks.py (S2.3, R1): the checks panel on a real withdrawn Tk root (builder_tk_base.py), against
the scripted FakeController. Each pump delivers one scripted batch."""
import unittest

from builder_tk_base import Base
from tools.builder.checks import CheckResult, Status
from tools.builder.session import CheckResultReady, ChecksDone, ChecksInvalidated, PathsIncomplete, ProcessError

OK_ROW = [CheckResultReady(CheckResult('git', Status.OK, 'ok', '/usr/bin/git'))]
READY = [OK_ROW, [ChecksDone(True)]]


class ChecksBase(Base):
    def chosen(self, game='totk', **fake):
        self.start(**fake)
        self.loaded(game)
        self.view.refresh()

    def check(self, pumps=2):
        self.view.checks.check_button.invoke()
        for _ in range(pumps):
            self.view.pump()

    def state(self, button):
        return getattr(self.view.checks, f'{button}_button').cget('state')


class Checks(ChecksBase):
    def test_check_needs_a_game(self):
        self.start(checks=READY)
        self.loaded()
        self.view.refresh()
        self.assertEqual(self.state('check'), 'disabled')

    def test_check_commits_typed_paths_first(self):
        self.chosen(checks=READY)
        self.type('keys', '/k')
        self.check()
        self.assertEqual(self.ctl.calls[-2:], [('set_paths', {'keys': '/k'}), 'run_checks'])

    def test_a_block_with_a_fix_and_copy(self):
        block = CheckResult('homebrew', Status.BLOCK, 'homebrew_missing', '/opt/homebrew/bin/brew', 'install-brew')
        self.chosen(checks=[[CheckResultReady(block)], [ChecksDone(False)]])
        self.check()
        self.assertIn('BLOCK  Homebrew: Homebrew is not installed (/opt/homebrew/bin/brew is missing).',
                      self.labels(self.view.checks.rows))
        self.assertIn('Fix: install-brew', self.labels(self.view.checks.rows))
        copy = next(w.button for row in self.view.checks.rows.winfo_children() for w in row.winfo_children()
                    if w.winfo_class() == 'FlatButton')
        copy.invoke()
        self.assertEqual((self.copied, self.state('build')), (['install-brew'], 'disabled'))

    def test_build_follows_a_ready_checks_done(self):
        self.chosen(checks=READY)
        self.check(pumps=1)
        self.assertEqual(self.state('build'), 'disabled')       # results so far, not done yet
        self.view.pump()
        self.assertEqual((self.state('build'), self.view.checks.status.cget('text')), ('normal', 'Ready to build.'))

    def test_invalidated_clears_and_disables(self):
        self.chosen(checks=READY)
        self.check()
        self.ctl.events.append(ChecksInvalidated())
        self.view.pump()
        self.assertEqual((self.state('build'), self.labels(self.view.checks.rows)), ('disabled', []))

    def test_a_path_change_disables_build_at_once(self):                      # closing audit 3
        self.chosen(checks=READY)
        self.check()
        self.type('keys', '/x')
        self.commit('keys', '<FocusOut>')
        self.assertEqual(self.state('build'), 'disabled')      # before the pump brings ChecksInvalidated

    def test_a_game_switch_disables_build_at_once(self):                      # S3.4, f2
        self.chosen(checks=READY)
        self.check()
        self.radio('botw').invoke()
        self.assertEqual(self.state('build'), 'disabled')      # before the pump brings ChecksInvalidated

    def test_checking_again_disables_build_at_once(self):                     # g1
        self.chosen(checks=READY)
        self.check()
        self.view.checks.check_button.invoke()
        self.assertEqual(self.state('build'), 'disabled')

    def test_build_with_a_changed_path_starts_nothing(self):
        self.chosen(checks=READY)
        self.check()
        self.type('keys', '/new')
        self.view.checks.build_button.invoke()
        self.assertNotIn('start_build', self.ctl.calls)
        self.assertEqual((self.state('build'), self.view.checks.status.cget('text')),
                         ('disabled', 'A path changed: check again.'))

    def test_paths_incomplete_and_a_checks_error(self):
        self.chosen(checks=[[PathsIncomplete(('keys',))]])
        self.check(pumps=1)
        self.assertEqual(self.view.checks.status.cget('text'), 'Missing: Keys (prod.keys).')
        self.chosen(checks=[[ProcessError('checks', 'boom'), ChecksDone(False)]])
        self.check(pumps=1)
        self.assertIn('checks: boom', self.labels(self.view.checks.rows))


if __name__ == '__main__':
    unittest.main()
