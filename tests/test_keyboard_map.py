"""The keyboard and mouse layout (games/totk/runtime/keyboard_map.h): tests/test_keyboard_map.c, and the two places that
restate its default by hand (the controls page's key table, the keyboard button icons), checked against the header."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'

# The default layout as keyboard_map.h makes it: action name -> the labels of its inputs, in slot order.
DUMP = r'''
#include "keyboard_map.h"
int main(void) {
    KeyMap m; keymap_default(&m);
    for (int a = 0; a < KEYMAP_ACTIONS; a++) {
        printf("%s", keymap_action_name(a));
        for (int s = 0; s < KEYMAP_SLOTS; s++) if (m.slot[a][s] != KEYMAP_NONE) printf("\t%s", keymap_input_label(m.slot[a][s]));
        printf("\n");
    }
    return 0;
}
'''


def default_layout(test):
    with tempfile.TemporaryDirectory() as tmp:
        source, probe = Path(tmp) / 'dump.c', Path(tmp) / 'dump'
        source.write_text(DUMP)
        build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{TOTK}/runtime', str(source), '-o', str(probe)],
                               capture_output=True, text=True)
        test.assertEqual(build.returncode, 0, build.stderr[-2000:])
        out = subprocess.run([str(probe)], capture_output=True, text=True, timeout=30, check=True).stdout
    return {fields[0]: fields[1:] for fields in (line.split('\t') for line in out.splitlines())}


def table(path, header):
    """The rows of the markdown table that starts with header, as lists of cells."""
    lines = path.read_text().splitlines()
    start = lines.index(header)
    rows = []
    for line in lines[start + 2:]:
        if not line.startswith('|'):
            break
        rows.append([cell.strip() for cell in line.strip('|').split('|')])
    return rows


# The controls page's Switch column -> the layout's actions (the sticks' four directions are one row each).
SWITCH = {'A': ['a'], 'B': ['b'], 'X': ['x'], 'Y': ['y'], 'ZR': ['zr'], 'ZL': ['zl'], 'L': ['l'], 'R': ['r'],
          'left stick press': ['lstick'], 'right stick press': ['rstick'], '+': ['plus'], '−': ['minus'],
          'left stick': ['move_up', 'move_left', 'move_down', 'move_right'],
          'right stick': ['camera_up', 'camera_left', 'camera_down', 'camera_right'],
          'd-pad': ['left', 'up', 'right', 'down']}
# How the page writes an input -> keyboard_map.h's label.
WRITTEN = {'arrow keys': ['↑', '←', '↓', '→'], 'left button': ['Left click'], 'right button': ['Right click'],
           'wheel click': ['Wheel click'], 'moving the mouse': []}


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class KeyboardMapTests(unittest.TestCase):
    def test_cases(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    str(ROOT / 'tests/test_keyboard_map.c'), '-o', str(probe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS', run.stdout)

    def test_controls_page_shows_the_default(self):
        """docs/controls.md's key table says what keyboard_map.h's default does, and the Chinese page's table says the same."""
        layout = default_layout(self)
        rows = table(ROOT / 'docs/controls.md', '| Action | Switch | Keys | Mouse |')
        self.assertEqual(len(rows), 15)
        covered = set()
        for action, switch, keys, mouse in rows:
            with self.subTest(row=action):
                names = SWITCH[switch]
                covered.update(names)
                written = []
                for part in [p for p in re.split(r',\s*', keys) + [mouse] if p]:
                    written.append(WRITTEN.get(part, part.replace(' / ', ' ').split() if ' ' in part else [part]))
                if len(names) == 4:   # a stick or the d-pad: one input per direction, in the page's order
                    inputs = [label for part in written for label in part]
                    self.assertEqual([layout[n] for n in names], [[i] for i in inputs], f'{action}: {keys}')
                else:
                    labels = sorted(label for part in written for label in part)
                    self.assertEqual(sorted(layout[names[0]]), labels, f'{action}: {keys} / {mouse}')
        self.assertEqual(covered, set(layout), 'every action has a row')
        zh = table(ROOT / 'docs/controls.zh-CN.md', '| 操作 | Switch | 按键 | 鼠标 |')
        switch_zh = {'左摇杆': 'left stick', '右摇杆': 'right stick', '左摇杆按下': 'left stick press', '右摇杆按下': 'right stick press', '十字键': 'd-pad'}
        self.assertEqual([switch_zh.get(r[1], r[1]) for r in zh], [r[1] for r in rows], 'the Chinese table has the same rows')
        normal = lambda keys: keys.replace('、', ', ').replace('空格', 'Space').replace('方向键', 'arrow keys')
        self.assertEqual([normal(r[2]) for r in zh], [r[2] for r in rows], 'and the same keys')

    def test_keyboard_icons_draw_the_default(self):
        """make_button_icons.py --style keyboard draws, for each button prompt, the default layout's first input."""
        layout = default_layout(self)
        source = (TOTK / 'scripts/make_button_icons.py').read_text()
        start = source.index("0xE040: single(")   # the keyboard style's table (the controller styles draw faces)
        glyphs = {int(code, 16): label for code, label in
                  re.findall(r"0x(E0[0-9A-F]{2}): (?:single|wide)\('([^']+)'", source[start:source.index('\n\n', start)])}
        icon = {'Left click': 'LMB', 'Right click': 'RMB', 'Control': 'Ctrl'}
        prompts = {0xE040: 'a', 0xE041: 'b', 0xE042: 'x', 0xE043: 'y', 0xE044: 'l', 0xE045: 'r', 0xE046: 'zl', 0xE047: 'zr',
                   0xE04B: 'plus', 0xE04C: 'minus', 0xE056: 'lstick', 0xE057: 'rstick',
                   0xE060: 'up', 0xE061: 'down', 0xE062: 'left', 0xE063: 'right'}
        for code, action in prompts.items():
            with self.subTest(prompt=hex(code), action=action):
                first = layout[action][0]
                self.assertEqual(glyphs.get(code), icon.get(first, first))


if __name__ == '__main__':
    unittest.main()
