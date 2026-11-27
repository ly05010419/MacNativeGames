"""HDR / EDR on Breath of the Wild 1.9.0 (totk_hdr_compose.h botw_hdr_compose_source): the scene compose f7ee6fe1's
HDR twin, drawn on the GPU by tests/botw_hdr_compose_probe.m. The compose MSL is translated from the player's game, so
it is not in the repository: BOTW190_SHADER_SEED names the botw190 shader-seed folder (default
games/totk/build/shader-seed/botw190); TOTK143_SHADER_SEED, the v143 one, adds the cross check. Skipped without it."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOTW_SEED = Path(os.environ.get('BOTW190_SHADER_SEED', ROOT / 'games/totk/build/shader-seed/botw190'))
TOTK_SEED = Path(os.environ.get('TOTK143_SHADER_SEED', ROOT / 'games/totk/build/shader-seed/v143'))
BOTW_COMPOSE = 'f7ee6fe10b925543e905dfc9437ce67dc21a118d465b03bb3c9eccafdf2e33c4-b7530f698e76_b.metal'
TOTK_COMPOSE = '0ac9a54938974396134d911dc0f29c79ce43e3d8cd50b11ee8b85a48ead6d2a5-b7530f698e76_b.metal'


@unittest.skipUnless(sys.platform == 'darwin', 'Metal probe')
@unittest.skipUnless((BOTW_SEED / BOTW_COMPOSE).is_file(), 'no BotW 1.9.0 shader seed (BOTW190_SHADER_SEED)')
class BotwHdrCompose(unittest.TestCase):
    def test_compose_twin(self):
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / 'probe'
            built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    str(ROOT / 'tests/botw_hdr_compose_probe.m'), '-framework', 'Foundation',
                                    '-framework', 'Metal', '-o', str(probe)], capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            command = [str(probe), str(BOTW_SEED / BOTW_COMPOSE)]
            if (TOTK_SEED / TOTK_COMPOSE).is_file():
                command.append(str(TOTK_SEED / TOTK_COMPOSE))
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('pass: BotW 1.9.0 HDR compose', result.stdout)

    def test_final_copy_keys(self):
        # hdr_capture keeps the FP16 canvas only at the game's last copy into the presented image; a key that names no
        # shader of the game means HDR never reaches the screen (the surface stays SDR with no error).
        render = (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_hdr_render.h').read_text()
        block = render[render.index('#ifdef GAME_BOTW190', render.index('static void hdr_capture')):]
        block = block[:block.index('#else')]
        keys = re.findall(r'"([0-9a-f]{64})-"', block)
        self.assertEqual(len(keys), 2, block)
        for key in keys:
            self.assertTrue(list(BOTW_SEED.glob(key + '-*.metal')), f'{key} is not a BotW 1.9.0 shader')
        # the fragment is a plain copy of one texture: the canvas, which carries the FP16 companion
        fragment = (BOTW_SEED / (keys[0] + '-b7530f698e76_b.metal')).read_text()   # keys[0]: the fragment (sh[1])
        self.assertIn('fragment', fragment)
        self.assertEqual(len(re.findall(r'texture2d<float>', fragment)), 1, fragment)


if __name__ == '__main__':
    unittest.main()
