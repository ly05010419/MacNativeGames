# dm/games/game.py's profile-key validation, on a synthetic profile: pso_gpu (the GPU architectures the app's offline pipeline archive is made for, beside pso_mtl4) takes, none and no others,
# and a profile without the key generates its header unchanged; the game's upscaler shaders (the launcher's Game's upscaler) are the compute shaders its shader map found in agl_ndsr.pck
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
GAME = REPO / 'dm/games/game.py'
ID = '0100000000F0F000'
TSV = ('title_id\t' + ID + '\nname\tTest\nprogram_id\t' + ID + '\nmodules\tmain\nromfs_marker\tromfs.img\n'   # what a header needs beside the keys under test
       'mem_reserve\t0x100000\nmem_alias\t0x100000\nmem_stack\t0x100000\napp_pool\t0x100000\n')


def header(rows):
    """game.py header of a synthetic profile with these extra rows: (exit code, its output)"""
    with tempfile.TemporaryDirectory() as d:
        Path(d, ID).mkdir()
        (Path(d, ID) / 'game.tsv').write_text(TSV + rows)
        r = subprocess.run([sys.executable, str(GAME), 'header', ID, str(Path(d, ID) / 'game.h')],
                           env={**os.environ, 'DM_GAMES_DIR': d}, capture_output=True, text=True)
    return r.returncode, (r.stderr + r.stdout).strip()


class PsoGPU(unittest.TestCase):
    def test_valid_tokens(self):
        for rows in ('', 'pso_mtl4\ton\npso_gpu\tg18p\n', 'pso_mtl4\ton\npso_gpu\tapplegpu_g19p\n',   # without the key: the header is made as before, no message
                     'pso_mtl4\ton\npso_gpu\tg18p g19p\n', 'pso_mtl4\ton\npso_gpu\tg14s g15s g16s g17p\n'):
            rc, out = header(rows)
            self.assertEqual((rc, out), (0, ''), rows)

    def test_rejected_tokens(self):
        for rows in ('pso_gpu\tnone\n',                     # none is not a value: absence of the key says no archive of its own
                     'pso_mtl4\ton\npso_gpu\tg18p none\n', 'pso_gpu\tx g18p!\n', 'pso_gpu\tg14s g15s g16s g17p g18p\n',
                     'pso_gpu\tapplegpu_g19p,x\n'):
            rc, out = header(rows)
            self.assertNotEqual(rc, 0, rows)
            self.assertIn(': pso_gpu takes 1-4 whitespace-separated GPU architectures', out, rows)



MAP = ('# kind: evidence  Test shaders\n# column 5 records where each key first appears\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n'
       'cs_other.bin\t1\tcompute\tc56db3d5a0f1b5af\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_ndsr.pck#NDSR@0x124900\n'
       'cs_other.bin\t2\tcompute\t7629d857a0a93833\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.debug.sarc.zs/agl_ndsr.pck#NDSR@0xeb4c0\n'
       'cs_other.bin\t3\tcompute\t7629d857a0a93833\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_ndsr.pck#NDSR@0x124a00\n'   # a key twice: once
       'ps_other.bin\t4\tpixel\t0e423e497f1e3355\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_ndsr.pck#NDSR@0x1247d8\n'   # the pack's draw shaders: not the upscaler
       'cs_other.bin\t5\tcompute\t0123456789abcdef\t/Lib/agl/agl_resource.Nin_Ounce_NVN2.release.sarc.zs/agl_common.sharcb#BAHS@0x40\n')


def macros(shader_map):
    """The upscaler macros of a synthetic profile's header, with this shader map (None: none)"""
    with tempfile.TemporaryDirectory() as d:
        Path(d, ID).mkdir()
        (Path(d, ID) / 'game.tsv').write_text(TSV)
        if shader_map is not None: (Path(d, ID) / 'shader_map.tsv').write_text(shader_map)
        subprocess.run([sys.executable, str(GAME), 'header', ID, str(Path(d, ID) / 'game.h')], env={**os.environ, 'DM_GAMES_DIR': d}, check=True, capture_output=True)
        return [l for l in (Path(d, ID) / 'game.h').read_text().splitlines() if 'UPSCALER' in l]


class UpscalerKeys(unittest.TestCase):
    def test_from_shader_map(self):
        none = ['#define DM_GAME_UPSCALER_KEYS ""', '#define DM_GAME_UPSCALER_N 0']   # the launcher hides the setting
        self.assertEqual(macros(None), none)
        self.assertEqual(macros(MAP.split('cs_other.bin')[0]), none)
        self.assertEqual(macros(MAP), ['#define DM_GAME_UPSCALER_KEYS "7629d857a0a93833,c56db3d5a0f1b5af"', '#define DM_GAME_UPSCALER_N 2'])


if __name__ == '__main__':
    unittest.main()
