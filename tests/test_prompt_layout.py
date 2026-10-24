"""scripts/prompt_layout.py (which controller button a prompt must show) against the C layout parser (runtime/controller_map.h)."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
import prompt_layout as pl      # noqa: E402

OWNER = """# Custom face buttons
north = a     # Xbox Y (top): interact
south = x     # Xbox A (bottom): jump
west  = y     # Xbox X (left): attack
east  = b     # Xbox B (right): sprint
"""
CORPUS = {
    'default': '',
    'owner': OWNER,
    'comments and case': '  SOUTH\t=  ZR  \r\n\r\n# note\nmenu = NONE # off\n',
    'modes': 'south = x tap\nl3 = b TOGGLE\nl2 = zr aim r2\nr2 = zl aim touchpad\nmouse_middle = zr aim mouse5\n',
    'shoulders': 'l1 = zl\nl2 = l\nview = plus\nmenu = minus\nl3 = rstick\n',
    'bad: unknown button': 'east = a\nsout = b\n',
    'bad: unknown Switch button': 'south = c\n',
    'bad: no equals': '\n# note\nsouth b\n',
    'bad: two modes': 'south = b tap extra\n',
    'bad: none with mode': 'menu = none tap\n',
    'bad: aim by itself': 'l2 = zr aim l2\n',
    'bad: aim without fire': 'l2 = zr aim\n',
}


def dump_c(binary, text, face):
    with tempfile.NamedTemporaryFile('w', suffix='.txt') as f:
        f.write(text); f.flush()
        out = subprocess.run([str(binary), f.name, face], capture_output=True, text=True, timeout=30)
    assert out.returncode == 0, out.stderr
    return [line.split() for line in out.stdout.splitlines()]


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class PromptLayoutMatchesC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name)/'controls_dump'
        build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', str(ROOT/'tests/controls_dump.c'),
                                '-o', str(cls.binary)], capture_output=True, text=True)
        if build.returncode:
            raise unittest.SkipTest(f'clang could not build the dump: {build.stderr[-300:]}')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def python_dump(self, text, face):
        mapping, modes, _ = pl.parse_controls(text)
        rows = [['phys', str(i), pl.PHYSICAL[i], str(mapping[i]),
                 str(pl.MODES.index(modes[i][0])), str(modes[i][1] if modes[i][1] is not None else -1)] for i in range(len(pl.PHYSICAL))]
        masks = pl.pad_switch_masks(text, face)
        return rows + [['pad', pad, str(masks[pad])] for pad in 'abxy']

    def test_every_case_agrees_in_both_face_modes(self):
        for name, text in CORPUS.items():
            for face in pl.FACE_MODES:
                with self.subTest(case=name, face=face):
                    c = dump_c(self.binary, text, face)
                    if c[0][0] == 'error':
                        with self.assertRaises(pl.LayoutError) as caught:
                            pl.parse_controls(text)
                        self.assertEqual(int(c[0][1]), caught.exception.line)
                    else:
                        self.assertEqual(c, self.python_dump(text, face))

    def test_the_harness_sees_a_difference(self):   # negative control: a different file or face mode must not compare equal
        self.assertNotEqual(dump_c(self.binary, OWNER, 'position'), dump_c(self.binary, '', 'position'))
        self.assertNotEqual(dump_c(self.binary, OWNER, 'position'), dump_c(self.binary, OWNER, 'label'))
        self.assertNotEqual(self.python_dump(OWNER, 'label'), dump_c(self.binary, OWNER, 'position'))


class PromptControls(unittest.TestCase):
    def test_default_layout_is_the_switch_layout_by_position(self):
        got = pl.prompt_controls('', 'position')
        self.assertEqual((got['a'], got['b'], got['x'], got['y']), ('b', 'a', 'y', 'x'))   # Switch A is the Xbox B (right) button
        self.assertEqual((got['l'], got['r'], got['zl'], got['zr']), ('l1', 'r1', 'l2', 'r2'))
        self.assertEqual((got['plus'], got['minus'], got['lstick'], got['rstick']), ('menu', 'view', 'l3', 'r3'))

    def test_default_layout_by_label_names_the_same_letters(self):
        got = pl.prompt_controls('', 'label')
        self.assertEqual((got['a'], got['b'], got['x'], got['y']), ('a', 'b', 'x', 'y'))   # Xbox A is Switch A

    def test_the_owner_layout_in_each_face_mode(self):
        # controls.txt names positions: by position north is the Xbox Y button, by label it is the Xbox X button.
        by_position = pl.prompt_controls(OWNER, 'position')
        self.assertEqual((by_position['a'], by_position['x'], by_position['y'], by_position['b']), ('y', 'a', 'x', 'b'))
        by_label = pl.prompt_controls(OWNER, 'label')
        self.assertEqual((by_label['a'], by_label['x'], by_label['y'], by_label['b']), ('x', 'b', 'y', 'a'))

    def test_a_written_line_beats_a_default(self):
        got = pl.prompt_controls('north = a\n', 'position')   # east keeps its default A too; the file's line decides the prompt
        self.assertEqual(got['a'], 'y')

    def test_a_button_nothing_drives_keeps_its_switch_icon(self):
        got = pl.prompt_controls('menu = none\n', 'position')
        self.assertIsNone(got['plus'])

    def test_mouse_and_touchpad_are_not_named(self):
        got = pl.prompt_controls('touchpad = minus\nmouse_left = a\n', 'position')
        self.assertEqual((got['a'], got['minus']), ('b', 'view'))

    def test_shoulders_follow_the_file(self):
        got = pl.prompt_controls('l1 = zl\nl2 = l\n', 'position')
        self.assertEqual((got['zl'], got['l']), ('l1', 'l2'))

    def test_a_bad_file_names_its_line(self):
        with self.assertRaises(pl.LayoutError) as caught:
            pl.prompt_controls('east = a\nsout = b\n', 'position')
        self.assertEqual(caught.exception.line, 2)

    def test_describe_shows_the_result(self):
        lines = pl.describe(OWNER, 'label')
        self.assertIn('Switch A <- Xbox X', lines)
        self.assertIn('Switch Y <- Xbox Y', lines)
        lines = pl.describe(OWNER, 'label', 'switch')
        self.assertIn('Switch A <- Switch X', lines)
        self.assertIn('Switch + <- Switch +', lines)


if __name__ == '__main__':
    unittest.main()
