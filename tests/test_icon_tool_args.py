"""The button icon tools take the argument list that app/launcher.m (button_icons) builds, for every game version it can name:
argparse accepts every argument, so a run only stops on the missing RomFS (exit 2), never on the command line."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
BOTW = REPO / 'games/botw/scripts/botw_prompts.py'
TOTK = REPO / 'games/totk/scripts/make_button_icons.py'


def launcher_arguments(version, style, face, scale=None):
    """The arguments after the tool in launcher.m: the folder to fill, --romfs extracted/<version>/romfs, --version, --style,
    --face-buttons, --scale (the Breath of the Wild tool only), --controls."""
    return ['icons', '--romfs', f'extracted/{version}/romfs', '--version', version, '--style', style, '--face-buttons', face,
            *(['--scale', str(scale)] if scale else []), '--controls', 'controls.txt']


class LauncherArguments(unittest.TestCase):
    def run_tool(self, tool, *arguments):
        with tempfile.TemporaryDirectory() as tmp:       # the launcher's folder: no extracted/<version>/romfs in it
            Path(tmp, 'controls.txt').write_text('')
            run = subprocess.run([sys.executable, '-B', str(tool), *arguments], cwd=tmp, capture_output=True, text=True)
            self.assertFalse(Path(tmp, 'icons').exists(), 'nothing is written when the RomFS is missing')
        if run.returncode and ('fontTools' in run.stderr or 'needs Python 3.14' in run.stderr):
            self.skipTest('needs fontTools (requirements.txt) and Python 3.14')
        return run

    def assert_accepted(self, run):
        self.assertEqual(run.returncode, 2, run.stderr)
        self.assertIn('not found', run.stderr)
        for refusal in ('unrecognized arguments', 'invalid choice', 'expected one argument'):
            self.assertNotIn(refusal, run.stderr)

    def test_botw_tool_takes_the_launcher_arguments_of_every_version(self):
        for version in ('botw160', 'botw190'):
            for style, face in (('xbox', 'position'), ('playstation', 'position'), ('xbox', 'label'), ('switch', 'label')):
                with self.subTest(version=version, style=style, face=face):
                    self.assert_accepted(self.run_tool(BOTW, *launcher_arguments(version, style, face, scale=2)))

    def test_totk_tool_takes_the_launcher_arguments_of_every_version(self):
        for version in ('v0', 'v112', 'v121', 'v143'):
            for style, face in (('xbox', 'position'), ('playstation', 'position'), ('xbox', 'label')):
                with self.subTest(version=version, style=style, face=face):
                    self.assert_accepted(self.run_tool(TOTK, *launcher_arguments(version, style, face)))

    def test_negative_control_an_argument_a_tool_does_not_know_is_seen(self):
        run = self.run_tool(TOTK, *launcher_arguments('v121', 'xbox', 'position', scale=2))      # --scale is for the BOTW tool only
        self.assertEqual(run.returncode, 2)
        self.assertIn('unrecognized arguments', run.stderr)
        run = self.run_tool(BOTW, *launcher_arguments('v121', 'xbox', 'position', scale=2))      # a TOTK version is not a BOTW one
        self.assertEqual(run.returncode, 2)
        self.assertIn('invalid choice', run.stderr)


if __name__ == '__main__':
    unittest.main()
