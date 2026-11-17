"""Include TMNT regressions in ordinary repository unittest discovery."""
import importlib.util
from pathlib import Path
import sys
import unittest


def load_tests(loader, tests, pattern):
    directory = Path(__file__).resolve().parents[1] / 'games/tmnt/tests'
    suite = unittest.TestSuite()
    for path in sorted(directory.glob('test_*.py')):
        name = '_tmnt_' + path.stem
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
        suite.addTests(loader.loadTestsFromModule(module))
    return suite
