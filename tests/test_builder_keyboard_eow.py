"""MNG Launcher's Controls page for Echoes of Wisdom (2026-11-17): the key test starts from the table EoW's runtime starts from
(games/eow/runtime/keyboard_map.h keymap_default, the fixed layout it had before it had key bindings), not from Tears of the
Kingdom's; every other game keeps the table it had (tools/builder/keyboard_layout.py DEFAULT)."""
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder import keyboard_layout as kb  # noqa: E402
from tools.builder.games import EOW, PAGES  # noqa: E402
from tools.builder_jobs import tab_controls  # noqa: E402

HEADER = ROOT / 'games/eow/runtime/keyboard_map.h'
ACTION = {'KEYMAP_A': 'a', 'KEYMAP_B': 'b', 'KEYMAP_X': 'x', 'KEYMAP_Y': 'y', 'KEYMAP_LSTICK': 'lstick',
          'KEYMAP_RSTICK': 'rstick', 'KEYMAP_L': 'l', 'KEYMAP_R': 'r', 'KEYMAP_ZL': 'zl', 'KEYMAP_ZR': 'zr',
          'KEYMAP_PLUS': 'plus', 'KEYMAP_MINUS': 'minus', 'KEYMAP_DLEFT': 'left', 'KEYMAP_DUP': 'up',
          'KEYMAP_DRIGHT': 'right', 'KEYMAP_DDOWN': 'down', 'KEYMAP_MOVE_UP': 'move_up', 'KEYMAP_MOVE_DOWN': 'move_down',
          'KEYMAP_MOVE_LEFT': 'move_left', 'KEYMAP_MOVE_RIGHT': 'move_right', 'KEYMAP_CAMERA_UP': 'camera_up',
          'KEYMAP_CAMERA_DOWN': 'camera_down', 'KEYMAP_CAMERA_LEFT': 'camera_left', 'KEYMAP_CAMERA_RIGHT': 'camera_right',
          'KEYMAP_SCREENSHOT': 'screenshot'}


def header_default():
    """keymap_default of games/eow/runtime/keyboard_map.h, in keyboard.txt's names."""
    text = HEADER.read_text()
    names = {int(code): name for code, name in re.findall(r'\{(\d+), "([a-z0-9_]+)", "', text)}
    body = text[text.index('static inline void keymap_default'):text.index('#undef KEYMAP_SET')]
    table = {}
    for action, first, second in re.findall(r'KEYMAP_SET\((KEYMAP_\w+), (\w+), (\w+)\);', body):
        table[ACTION[action]] = tuple(names[int(v)] for v in (first, second) if v != 'KEYMAP_NONE')
    return table


class EchoesKeyboard(unittest.TestCase):
    def test_eow_default_is_its_runtime_table(self):
        self.assertEqual(kb.defaults_for('eow'), header_default())
        self.assertEqual(set(header_default()), set(kb.ACTIONS))

    def test_other_games_keep_their_table(self):
        for game in ('totk', 'botw', 'oot', 'acnh', 'odyssey'):
            self.assertIs(kb.defaults_for(game), kb.DEFAULT)
        self.assertEqual(kb.default_layout().bindings, {a: kb.DEFAULT[a] for a in kb.ACTIONS})

    def test_page_reads_the_eow_table_and_header(self):
        self.assertIn('controls', PAGES['eow'])
        with tempfile.TemporaryDirectory() as tmp:
            ctx = SimpleNamespace(data_dir=Path(tmp) / 'EoW', app=Path(tmp) / 'none.app', spec=EOW, repo=ROOT)
            view = tab_controls.keyboard(ctx)
            actions = {a['id']: a['keys'] for a in view['actions']}
            self.assertEqual(actions['a'], ['J'])
            self.assertEqual(actions['b'], ['K', 'Space'])
            self.assertEqual(view['keys']['38']['actions'], ['A'])        # J, with the button's plain name
            self.assertEqual(view['keys']['35']['actions'], ['Screenshot'])
            (ctx.data_dir).mkdir()
            (ctx.data_dir / 'keyboard.txt').write_text('a = m\n')       # what the settings panel wrote
            actions = {a['id']: a['keys'] for a in tab_controls.keyboard(ctx)['actions']}
            self.assertEqual(actions['a'], ['M'])
            self.assertEqual(actions['b'], ['K', 'Space'])                 # the rest stays EoW's default


if __name__ == '__main__':
    unittest.main()
