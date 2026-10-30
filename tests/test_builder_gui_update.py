"""tools/builder/gui_update.py (S4.1): the window's update line on a real withdrawn Tk root (builder_tk_base.py),
against the scripted FakeController. The window checks for updates when it opens."""
import unittest

from builder_tk_base import Base
from tools.builder.session import ProcessError, UpdateApplied, UpdateStatusReady
from tools.builder.strings import text
from tools.builder.updates import UpdateResult, UpdateState, UpdateStatus

AVAILABLE = UpdateStatus(UpdateState.AVAILABLE, 'main', 'origin/main', 7, tuple(f'h{i} subject {i}' for i in range(7)))


class Panel(Base):
    def deliver(self, event):
        self.ctl.events.append(event)
        self.view.pump()
        return self.view.updates

    def test_it_checks_on_its_own_when_the_window_opens(self):
        self.start()
        self.assertEqual((self.ctl.update_calls, self.view.updates.status.cget('text')),
                         (['check_updates'], text('gui.update.checking')))

    def test_an_update_available_lists_the_commits_and_enables_update(self):
        self.start()
        panel = self.deliver(UpdateStatusReady(AVAILABLE))
        self.assertEqual(panel.status.cget('text'), 'Update available: 7 new commit(s) on origin/main.')
        lines = panel.subjects.cget('text').splitlines()                     # S4.4: the commits are in Details
        self.assertEqual((lines[0], lines[-1], len(lines)), ('h0 subject 0', '...', 6))
        self.assertEqual(panel.details.winfo_manager(), '')                 # folded at start
        panel.toggle_details()
        self.assertEqual(panel.details.winfo_manager(), 'pack')
        self.assertEqual(panel.update_button.cget('state'), 'normal')
        panel.update_button.invoke()
        self.assertEqual((self.ctl.update_calls[-1], panel.update_button.cget('state')),
                         ('apply_update', 'disabled'))                       # S4.4b B3: disabled by the click
        self.view.pump()
        self.assertEqual(panel.update_button.cget('state'), 'disabled')         # not twice

    def test_every_state_has_a_text_and_only_available_enables_update(self):
        self.start()
        for state in UpdateState:
            with self.subTest(state=state):
                panel = self.deliver(UpdateStatusReady(UpdateStatus(state, 'main', 'origin/main', 1, (), 'net down')))
                self.assertTrue(panel.status.cget('text'))
                self.assertEqual(panel.update_button.cget('state'),
                                 'normal' if state is UpdateState.AVAILABLE else 'disabled')

    def test_it_names_the_repository_it_updates(self):                        # F3
        self.start()
        self.assertEqual(self.view.updates.where.cget('text'), 'Repository: /repo')

    def test_after_an_update_reopen_before_building_but_check_again(self):    # F2
        self.start()
        self.loaded('totk')
        self.deliver(UpdateStatusReady(AVAILABLE))                       # S4.4b: the check at open has answered
        panel = self.deliver(UpdateApplied(UpdateResult(True, 'aaa', 'bbb')))
        reopen = text('gui.update.reopen')
        self.assertEqual(panel.status.cget('text'), text('gui.update.done', old='aaa', new='bbb') + '\n' + reopen)
        self.assertEqual({self.view.checks.check_button.cget('state'), self.view.checks.build_button.cget('state'),
                          panel.update_button.cget('state')}, {'disabled'})
        self.assertEqual(panel.check_button.cget('state'), 'normal')
        panel.check_button.invoke()
        self.assertEqual(self.ctl.update_calls[-1], 'check_updates')
        panel = self.deliver(UpdateStatusReady(AVAILABLE))                      # more commits came: update again
        self.assertEqual((panel.status.cget('text').splitlines()[-1], panel.update_button.cget('state')),
                         (reopen, 'normal'))
        panel = self.deliver(UpdateApplied(UpdateResult(False, 'bbb', 'bbb', 'offline')))
        self.assertEqual((panel.restart_needed, panel.status.cget('text').splitlines()[-1]), (True, reopen))
        self.assertEqual(self.view.checks.check_button.cget('state'), 'disabled')

    def test_a_failed_update_says_why_and_allows_building(self):
        self.start()
        self.loaded('totk')
        panel = self.deliver(UpdateApplied(UpdateResult(False, 'aaa', 'aaa', 'not possible to fast-forward')))
        self.assertEqual(panel.status.cget('text'), 'The update did not run: not possible to fast-forward')
        self.assertEqual((panel.restart_needed, self.view.checks.check_button.cget('state')), (False, 'normal'))
        panel = self.deliver(ProcessError('update', 'boom'))
        self.assertEqual(panel.status.cget('text'), 'The update did not run: boom')

    def test_one_click_one_command(self):                                      # S4.4b, B3
        self.start()
        panel = self.deliver(UpdateStatusReady(AVAILABLE))
        panel.update_button.invoke()
        panel.update_button.invoke()                                  # before the pump
        panel.check_button.invoke()
        self.assertEqual((self.ctl.update_calls.count('apply_update'), self.callback_errors), (1, []))
        self.assertEqual(self.ctl.update_calls.count('check_updates'), 1)            # only the one at open

    def test_check_for_updates_waits_for_its_answer(self):                     # S4.4b, F6
        self.start()
        self.view.pump()
        self.assertEqual(self.view.updates.check_button.cget('state'), 'disabled')    # the check at open runs
        panel = self.deliver(UpdateStatusReady(AVAILABLE))
        self.assertEqual(panel.check_button.cget('state'), 'normal')

    def test_up_to_date_is_short_and_the_branch_is_in_details(self):
        self.start()
        panel = self.deliver(UpdateStatusReady(UpdateStatus(UpdateState.UP_TO_DATE, 'main', 'origin/main')))
        self.assertEqual(panel.status.cget('text'), 'Up to date.')
        self.assertEqual(panel.follows.cget('text'), text('gui.update.follows', branch='main', upstream='origin/main'))
        self.assertEqual(panel.update_button.cget('state'), 'disabled')

    def test_head_moved_means_reopen_even_when_git_said_no(self):              # S4.4b, E1
        self.start()
        panel = self.deliver(UpdateApplied(UpdateResult(False, 'aaa', 'bbb', 'timed out')))
        self.assertTrue(panel.restart_needed)


if __name__ == '__main__':
    unittest.main()
