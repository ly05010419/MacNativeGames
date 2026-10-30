"""The button icon plan (games/totk/runtime/keyboard_icons.h): what the settings panel's Apply button icons has
make_button_icons.py --style plan draw, from the keyboard layout in force. The default layout's plan is the keyboard
style's own table, so the shipped keyboard icons and a plan of the default layout are the same drawing."""
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOTK = ROOT / 'games/totk'

# Reads a keyboard.txt from standard input (empty: the default layout) and prints its plan; argv[1] is the buffer size.
PROBE = r'''
#include <stdlib.h>
#include "keyboard_icons.h"
int main(int argc, char **argv) {
    static char text[8192], json[16384];
    size_t n = fread(text, 1, sizeof text - 1, stdin); text[n] = 0;
    KeyMap m; keymap_default(&m);
    const char *error = NULL;
    if (n && keymap_parse(text, &m, &error)) { fprintf(stderr, "bad layout: %s\n", error); return 2; }
    size_t size = argc > 1 ? (size_t)atoi(argv[1]) : sizeof json;
    int written = keyboard_icons_plan(&m, json, size);
    printf("%d\n%s\n", written, written < 0 ? "" : json);
    return 0;
}
'''


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class KeyboardIconsPlanTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        source, cls.probe = Path(cls.tmp.name) / 'plan.c', Path(cls.tmp.name) / 'plan'
        source.write_text(PROBE)
        build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', f'-I{TOTK}/runtime', str(source),
                                '-o', str(cls.probe)], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stderr[-2000:])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_plan(self, layout='', size=None):
        out = subprocess.run([str(self.probe)] + ([str(size)] if size else []), input=layout, capture_output=True,
                             text=True, timeout=30, check=True).stdout.splitlines()
        return int(out[0]), (json.loads(out[1]) if int(out[0]) >= 0 else None)

    def test_default_plan_matches_keyboard_style(self):
        """Every glyph the keyboard style draws, with its shape and labels, and no other glyph."""
        source = (TOTK / 'scripts/make_button_icons.py').read_text()
        start = source.index("0xE040: single('E')")
        body = source[start:source.index('\n    }', start)]
        expected = {}
        for code, shape, args in re.findall(r"0x(E0[0-9A-F]{2}): (single|wide|pair|cluster)\(([^)]*)\)", body):
            expected[code] = [shape] + re.findall(r"'([^']*)'", args)
        written, plan = self.run_plan()
        self.assertGreater(written, 0)
        self.assertEqual(plan['version'], 1)
        self.assertEqual(plan['glyphs'], expected)

    def test_rebound_a_draws_new_key(self):
        _, plan = self.run_plan('a = k\n')
        self.assertEqual(plan['glyphs']['E040'], ['single', 'K'])

    def test_unbound_draws_none(self):
        _, plan = self.run_plan('a = none\n')
        self.assertEqual(plan['glyphs']['E040'], ['none'])

    def test_camera_glyphs_follow_camera_actions(self):
        _, plan = self.run_plan('camera_up = i\n')
        self.assertEqual(plan['glyphs']['E053'], ['single', 'I'])
        self.assertEqual(plan['glyphs']['E05F'], ['cluster', 'I', '←', '↓', '→'])

    def test_mouse_drawn_without_mouse_mode(self):
        _, plan = self.run_plan()
        self.assertEqual(plan['glyphs']['E043'], ['wide', 'LMB'])

    def test_long_names_are_shortened(self):
        _, plan = self.run_plan('b = backspace\nx = right_shift\n')
        self.assertEqual(plan['glyphs']['E041'], ['wide', 'Bksp'])
        self.assertEqual(plan['glyphs']['E042'], ['wide', 'RShift'])

    def test_buffer_too_small(self):
        written, _ = self.run_plan(size=16)
        self.assertEqual(written, -1)


if __name__ == '__main__':
    unittest.main()
