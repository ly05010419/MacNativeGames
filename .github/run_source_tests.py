"""Run the slow AppKit settings probe separately, preserving normal unittest discovery."""
import argparse
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def select(suite, group):
    selected = unittest.TestSuite()
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            selected.addTest(select(test, group))
        elif (type(test).__module__ == 'test_settings_panel') == (group == 'settings'):
            selected.addTest(test)
    return selected


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('group', choices=('core', 'settings'))
    args = parser.parse_args()
    suite = select(unittest.defaultTestLoader.discover(str(ROOT/'tests')), args.group)
    if not suite.countTestCases():
        sys.exit('No tests selected')
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(not result.wasSuccessful())
