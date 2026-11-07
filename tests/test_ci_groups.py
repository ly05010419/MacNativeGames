"""CI groups preserve every discovered case and still report import failures."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location('run_source_tests', Path(__file__).resolve().parents[1]/'.github/run_source_tests.py')
CI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CI)


def cases(suite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from cases(test)
        else:
            yield test


class CIGroups(unittest.TestCase):
    def test_groups_cover_nested_suites_exactly_once_in_order(self):
        tests = [type('Probe', (unittest.TestCase,), {'__module__': name, 'runTest': lambda self: None})()
                 for name in ('test_one', 'test_settings_panel', 'test_two', 'test_settings_panel')]
        suite = unittest.TestSuite([unittest.TestSuite(tests[:2]), unittest.TestSuite(tests[2:])])
        self.assertEqual(list(cases(CI.select(suite, 'core'))), [tests[0], tests[2]])
        self.assertEqual(list(cases(CI.select(suite, 'settings'))), [tests[1], tests[3]])
        self.assertEqual(list(cases(suite)), tests)

    def test_failed_imports_are_kept_in_core(self):
        suite = unittest.TestLoader().loadTestsFromName('_mng_missing_test_module_')
        self.assertEqual(CI.select(suite, 'core').countTestCases(), 1)
        self.assertEqual(CI.select(suite, 'settings').countTestCases(), 0)
        self.assertEqual(list(cases(CI.select(suite, 'core'))), list(cases(suite)))

    def test_settings_helpers_in_other_modules_stay_in_core(self):
        case = type('SettingsHelpers', (unittest.TestCase,), {'__module__': 'test_frame_lock', 'runTest': lambda self: None})()
        suite = unittest.TestSuite([case])
        self.assertEqual(list(cases(CI.select(suite, 'core'))), [case])
        self.assertEqual(CI.select(suite, 'settings').countTestCases(), 0)
