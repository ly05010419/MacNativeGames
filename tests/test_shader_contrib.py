"""The player's contribution file (totk_shader_contrib.h): written by the runtime, merged by scripts/shader_list.py."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'
sys.path.insert(0, str(TOTK / 'scripts'))
from shader_list import ShaderList, canonical  # noqa: E402


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class ContributionFileTests(unittest.TestCase):
    def test_probe_writes_rows_the_shared_list_merges(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp); probe = tmp / 'probe'; contrib = tmp / 'shader-list-contribution.txt'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    f'-I{TOTK}/third_party/d2r_gfx/gfx/nvn', str(TOTK / 'tests/shader_contrib/probe.m'),
                                    '-framework', 'Foundation', '-o', str(probe)], capture_output=True, text=True)
            if build.returncode:
                self.skipTest(f'clang could not build the probe: {build.stderr[-300:]}')
            run = subprocess.run([str(probe), str(contrib)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(run.stdout)['status'], 'pass')
            rows = [line.split('\t') for line in contrib.read_text().splitlines() if line.startswith('V\t')]
            self.assertTrue(rows and all(canonical(json.loads(deps)) == deps for _, _, deps in rows), 'deps as canonical() writes them')
            shared = ShaderList()
            shared.add_rows(contrib, 'm')
            self.assertEqual(len(shared.variants), 1000)
            self.assertFalse(shared.warm)


if __name__ == '__main__':
    unittest.main()


from collect_shader_issues import attachment_urls, contribution_rows  # noqa: E402


class IssueCollectionTests(unittest.TestCase):
    KEY_A, KEY_B = 'a' * 64, 'b' * 64

    def test_attachment_links_in_order_without_repeats(self):
        body = ('### shader-list-contribution.txt\n\n[shader-list-contribution.txt](https://github.com/user-attachments/files/123/'
                'shader-list-contribution.txt)\nagain: https://github.com/user-attachments/files/123/shader-list-contribution.txt '
                'old style: https://github.com/someone/MacNativeGames/files/456/contrib.txt).\nnot one: https://example.com/files/1/x.txt')
        self.assertEqual(attachment_urls(body), ['https://github.com/user-attachments/files/123/shader-list-contribution.txt',
                                                 'https://github.com/someone/MacNativeGames/files/456/contrib.txt'])
        self.assertEqual(attachment_urls(None), [])

    def test_only_shader_rows_are_taken(self):
        text = '\n'.join([
            '# header', '',
            f'V\t{self.KEY_A}\t{{"tex":{{"9":"2d","10":"3d"}}}}',     # canonicalised
            f'V\t{self.KEY_A}\t{{"tex":{{"10":"3d","9":"2d"}}}}',     # the same row
            f'V\t{self.KEY_B}\t{{}}',
            f'S\t{self.KEY_A}-0123456789ab_b\t0\tm',                  # warm rows are not taken from contributions
            f'L\t{self.KEY_A}\tShader/x.bnsh\t0\t880\t100',            # nor locators
            'V\tnot-a-key\t{}', f'V\t{self.KEY_B}\t{{"local":[1]}}', f'V\t{self.KEY_B}\tnot json'])
        rows, dropped = contribution_rows(text)
        self.assertEqual(rows, {(self.KEY_A, '{"tex":{"10":"3d","9":"2d"}}'), (self.KEY_B, '{}')})
        self.assertEqual(dropped, 5)
