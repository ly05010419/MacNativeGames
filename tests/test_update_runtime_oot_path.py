"""update_runtime.py update_ocarina (#2074): the Ocarina of Time update runs its build with the Homebrew tool folders on
PATH, as main() does for Tears of the Kingdom and Breath of the Wild. Started from the app's Update button, the update
inherits the Finder's PATH, and make_opening_overlay.py then found no ffmpeg or ffprobe."""
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import update_runtime  # noqa: E402


class OcarinaToolPath(unittest.TestCase):
    def run_update(self, path):
        seen = {}
        module = mock.Mock()
        module.update_runtime.side_effect = lambda app, work: seen.update(path=os.environ['PATH'])
        with mock.patch.dict(os.environ, {'PATH': path}), \
             mock.patch('importlib.util.spec_from_file_location'), \
             mock.patch('importlib.util.module_from_spec', return_value=module):
            update_runtime.update_ocarina(Path('/Applications/Ocarina of Time.app'), None)
        module.update_runtime.assert_called_once()
        return seen['path'].split(os.pathsep)

    def test_finder_path_gains_the_tool_folders(self):
        parts = self.run_update('/usr/bin:/bin:/usr/sbin:/sbin')
        self.assertEqual(parts[:len(update_runtime.build_app.TOOL_PATHS)], update_runtime.build_app.TOOL_PATHS)
        self.assertIn('/opt/homebrew/bin', parts)
        self.assertEqual(parts[-4:], ['/usr/bin', '/bin', '/usr/sbin', '/sbin'])

    def test_the_callers_path_is_kept(self):
        self.assertIn('/custom/tools', self.run_update('/custom/tools:/usr/bin'))


if __name__ == '__main__':
    unittest.main()
