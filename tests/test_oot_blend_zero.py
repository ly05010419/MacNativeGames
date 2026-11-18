"""Ocarina of Time: NVIDIA's "zero times anything is zero" blend (nvn_blend_zero.h; nxnative 0662f9d8 / PR #2030 / issue #1839).
The mist shader 5ad65c0eefd09c7f writes NaN with alpha 0 into an RGBA16F target blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA;
Apple's blender keeps the NaN and the bloom makes white clouds of it (2026-11-13: the whole screen in Kokiri Forest, gone
with that one shader skipped). The guard is checked on this Mac's GPU, the transform on the shapes the translator writes,
the wiring in the translation and the runtime by their source, and the chain moves with it."""
from pathlib import Path
import random
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
PROBE = ROOT / 'tests/blend_zero_probe.m'
SEED = Path('/Volumes/开发/MacNativeGames-artifacts/keep/apps/Ocarina of Time.app/Contents/Resources/totk/build/shader-seed')


def seed_sample(n=40):
    """The mist shader and a sample of a local Ocarina of Time seed's pixel shaders, when there is one."""
    if not SEED.is_dir():
        return []
    names = [p for p in SEED.glob('*_b.metal')]
    pixel = [p for p in names if 'fragment main0_out main0(' in p.read_text(errors='replace')]
    random.Random(7).shuffle(pixel)
    mist = [p for p in pixel if p.name.startswith('5ad65c0eefd09c7f')]
    return [str(p) for p in mist + pixel[:n]]


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools and a Metal GPU')
class BlendZeroGpu(unittest.TestCase):
    def test_the_guard_on_this_gpu(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-w', f'-I{NVN}', str(PROBE), '-framework', 'Foundation',
                                    '-framework', 'Metal', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            r = subprocess.run([str(exe), *seed_sample()], capture_output=True, text=True, timeout=600)
        lines = r.stdout.splitlines()
        self.assertTrue(lines and lines[-1].startswith('done '), r.stdout[-3000:] + r.stderr[-2000:])
        failed = [l for l in lines if l.startswith('FAIL')]
        self.assertEqual(failed, [], '\n'.join(failed))
        self.assertEqual(lines[-1], 'done 0')


class Wiring(unittest.TestCase):
    def test_ocarina_s_translations_get_the_guard_and_the_runtime_binds_its_word(self):
        rt = (NVN / 'totk_shader_rt.m').read_text()
        self.assertIn('#if defined(GAME_OOT)\n    if (!strcmp(stage, "ps")) refl = blend_zero_guard(variant, refl);', rt)
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('o->has_bz=[j[@"blend_zero"] boolValue];', mtl)
        self.assertIn('if (nvn_stage == 1 && !metal_vertex && sh->has_bz) bz_bind(en);', mtl)
        self.assertIn('enc_r_args((id<MTL4RenderCommandEncoder>)en, MTLRenderStageFragment, gpu, BLEND_ZERO_BUFFER);', mtl)

    def test_the_buffer_is_free(self):
        # translated shaders use buffer 0; 29 is TOTK_NAN_TRACE's, 30 the HDR twin's
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertNotRegex(mtl, r'MTLRenderStageFragment,[^;]*,\s*28\)')
        self.assertIn('#define BLEND_ZERO_BUFFER 28', (NVN / 'nvn_blend_zero.h').read_text())

    def test_ocarina_s_chain_names_the_guard(self):
        text = (NVN / 'dmsh_chain.h').read_text()
        # the guard came with v27; a later chain keeps it (and still names it on the line)
        m = re.search(r'#define DMSH_CHAIN "oot-native-v(\d+)-[^"]*"\s*/\* game: oot \*/(.*)', text)
        self.assertIsNotNone(m)
        self.assertGreaterEqual(int(m.group(1)), 27)
        self.assertIn('nvn_blend_zero.h', m.group(2))


if __name__ == '__main__':
    unittest.main()
