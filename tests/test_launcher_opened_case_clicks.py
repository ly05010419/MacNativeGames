"""The opened case's actions take their clicks. Opened as a book, the case's front cover turns half a circle about the
spine and is drawn over the left half, but laid out it stays where it was: over the tray on the right, above the game's
actions. With hit testing on, it took every click there: in MNG Launcher 1.1.0, 极速更新, 启动游戏 and 添加这个游戏 did
nothing, while a click on the ground beside the case still closed it (2026-11-20)."""
from pathlib import Path
import unittest

CASE = Path(__file__).resolve().parents[1] / 'tools/builder_app/Sources/MNGBuilder/CaseDetail.swift'


class OpenedCaseClicks(unittest.TestCase):
    def test_the_turned_cover_lets_clicks_through_to_the_actions(self):
        source = CASE.read_text()
        book = source[source.index('private func book('):]
        book = book[:book.index('\n    }\n')]
        turn = book.index('.rotation3DEffect(.degrees(angle)')   # the cover, turned about the spine
        after = book[turn:book.index('.offset(x: hinge, y: top)', turn)]
        self.assertIn('.allowsHitTesting(false)', after)
        # the tray, drawn under it, still takes clicks: the actions are on it
        tray = book[book.index('tray(width: openW'):book.index('.offset(x: hinge, y: top)')]
        self.assertNotIn('allowsHitTesting(false)', tray)


if __name__ == '__main__':
    unittest.main()
