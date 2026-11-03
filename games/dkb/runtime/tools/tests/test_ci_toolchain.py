"""CI builds with the toolchain the project supports: the Xcode that .github/workflows/tests.yml names in DEVELOPER_DIR is the one README tells players to install and the NSP
script names beside dmsh_chain.lock, and every macOS job runs on the GitHub image of that Xcode (xcode-<major>). The build check compares the image's Metal compiler with the
lock itself (own_compilers.py --check-metal); this test keeps the names in step (own_compilers.py's stop message too), so raising the toolchain is one change in all of them. No Xcode or network needed."""
from pathlib import Path
import re
import unittest

REPO = Path(__file__).resolve().parents[3]


class Toolchain(unittest.TestCase):
    def setUp(self):
        self.workflow = (REPO / '.github/workflows/tests.yml').read_text()
        m = re.search(r'DEVELOPER_DIR: /Applications/Xcode_(\d+\.\d+)\.app/Contents/Developer', self.workflow)
        if m is None: self.fail('tests.yml: DEVELOPER_DIR names no /Applications/Xcode_<version>.app')
        self.xcode = m.group(1)

    def test_every_macos_job_runs_on_that_xcodes_image(self):
        runners = re.findall(r'runs-on: (\S+)', self.workflow)
        self.assertTrue(runners, 'tests.yml: no runs-on')
        image = f'xcode-{self.xcode.split(".")[0]}'
        self.assertEqual(sorted({r for r in runners if r.startswith(('macos-', 'xcode-'))}), [image], f'tests.yml: every macOS job runs on {image} (Xcode {self.xcode})')

    def test_readme_names_the_same_xcode(self):
        readme = (REPO / 'README.md').read_text()
        named = re.findall(r'\[Xcode\]\([^)]*\) (\d+\.\d+)', readme)
        self.assertEqual(set(named), {self.xcode}, f'README.md: the Xcode to install is not CI\'s {self.xcode}')
        self.assertEqual(set(re.findall(r'Xcode[ -](\d+\.\d+)', readme)), {self.xcode}, f'README.md: an Xcode it names (the side-by-side one too) is not CI\'s {self.xcode}')

    def test_metal_stop_names_the_same_xcode(self):   # own_compilers.py's stop message: the Xcode to install and select (GitHub issue #717)
        named = re.findall(r"XCODE_VERSION = '(\d+\.\d+)'", (REPO / 'dm/gfx/tools/own_compilers.py').read_text())
        self.assertEqual(named, [self.xcode], f'dm/gfx/tools/own_compilers.py: XCODE_VERSION is not CI\'s {self.xcode}')

    def test_nsp_script_names_the_same_xcode(self):
        named = re.findall(r'Xcode (\d+\.\d+)', (REPO / 'scripts/build_ipa_from_nsp.py').read_text())
        self.assertTrue(named, 'scripts/build_ipa_from_nsp.py names no Xcode version')
        self.assertEqual(set(named), {self.xcode}, f'scripts/build_ipa_from_nsp.py: the tested Xcode is not CI\'s {self.xcode}')


if __name__ == '__main__':
    unittest.main()
