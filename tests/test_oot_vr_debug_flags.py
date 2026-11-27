"""Every diagnostic flag the OOT VR game side knows (games/oot/runtime/oot_vr.h runtime_oot_vr_debug) must be forwarded to it by
the input file's flag handler (games/oot/runtime/input.c). 2026-11-27: "flag vr_stick_drawcall" was added to oot_vr.h only, the
graphics side answered "unknown runtime flag" and a whole test run was lost."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / 'games' / 'oot' / 'runtime'


def debug_names():
    text = (RUNTIME / 'oot_vr.h').read_text(encoding='utf-8')
    start = text.index('void runtime_oot_vr_debug(const char *name, int v) {')
    end = text.index('\n}\n', start)
    return set(re.findall(r'!strcmp\(name, "([a-z0-9_]+)"\)', text[start:end]))


def forwarded_names():
    for line in (RUNTIME / 'input.c').read_text(encoding='utf-8').splitlines():
        if 'runtime_oot_vr_debug(name, v)' in line:
            return set(re.findall(r'!strcmp\(name,"([a-z0-9_]+)"\)', line))
    return set()


class DebugFlagsForwarded(unittest.TestCase):
    def test_the_game_side_knows_some(self):
        self.assertIn('vr_noturn', debug_names())

    def test_every_one_is_forwarded(self):
        missing = sorted(debug_names() - forwarded_names())
        self.assertEqual(missing, [], 'add these to the runtime_oot_vr_debug line in games/oot/runtime/input.c')


if __name__ == '__main__':
    unittest.main()
