"""The progress box's 日志 / Log opens from its word too, not only from the small triangle before it. On macOS a
DisclosureGroup's label is not clickable: a player clicking 日志 saw nothing happen and thought the log could not be
opened (2026-11-18, during an Ocarina of Time fast update)."""
from pathlib import Path
import unittest

ACTION = Path(__file__).resolve().parents[1] / 'tools/builder_app/Sources/MNGBuilder/ActionSection.swift'


class LogToggle(unittest.TestCase):
    def test_the_word_log_opens_and_closes_it(self):
        source = ACTION.read_text()
        start = source.index('DisclosureGroup(')
        group = source[start:source.index('\n            }', start)]   # the group with its label, to the end of its if
        self.assertIn('isExpanded: $showLog', group)
        self.assertIn('Text(T.log)', group)                       # the label is the word itself ...
        self.assertIn('.onTapGesture', group)                     # ... and a click on it toggles the log
        self.assertIn('showLog.toggle()', group)
        self.assertIn('.contentShape(Rectangle())', group)        # the whole word, not only its glyphs


if __name__ == '__main__':
    unittest.main()
