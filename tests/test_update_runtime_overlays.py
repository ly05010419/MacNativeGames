"""update_runtime.py check_overlays (after PR #1222): a picture-size RomFS overlay made by another
make_output_res_overlay.py than this repository's is named, since only a full rebuild renews it (#1182)."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import update_runtime  # noqa: E402


class Overlays(unittest.TestCase):
    def setUp(self):
        self.res = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.res)
        self.want = update_runtime.sha256(ROOT / 'games/totk/scripts/make_output_res_overlay.py')

    def overlay(self, size, record):
        folder = self.res / 'extracted/v143/romfs-patches-out' / size
        folder.mkdir(parents=True)
        (folder / 'overlay.json').write_text(record if isinstance(record, str) else json.dumps(record))

    def test_this_repositorys_script_needs_no_rebuild(self):
        self.overlay('1920x1080', {'script': 'make_output_res_overlay.py', 'script_sha256': self.want})
        self.assertEqual(update_runtime.check_overlays(self.res, 'v143'), [])

    def test_another_script_or_a_broken_record_is_named(self):
        self.overlay('1920x1080', {'script_sha256': self.want})
        self.overlay('2560x1440', {'script_sha256': '0' * 64})
        self.overlay('3840x2160', '{not json')
        self.assertEqual(update_runtime.check_overlays(self.res, 'v143'), ['2560x1440', '3840x2160'])

    def test_no_overlays(self):
        self.assertEqual(update_runtime.check_overlays(self.res, 'botw190'), [])


if __name__ == '__main__':
    unittest.main()
