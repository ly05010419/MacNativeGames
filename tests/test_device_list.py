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

    def test_the_lists_are_read_and_written_off_the_recording_thread(self):
        """2026-11-18: the first row recorded in play read the App's lists (Ocarina of Time: 27 MB, 27,076 rows) on the
        submit thread with the replay section held: 104 ms, the one frame over 100 ms left in every session of the
        user's recorded route. The filter and the file write run on a serial queue now, rows in their order; the
        queue is drained when the window closes and when the process exits."""
        record = body('pso_user_record')
        queued = record.index('dispatch_async(pso_user_queue(), ^{')
        for late in ('g_pso_shared = fopen(device_list()', 'componentsSeparatedByString', 'fputs(line, g_pso_shared)', 'atomic_fetch_add(&g_collect_new[kind], 1)'):
            self.assertGreater(record.index(late), queued, late)
        self.assertLess(record.index('pso_capture(line);'), queued)          # the capture diagnostic and the played list
        self.assertLess(record.index('pso_played_record(line);'), queued)    # see every row, as before
        queue = SOURCE[SOURCE.index('static dispatch_queue_t pso_user_queue(void) {'):SOURCE.index('static void pso_user_record(')]
        self.assertIn('DISPATCH_QUEUE_SERIAL', queue)
        self.assertIn('atexit_b(^{ dispatch_sync(q, ^{}); });', queue)
        quit_ = SOURCE[SOURCE.index('void mtl_pipeline_archive_quit(void) {'):]
        self.assertIn('dispatch_sync(pso_user_queue(), ^{});', quit_[:quit_.index('\n}\n')])

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
