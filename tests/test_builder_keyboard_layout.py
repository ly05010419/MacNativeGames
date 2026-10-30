"""tools/builder/keyboard_layout.py (S2.1): the key names and the default table against runtime/keyboard_map.h itself,
the layout file's rules (as its header comment states them), and the Tk keysym to input mapping the key test uses."""
import re
import tempfile
import unittest
from pathlib import Path

from tools.builder import keyboard_layout as kl

RUNTIME = Path(__file__).resolve().parents[1] / 'games/totk/runtime/keyboard_map.h'


def header_inputs(text):
    """The runtime's input table: code -> the name keyboard.txt spells."""
    body = text.split('keymap_inputs(int *count)', 1)[1].split('};', 1)[0]
    return {int(code): name for code, name in re.findall(r'\{(\d+), "([^"]+)", "', body)}


def header_defaults(text, inputs):
    """The runtime's keymap_default: action name -> the input names, 'none' slots removed."""
    body = text.split('keymap_default(KeyMap *m)', 1)[1].split('#undef KEYMAP_SET', 1)[0]
    found = {}
    for action, first, second in re.findall(r'KEYMAP_SET\((KEYMAP_\w+),\s*([^,]+),\s*([^)]+)\);', body):
        name = action[len('KEYMAP_'):].lower()
        if name in ('dleft', 'dup', 'dright', 'ddown'):     # the key's name, as the runtime spells it
            name = name[1:]
        found[name] = tuple(
            input_name for input_name in (input_of(first, inputs), input_of(second, inputs))
            if input_name is not None)
    return found


def input_of(token, inputs):
    token = token.strip()
    if token == 'KEYMAP_NONE':
        return None
    mouse = re.match(r'KEYMAP_MOUSE_FIRST\s*\+\s*(\d+)$', token)
    if mouse:
        return inputs[128 + int(mouse.group(1))]
    return inputs[int(token, 0)]


class Header(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = RUNTIME.read_text()
        cls.inputs = header_inputs(cls.text)

    def test_every_input_the_runtime_names_is_known_with_the_same_name(self):
        self.assertEqual(set(kl.INPUTS), set(self.inputs.values()))

    def test_the_default_table_is_the_runtime_default_table(self):
        self.assertEqual(kl.DEFAULT, header_defaults(self.text, self.inputs))

    def test_the_actions_and_their_titles_cover_the_runtime_actions(self):
        names = re.findall(r'"([a-z_]+)"', self.text.split('keymap_action_name', 1)[1].split('};', 1)[0])
        self.assertEqual(list(kl.ACTIONS), names)
        self.assertEqual(set(kl.TITLES), set(kl.ACTIONS))


class Layouts(unittest.TestCase):
    def test_a_missing_file_is_the_runtime_default(self):
        layout = kl.read_layout(Path(tempfile.gettempdir()) / 'no-such-keyboard.txt')
        self.assertEqual(layout.bindings, dict(kl.DEFAULT))
        self.assertEqual(layout.problems, ())

    def test_a_line_replaces_one_action_and_takes_its_input_from_the_others(self):
        layout = kl.parse_layout('b = space\n# a comment\n\nmove_up = arrow_up\n', 'keyboard.txt')
        self.assertEqual(layout.bindings['b'], ('space',))
        self.assertEqual(layout.bindings['a'], kl.DEFAULT['a'])
        self.assertEqual(layout.bindings['x'], ())                 # space left X for B
        self.assertEqual(layout.bindings['move_up'], ('arrow_up',))

    def test_none_leaves_an_action_without_input(self):
        layout = kl.parse_layout('a = none\n', 'keyboard.txt')
        self.assertEqual(layout.bindings['a'], ())

    def test_a_later_line_wins_a_shared_input(self):
        layout = kl.parse_layout('x = f\nl = f\n', 'keyboard.txt')
        self.assertEqual(layout.bindings['x'], ())
        self.assertEqual(layout.bindings['l'], ('f',))

    def test_names_are_case_insensitive(self):
        layout = kl.parse_layout('A = SHIFT C\n', 'keyboard.txt')
        self.assertEqual(layout.bindings['a'], ('shift', 'c'))

    def test_a_bad_line_falls_back_to_the_whole_default(self):
        for text in ('a = nosuchkey\n', 'bogus = a\n', 'a = b c d\n', 'a\n', 'a = shift none\n'):
            layout = kl.parse_layout(text, 'keyboard.txt')
            self.assertEqual(layout.bindings, dict(kl.DEFAULT), text)
            self.assertTrue(layout.problems, text)


class Names(unittest.TestCase):
    def test_a_key_a_person_presses_becomes_the_input_name(self):
        self.assertEqual(kl.key_name('a'), 'a')
        self.assertEqual(kl.key_name('Return'), 'return')
        self.assertEqual(kl.key_name('Shift_L'), 'shift')
        self.assertEqual(kl.key_name('Right'), 'arrow_right')
        self.assertEqual(kl.key_name('F5'), 'f5')
        self.assertEqual(kl.key_name('KP_Multiply'), 'keypad_multiply')
        self.assertIsNone(kl.key_name('Command_L'))               # the system's key, never bound

    def test_shifted_punctuation_is_the_number_key_underneath(self):
        self.assertEqual(kl.key_name('exclam'), '1')
        self.assertEqual(kl.key_name('plus'), 'equal')
        self.assertEqual(kl.key_name('asterisk'), '8')

    def test_labels_are_what_the_window_shows(self):
        self.assertEqual(kl.label('Return'), 'Return')
        self.assertEqual(kl.label('exclam'), '1')
        self.assertEqual(kl.label('Prior'), 'Page Up')
        self.assertEqual(kl.label('a'), 'A')

    def test_modifiers_come_from_the_state_bits_and_the_held_keys(self):
        self.assertEqual(kl.modifiers(0x1, set()), ('Shift',))
        self.assertEqual(kl.modifiers(0x1 | 0x4, set()), ('Shift', 'Control'))
        self.assertEqual(kl.modifiers(0, {'Option_L'}), ('Option',))
        self.assertEqual(kl.modifiers(0, {'Shift_L'}), ('Shift',))
        self.assertTrue(kl.is_modifier('Command_L'))
        self.assertFalse(kl.is_modifier('a'))

    def test_owner_names_the_default_actions_and_nothing_else(self):
        layout = kl.default_layout()
        self.assertEqual(kl.owner(layout, 'e'), ('a',))
        self.assertEqual(kl.owner(layout, '2'), ('left',))
        self.assertEqual(kl.owner(layout, 'right_shift'), ('b',))  # the right key mirrors the left one
        self.assertEqual(kl.owner(layout, 'z'), ())

    def test_a_layout_the_runtime_cannot_parse_is_reported_not_raised(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'keyboard.txt'
            path.write_text('a = nosuchkey\n')
            layout = kl.read_layout(path)
        self.assertEqual(layout.bindings, dict(kl.DEFAULT))
        self.assertTrue(layout.problems)


if __name__ == '__main__':
    unittest.main()
