"""MNG Launcher: a view turned in 3D takes clicks where it would stand unturned.

SwiftUI hit testing does not follow rotation3DEffect / projectionEffect. In Launcher 1.1.0 the opened case's cover, turned
-180° about the spine onto the left half, still took every click on the right half, where it stood before the turn, and
no button in the opened case answered (user 2026-11-20: "launcher 所有按键 都失灵"). So every 3D effect in the Launcher's
Swift sources either sits in a chain with .allowsHitTesting(false), or says in a `// hits:` comment just above it why the
place it takes clicks is right (turned 0° whenever it has controls, or inside a view that takes no clicks)."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[1]
SOURCES = [REPO / 'tools/builder_app/Sources', REPO / 'tools/ios_launcher/Sources']
EFFECT = re.compile(r'\.(rotation3DEffect|projectionEffect|transform3DEffect)\(')


def chain_after(lines, at):
    """The modifier chain from line `at` on: its own lines while parentheses are open, then each next line that is a
    further modifier (`.x`) or a comment."""
    depth, out, i = 0, [], at
    while i < len(lines):
        line = lines[i]
        if i > at and depth == 0 and not line.strip().startswith(('.', '//')):
            break
        out.append(line)
        depth += line.count('(') - line.count(')')
        i += 1
    return out


def explained(lines, at):
    """A `// hits:` comment among the comment lines and 3D effects right above line `at`."""
    i = at - 1
    while i >= 0 and (lines[i].strip().startswith('//') or EFFECT.search(lines[i])):
        if lines[i].strip().startswith('// hits:'):
            return True
        i -= 1
    return False


def unchecked(text):
    lines = text.splitlines()
    return [n + 1 for n, line in enumerate(lines) if EFFECT.search(line) and not explained(lines, n)
            and not any('.allowsHitTesting(false)' in l for l in chain_after(lines, n))]


class TurnedViewsTakeNoStrayClicks(unittest.TestCase):
    def test_every_3d_effect_takes_no_clicks_or_says_why(self):
        found = 0
        for root in SOURCES:
            for swift in sorted(root.rglob('*.swift')):
                text = swift.read_text()
                found += len(EFFECT.findall(text))
                with self.subTest(str(swift.relative_to(REPO))):
                    self.assertEqual(unchecked(text), [], 'a 3D-turned view takes clicks where it stood unturned: add '
                                     '.allowsHitTesting(false) to its chain, or a "// hits:" comment above saying why not')
        self.assertGreater(found, 0)

    def test_the_opened_cases_cover_takes_no_clicks(self):
        """The 1.1.0 bug itself: the cover turned over onto the left half takes no clicks (no `hits:` excuse)."""
        lines = (REPO / 'tools/builder_app/Sources/MNGBuilder/CaseDetail.swift').read_text().splitlines()
        (at,) = [n for n, l in enumerate(lines) if '.rotation3DEffect(.degrees(angle)' in l]
        self.assertTrue(any('.allowsHitTesting(false)' in l for l in chain_after(lines, at)))
        # and not too much: the tray under it, which holds the game's actions, still takes clicks (PR #2317's check)
        (tray,) = [n for n, l in enumerate(lines) if l.strip().startswith('tray(width: openW')]
        self.assertFalse(any('allowsHitTesting(false)' in l for l in chain_after(lines, tray)))

    def test_the_check_sees_a_turned_view_without_either(self):
        bare = 'Group { cover }\n    .rotation3DEffect(.degrees(-180), axis: (x: 0, y: 1, z: 0),\n        perspective: 0.45)\n' \
               '    .offset(x: 1)\n'
        self.assertEqual(unchecked(bare), [2])
        self.assertEqual(unchecked(bare + '    .allowsHitTesting(false)\n'), [])
        self.assertEqual(unchecked(bare.replace('    .rotation', '    // hits: turned 0° with controls\n    .rotation')), [])
        self.assertEqual(unchecked(bare + 'Other()\n    .allowsHitTesting(false)\n'), [2])   # another view's, not its own


if __name__ == '__main__':
    unittest.main()
