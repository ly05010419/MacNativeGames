"""The window fits (S4.4b, B1): each panel's footer keeps its full height at the default and the minimum window size,
with Advanced and Details open; the body scrolls, shows the panel that changed, answers the wheel and brings a
focused field into view. A real withdrawn Tk root (builder_tk_base.py): positions on screen do not move there, so
visibility is measured in the scroll area's own coordinates (C3), and heights as given against asked for (R1)."""
import unittest

from builder_tk_base import Base, tk
from tools.builder.checks import CheckResult, Status
from tools.builder.session import BuildStarted, CheckResultReady, ChecksDone, Finished, Outcome, StageChanged

if tk is not None:
    from tools.builder.gui_style import MIN_WINDOW, WINDOW

READY = [[CheckResultReady(CheckResult('git', Status.OK, 'ok', ''))], [ChecksDone(True)]]
LOG = '/logs/builder-totk.log'


class Fit(Base):
    def size(self, width, height):
        """A withdrawn root lays out only on a geometry change."""
        self.root.update_idletasks()
        self.root.geometry(f'{width + 1}x{height}')
        self.root.update_idletasks()
        self.root.geometry(f'{width}x{height}')
        self.root.update_idletasks()

    def open_folds(self):
        self.view.files.toggle_advanced()
        self.view.details.toggle_details()

    def fits(self, footer):
        """R1: the footer and every frame up to the form get the height they ask for; its bottom is inside the form."""
        form = footer
        while form.master is not self.root:
            form = form.master
        widget = footer
        while widget is not form.master:
            if widget.winfo_height() < widget.winfo_reqheight():
                return False
            widget = widget.master
        bottom = footer.winfo_rooty() - form.winfo_rooty() + footer.winfo_height()
        return bottom <= form.winfo_height() and self.whole(footer)

    def whole(self, parent):
        """Every button in parent gets the width it asks for and lies inside parent."""
        buttons = [w for w in parent.winfo_children() if w.winfo_class() == 'FlatButton' and w.winfo_ismapped()]
        return all(b.winfo_width() >= b.winfo_reqwidth() and 0 <= b.winfo_x()
                   and b.winfo_x() + b.winfo_width() <= parent.winfo_width() for b in buttons)

    def building(self):
        self.start(checks=READY, build=[[BuildStarted(('py',), LOG)], [StageChanged(1, 4, 'code', 'rewriting')]])
        self.loaded('totk')
        self.view.pump()
        self.view.checks.check_button.invoke()
        self.view.pump()
        self.view.pump()
        self.view.checks.build_button.invoke()
        self.view.pump()
        self.view.pump()

    def test_the_checks_footer_fits(self):
        self.start()
        self.loaded('totk')
        self.open_folds()
        for width, height in (WINDOW, MIN_WINDOW):
            with self.subTest(size=(width, height)):
                self.size(width, height)
                self.assertTrue(self.fits(self.view.checks.check_button.master))
                self.assertTrue(self.whole(self.view.files.frame))          # every Choose... complete
                self.assertEqual(len([w for w in self.view.files.frame.winfo_children()
                                      if w.winfo_class() == 'FlatButton' and w.winfo_ismapped()]), 7)

    def test_the_build_footer_fits(self):
        self.building()
        self.open_folds()
        for width, height in (WINDOW, MIN_WINDOW):
            with self.subTest(size=(width, height)):
                self.size(width, height)
                self.assertTrue(self.fits(self.view.build.cancel_button.master))
                self.assertTrue(self.fits(self.view.build.log_button.master))

    def test_the_done_footer_fits(self):
        self.building()
        self.ctl.busy = False
        self.ctl.events.append(Finished(Outcome.SUCCESS, 0, '/Games/TOTK.app', None, LOG))
        self.view.pump()
        self.open_folds()
        for width, height in (WINDOW, MIN_WINDOW):
            with self.subTest(size=(width, height)):
                self.size(width, height)
                self.assertTrue(self.fits(self.view.done.new_button.master))

    # the scrolling body (R2, R3, C1-C4): new in S4.4b, so these tests did not exist on 73eeaa3
    def visible(self, widget):
        """C3: the widget lies inside the part of the scroll region the canvas shows."""
        area = self.view.area
        area.sync()
        top, bottom = area.canvas.yview()
        height = area.region_height()
        y = area.offset(widget)
        return top * height <= y and y + widget.winfo_reqheight() <= bottom * height + 1

    def test_the_first_result_is_shown_after_check(self):
        self.start(checks=READY)
        self.loaded('totk')
        self.view.pump()
        self.view.files.toggle_advanced()
        self.size(*MIN_WINDOW)
        self.view.checks.check_button.invoke()
        self.view.pump()
        self.view.pump()
        rows = self.view.checks.rows.winfo_children()
        self.assertTrue(rows and self.visible(rows[0]))

    def test_the_build_stage_is_shown_after_build(self):
        self.start(checks=READY, build=[[BuildStarted(('py',), LOG)]])
        self.loaded('totk')
        self.view.pump()
        self.view.files.toggle_advanced()
        self.size(*MIN_WINDOW)
        self.view.checks.check_button.invoke()
        self.view.pump()
        self.view.pump()
        self.view.checks.build_button.invoke()
        self.view.pump()
        self.assertTrue(self.visible(self.view.build.stage))

    def test_the_body_scrolls_when_it_is_taller_than_the_window(self):
        self.start()
        self.loaded('totk')
        self.view.files.toggle_advanced()
        self.size(*MIN_WINDOW)
        self.view.area.sync()
        self.assertGreater(self.view.area.region_height(), self.view.area.canvas.winfo_height())

    def test_one_wheel_notch_moves_the_body_a_little(self):
        self.start(checks=READY)
        self.loaded('totk')
        self.view.pump()
        self.view.files.toggle_advanced()
        self.size(*MIN_WINDOW)
        self.view.checks.check_button.invoke()
        self.view.pump()
        self.view.pump()
        area, row = self.view.area, self.view.checks.rows.winfo_children()[0]
        area.canvas.yview_moveto(0)
        self.assertIn(area.tag, row.bindtags())                         # a row made after start has the tag
        script = self.root.bind_class(area.tag, '<MouseWheel>').replace('%D', '-120')
        self.root.tk.eval(script.replace('break', ''))
        top, bottom = area.canvas.yview()
        self.assertGreater(top, 0)
        self.assertLess(bottom, 1)                                     # moved, not to the end

    def scrollable(self, **fake):
        """MIN_WINDOW with Advanced open: the body is taller than its view."""
        self.start(**fake)
        self.ctl.saved = {'totk': {'game': '/a/long/folder/' * 12 + 'Tears of the Kingdom.xci', 'keys': '/k'}}
        self.loaded('totk')
        self.view.pump()
        self.view.files.toggle_advanced()
        self.size(*MIN_WINDOW)
        self.view.area.sync()

    def test_shift_wheel_scrolls_the_entry_and_the_plain_wheel_the_body(self):
        """C1, with real events: Shift+wheel reaches Entry's own sideways scrolling and leaves the body; the plain
        wheel scrolls the body once and leaves the entry."""
        self.scrollable()
        area, entry = self.view.area, self.entry('game')
        entry.xview_moveto(0)
        area.canvas.yview_moveto(0)
        entry.event_generate('<Shift-MouseWheel>', delta=-120)
        self.assertGreater(entry.xview()[0], 0)
        self.assertEqual(area.canvas.yview()[0], 0)
        entry.xview_moveto(0)
        entry.event_generate('<MouseWheel>', delta=-120)
        self.assertGreater(area.canvas.yview()[0], 0)
        self.assertEqual(entry.xview()[0], 0)

    def test_focus_brings_a_hidden_field_into_view_and_leaves_a_visible_one(self):
        """C4, through the real <FocusIn> event."""
        self.scrollable()
        area = self.view.area
        area.canvas.yview_moveto(0)
        before = area.canvas.yview()
        self.entry('game').event_generate('<FocusIn>')
        self.assertEqual(area.canvas.yview(), before)                  # visible already: nothing moves
        self.assertFalse(self.visible(self.entry('out')))
        self.entry('out').event_generate('<FocusIn>')
        self.assertTrue(self.visible(self.entry('out')))

    def test_show_log_brings_the_log_into_sight(self):
        """N1: after Show log at least 100 px of the log (or all of it) are in the visible part of the body."""
        for width, height in (WINDOW, MIN_WINDOW):
            with self.subTest(size=(width, height)):
                self.building()
                self.view.files.toggle_advanced()
                self.size(width, height)
                self.view.build.log_button.invoke()
                area, log = self.view.area, self.view.build.log
                area.sync()
                top, bottom = (f * area.region_height() for f in area.canvas.yview())
                y = area.offset(log)
                shown = min(bottom, y + log.winfo_height()) - max(top, y)
                self.assertGreaterEqual(shown, min(100, log.winfo_height()))
                self.doCleanups()

    def test_a_first_result_that_comes_a_tick_late_is_still_shown(self):
        """N2: the real controller hands one result per tick, and the first may miss the first pump."""
        self.scrollable(checks=[[], [CheckResultReady(CheckResult('git', Status.OK, 'ok', ''))], [ChecksDone(True)]])
        self.view.checks.check_button.invoke()
        for _ in range(3):
            self.view.pump()
        rows = self.view.checks.rows.winfo_children()
        self.assertTrue(rows and self.visible(rows[0]))

    def test_the_first_problem_is_shown_when_the_checks_end(self):
        ids = ('apple_silicon', 'macos_version', 'command_line_tools', 'cxx_headers', 'homebrew', 'brew_packages',
               'git', 'build_python', 'metal_toolchain', 'repository', 'input_files', 'input_rules')
        oks = [[CheckResultReady(CheckResult(i, Status.OK, 'ok', ''))] for i in ids]
        block = [[CheckResultReady(CheckResult('disk_space', Status.BLOCK, 'disk_space', '31 GB'))],
                 [ChecksDone(False)]]
        self.scrollable(checks=oks + block)
        self.view.checks.check_button.invoke()
        for _ in range(len(oks) + 3):
            self.view.pump()
        self.assertIsNotNone(self.view.checks.first_problem)
        self.assertTrue(self.visible(self.view.checks.first_problem))

if __name__ == '__main__':
    unittest.main()
