"""A new NVN texture over memory an earlier texture rendered into must see that rendering (Ocarina of Time terrain tiles, 2026-11-09)."""
import re
import unittest
from pathlib import Path

GFX = Path(__file__).resolve().parents[1] / 'games/oot/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless((GFX / 'nvn_mtl.m').is_file(), 'a branch without this game')
class StorageReuse(unittest.TestCase):
    def test_initializing_a_texture_writes_back_earlier_renderings(self):
        source = (GFX / 'hle_nvn.c').read_text(errors='replace')
        init = re.search(r'NVN\(nvnTextureInitialize,.*?\n\}', source, re.S).group(0)
        self.assertIn('mtl_wb_storage_reused(', init)

    def test_only_finished_same_sized_renderings_are_written_back(self):
        source = (GFX / 'nvn_mtl.m').read_text(errors='replace')
        body = re.search(r'void mtl_wb_storage_reused\(.*?\n\}', source, re.S).group(0)
        for needed in ('n > WB_SNAP_MAX', 'e->t.L.size != n', 'e->fence > done', 'wb_write_list(list, nl, 0)'):
            self.assertIn(needed, body)


if __name__ == '__main__':
    unittest.main()
