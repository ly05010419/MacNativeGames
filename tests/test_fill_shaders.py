"""The fill tool's translator lookup: the work tree's build first, then the checked-in macOS binary."""
import platform
import stat
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'
sys.path.insert(0, str(TOTK / 'ios/tools'))
import fill_shaders  # noqa: E402


def executable(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#!/bin/sh\n')
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class TranslatorServiceTests(unittest.TestCase):
    def test_the_build_copy_wins_and_the_prebuilt_binary_is_the_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            prebuilt = executable(tree / f'prebuilt/macos-{platform.machine()}/totk_xlate')
            self.assertEqual(fill_shaders.translator_service(tree), prebuilt)
            built = executable(tree / 'build/shader-service/totk_xlate')
            self.assertEqual(fill_shaders.translator_service(tree), built)

    def test_an_empty_tree_names_what_it_looked_for(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp)
            old = fill_shaders.TOTK
            fill_shaders.TOTK = tree
            try:
                with self.assertRaisesRegex(RuntimeError, 'missing shader translator'):
                    fill_shaders.translator_service(tree)
            finally:
                fill_shaders.TOTK = old


class ContributionTests(unittest.TestCase):
    def test_rerunning_writes_each_row_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            contrib = Path(tmp) / 'shader-list-contribution.txt'
            rows = ['V\t' + 'a' * 64 + '-0123456789ab' + '\t{}', 'V\t' + 'b' * 64 + '-0123456789ab' + '\t{}']
            self.assertEqual(fill_shaders.append_contribution(contrib, rows), 2)
            self.assertEqual(fill_shaders.append_contribution(contrib, rows + rows[1:]), 0)
            fill_shaders.append_contribution(contrib, rows + ['V\t' + 'c' * 64 + '-0123456789ab' + '\t{}'])
            lines = contrib.read_text().splitlines()
            self.assertEqual([l for l in lines if l.startswith('V\t')], rows + ['V\t' + 'c' * 64 + '-0123456789ab' + '\t{}'])
            self.assertEqual(sum(1 for l in lines if l.startswith('#')), 3)   # the header, written once


if __name__ == '__main__':
    unittest.main()
