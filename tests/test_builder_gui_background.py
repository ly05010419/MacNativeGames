"""tools/builder/gui_background.py (S4.3; S4.4 layout): the picture zone on the right of the form and its controls in
Details, on a real withdrawn Tk root (builder_tk_base.py), against the scripted FakeController. The picture is a PNG
Tk writes itself in a temporary folder; the model (sips) is tested apart."""
from pathlib import Path
import shutil
import tempfile
import unittest

from builder_tk_base import Base, tk
from tools.builder import background
from tools.builder.background import Background
from tools.builder.session import BackgroundChanged, ProcessError
from tools.builder.strings import text

if tk is not None:
    from tools.builder.gui import IMAGE_TYPES
    from tools.builder.gui_background import crop


@unittest.skipIf(tk is None, 'this Python has no Tk')
class Crop(unittest.TestCase):
    def test_the_zone_shows_the_middle_height_and_the_focus_across(self):
        self.assertEqual(crop(1600, 1000, 400, 760, 'center'), (600, 120, 1000, 880))
        self.assertEqual(crop(1600, 1000, 400, 760, 'left'), (200, 120, 600, 880))
        self.assertEqual(crop(1600, 1000, 400, 760, 'right'), (1000, 120, 1400, 880))

    def test_it_stays_inside_the_picture(self):
        self.assertEqual(crop(1600, 1000, 1200, 760, 'right'), (400, 120, 1600, 880))
        self.assertEqual(crop(300, 200, 400, 760, 'center'), (0, 0, 300, 200))       # a zone larger than the picture


class Zone(Base):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp)
        self.start()
        self.root.update_idletasks()
        self.root.geometry('1200x800')                  # a withdrawn root lays out on a geometry change
        self.root.update_idletasks()
        self.panel = self.view.background

    def png(self, name='background-shown.png', width=1600, height=1000):
        path = self.tmp / name
        image = tk.PhotoImage(master=self.root, width=width, height=height)
        image.put('#ff0000', to=(0, 0, width, height))
        image.write(str(path), format='png')
        return path

    def deliver(self, event):
        self.ctl.events.append(event)
        self.view.pump()

    def shown(self, focus='center'):
        self.deliver(BackgroundChanged(Background(self.tmp / 'background.jpg', self.png(), focus=focus)))
        self.root.update_idletasks()
        self.panel.draw()

    def test_the_saved_one_is_asked_for_and_the_zone_is_a_plain_panel_meanwhile(self):
        self.assertEqual(self.ctl.background_calls, [('saved', self.panel.screen())])
        self.assertEqual((self.panel.canvas.grid_info()['column'], self.panel.canvas.find_all()), (1, ()))
        self.assertEqual(self.panel.remove_button.cget('state'), 'disabled')

    def test_a_picture_fills_the_zone_without_text(self):
        self.shown()
        canvas = self.panel.canvas
        self.assertEqual([canvas.type(item) for item in canvas.find_all()], ['image'])
        self.assertEqual((self.panel.shown.width(), self.panel.shown.height()),
                         (min(1600, canvas.winfo_width()), min(1000, canvas.winfo_height())))
        self.assertEqual(self.root.tk.call(self.panel.shown, 'get', 0, 0, '-withalpha'), (255, 0, 0, 255))
        self.assertEqual({b.cget('state') for b in self.panel.focus_buttons}, {'normal'})

    def test_the_focus_is_shown_and_a_change_goes_to_the_controller(self):
        self.shown('right')
        self.assertEqual(self.panel.focus.get(), 'right')
        self.panel.focus_buttons[0].invoke()
        self.assertEqual(self.ctl.background_calls[-1], ('focus', self.panel.screen(), 'left'))

    def test_choose_and_remove(self):
        self.view.pump()                                    # S4.4b: the 'saved' asked at open is pending until a tick
        self.answer = '/mine.heic'
        self.panel.choose_button.invoke()
        self.assertEqual((self.picks[-1], self.ctl.background_calls[-1]),
                         (('image', None), ('save', '/mine.heic', self.panel.screen())))
        self.assertEqual(self.panel.note.cget('text'), text('gui.background.saving'))
        self.shown()
        self.assertEqual((self.panel.note.cget('text'), self.panel.remove_button.cget('state')), ('', 'normal'))
        self.panel.remove_button.invoke()
        self.assertEqual(self.ctl.background_calls[-1], ('remove',))
        self.deliver(BackgroundChanged(Background(None)))
        self.assertEqual((self.panel.canvas.find_all(), self.panel.picture), ((), None))
        self.assertEqual(self.panel.remove_button.cget('state'), 'disabled')

    def test_a_cancelled_dialog_does_nothing(self):
        self.panel.choose_button.invoke()
        self.assertEqual(self.ctl.background_calls, [('saved', self.panel.screen())])

    def test_a_refusal_or_an_error_says_why_and_keeps_what_is_shown(self):
        self.shown()
        old = Background(self.tmp / 'background.jpg', self.png('again.png'))
        for why in ('kind', 'too_big', 'unreadable'):
            self.deliver(BackgroundChanged(Background(old.path, old.shown, why)))
            self.assertEqual((self.panel.note.cget('text'), self.panel.picture is not None),
                             (text(f'gui.background.{why}'), True))
        self.deliver(ProcessError('background', "OSError('disk')"))
        self.assertEqual(self.panel.note.cget('text'), text('gui.background.failed', error="OSError('disk')"))

    def test_a_picture_that_does_not_load_says_so_and_can_still_be_removed(self):
        broken = self.tmp / 'background-shown.png'
        broken.write_text('damaged')
        self.deliver(BackgroundChanged(Background(self.tmp / 'background.jpg', broken)))
        self.assertEqual((self.panel.picture, self.panel.note.cget('text')),
                         (None, text('gui.background.unloadable', path=broken)))
        self.assertEqual(self.panel.remove_button.cget('state'), 'normal')

    def test_the_controls_wait_while_a_change_runs(self):
        self.shown()
        self.ctl.background_busy = True
        self.view.pump()
        buttons = [self.panel.choose_button, self.panel.remove_button] + self.panel.focus_buttons
        self.assertEqual({b.cget('state') for b in buttons}, {'disabled'})

    def test_one_click_one_command(self):                                      # S4.4b, B3
        self.shown('center')
        self.panel.remove_button.invoke()
        self.panel.remove_button.invoke()                             # before the pump
        self.assertEqual(([c for c in self.ctl.background_calls if c[0] == 'remove'], self.callback_errors),
                         ([('remove',)], []))

    def test_a_focus_click_while_busy_puts_the_choice_back(self):              # S4.4b, F6
        self.shown('center')
        self.panel.focus_buttons[2].invoke()                          # right: sent
        self.panel.focus_buttons[0].invoke()                          # left: refused before the pump
        self.assertEqual([c for c in self.ctl.background_calls if c[0] == 'focus'],
                         [('focus', self.panel.screen(), 'right')])
        self.assertEqual((self.panel.focus.get(), self.callback_errors), ('right', []))

    def test_the_controls_live_in_details(self):
        self.assertIs(self.panel.frame.master, self.view.updates.details)

    def test_the_dialog_offers_what_the_model_takes(self):
        self.assertEqual(IMAGE_TYPES, background.SUFFIXES)


if __name__ == '__main__':
    unittest.main()
