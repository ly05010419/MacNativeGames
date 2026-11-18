"""This device keeps one warm-up list, the shader cache folder's pso_collected.tsv (user 2026-11-16, "1 做").

The probe runs totk_device_list_merge (the old profile pso_user.tsv merged in once); the source checks keep the runtime
from writing or warming a second device list again.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
SOURCE = (NVN / 'nvn_mtl.m').read_text(encoding='utf-8')


def body(name):
    match = re.search(rf'static \w+ \*?{name}\([^;{{]*\)\s*\{{', SOURCE)
    if not match: raise AssertionError(f'{name} is not defined')
    depth, i = 0, match.end() - 1
    for j in range(i, len(SOURCE)):
        depth += {'{': 1, '}': -1}.get(SOURCE[j], 0)
        if depth == 0: return SOURCE[i:j + 1]
    raise AssertionError(f'{name} has no end')


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires macOS Foundation')
class DeviceListMerge(unittest.TestCase):
    def test_merge(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'probe'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Werror', f'-I{NVN}', str(ROOT / 'tests/device_list_probe.m'),
                            '-framework', 'Foundation', '-o', str(binary)], check=True, capture_output=True, text=True)
            work = Path(tmp) / 'work'; work.mkdir()
            result = subprocess.run([binary, str(work)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class OneDeviceList(unittest.TestCase):
    def test_rows_are_written_to_one_list(self):
        record = body('pso_user_record')
        self.assertIn('device_list()', record)
        self.assertNotIn('pso_user_file', SOURCE, 'the profile list is written again')
        self.assertNotIn('pso_user_path()', record)

    def test_warm_up_takes_the_one_list_as_this_devices(self):
        start = body('warm_start')
        self.assertIn('NSString *user = device_list();', start)
        self.assertNotIn('pso_shared_path()];', start.split('NSString *user = device_list();', 1)[1].split('paths = all;', 1)[0],
                         'the device list is warmed twice')
        self.assertIn('device_list()', body('warm_light'))   # iOS light warm-up

    def test_the_old_list_is_merged_once(self):
        dev = body('device_list')
        self.assertIn('totk_device_list_merge(pso_user_path(), pso_shared_path()', dev)
        self.assertIn('dispatch_once', dev)


if __name__ == '__main__':
    unittest.main()
