"""make_button_icons.py --style plan: the glyphs a plan file names (runtime/keyboard_icons.h), drawn with the keyboard
style's shapes, without game data."""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
sys.path.insert(0, str(ROOT/'tests'))
READY = all(importlib.util.find_spec(m) is not None for m in ('compression.zstd', 'fontTools'))


class Glyphs:
    """Stands in for make_button_icons.Glyphs (the text fonts come from the game): a letter cut out of a key is a box."""
    def hole(self, pen, label, cx, cy, cap, width):
        import make_button_icons
        make_button_icons.polygon(make_button_icons.ReverseContourPen(pen),
                                  [(cx - 10, cy - 10), (cx - 10, cy + 10), (cx + 10, cy + 10), (cx + 10, cy - 10)])


def default_plan():
    from test_keyboard_icons import KeyboardIconsPlanTest
    KeyboardIconsPlanTest.setUpClass()
    try:
        return KeyboardIconsPlanTest.run_plan(KeyboardIconsPlanTest, '')[1]
    finally:
        KeyboardIconsPlanTest.tearDownClass()


@unittest.skipUnless(READY, 'needs Python 3.14 and fontTools')
class PlanStyle(unittest.TestCase):
    def setUp(self):
        import make_button_icons
        self.icons = make_button_icons

    def outline(self, draw):
        from fontTools.pens.recordingPen import RecordingPen
        pen = RecordingPen(); draw(pen)
        return pen.value

    def test_default_plan_draws_the_keyboard_style(self):
        """A plan of the default layout: the same glyphs as the keyboard style, each with its advance and outline."""
        g = Glyphs()
        keyboard, plan = self.icons.keyboard_style(g), self.icons.plan_style(g, default_plan())
        self.assertEqual(sorted(plan), sorted(keyboard))
        for cp in keyboard:
            with self.subTest(glyph=hex(cp)):
                self.assertEqual(plan[cp][0], keyboard[cp][0])
                self.assertEqual(self.outline(plan[cp][1]), self.outline(keyboard[cp][1]))

    def test_changed_shape_takes_that_shapes_advance(self):
        plan = {'version': 1, 'glyphs': {'E040': ['wide', 'Bksp'], 'E041': ['single', 'K'], 'E042': ['none']}}
        icons = self.icons.plan_style(Glyphs(), plan)
        self.assertEqual(icons[0xE040][0], 900)     # E was a single key
        self.assertEqual(icons[0xE041][0], 640)     # Shift was a wide one
        self.assertEqual(icons[0xE042][0], 640)
        self.assertTrue(self.outline(icons[0xE042][1]))

    def test_unknown_shape_rejected(self):
        with self.assertRaisesRegex(ValueError, 'E040'):
            self.icons.plan_style(Glyphs(), {'version': 1, 'glyphs': {'E040': ['oval', 'E']}})

    def test_plan_style_requires_plan(self):
        import io
        err = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(sys, 'argv', ['make_button_icons.py', tmp, '--romfs', tmp, '--style', 'plan']), \
                mock.patch('sys.stderr', err):
            with self.assertRaises(SystemExit):
                self.icons.main()
        self.assertIn('--style plan needs --plan', err.getvalue())

    def test_progress_line(self):
        import io
        out = io.StringIO()
        with mock.patch('sys.stdout', out):
            self.icons.report_progress(True, 2, 6)
            self.icons.report_progress(False, 3, 6)
        self.assertEqual(out.getvalue(), 'progress 2/6\n')

    def test_plan_file_written_by_runtime_is_valid_json(self):
        plan = default_plan()
        self.assertEqual(json.loads(json.dumps(plan)), plan)


if __name__ == '__main__':
    unittest.main()
