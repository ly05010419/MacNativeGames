"""What a game's NROs decide (Portal: the Source engine in 28 NROs under an SDL main): the graphics API, the run-time GLSLC capture, and zip members for the shader split."""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); import profile_guess as pg
_had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01007BB017812000'   # shader_split loads a profile at import; other tests expect no default game
sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import shader_split as ss
if _had is None: del os.environ['DM_GAME']
else: os.environ['DM_GAME'] = _had

SDL_MAIN = {'eglCreateWindowSurface', 'eglGetProcAddress'}   # SDL's EGL backend; main names glShaderSource (gl_names) for SDL's GL renderer


class NroGraphics(unittest.TestCase):
    def test_nvn_in_nros_beats_an_sdl_main(self):
        s = pg.shader_sources(SDL_MAIN, 0, ['subsdk0'], 0, 0, gl_names=2, nro_imports={'nvnBootstrapLoader', 'glslcCompile'})
        self.assertEqual(s['graphics'][0], 'nvn')
        self.assertTrue(s['glslc_capture'][0])
        self.assertFalse(s['gl_capture'][0])

    def test_gl_without_nros_unchanged(self):   # Digimon Story Cyber Sleuth: no NRO, GL through EGL
        s = pg.shader_sources(SDL_MAIN | {'nvnBootstrapLoader'}, 0, [], 0, 0, gl_names=1, gl_files=1)
        self.assertEqual(s['graphics'][0], 'gl')
        self.assertFalse(s['glslc_capture'][0])

    def test_nro_glslc_needs_a_module_that_defines_it(self):
        s = pg.shader_sources(set(), 0, [], 0, 0, nro_imports={'nvnBootstrapLoader', 'glslcCompile'})
        self.assertEqual(s['graphics'][0], 'nvn')
        self.assertFalse(s['glslc_capture'][0])

    def test_load_order(self):
        self.assertEqual(sorted(['sdk', 'subsdk2', 'main', 'subsdk0', 'subsdk10'], key=pg.load_order), ['main', 'subsdk0', 'subsdk2', 'subsdk10', 'sdk'])


class ZipMembers(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def test_members_in_ranges_and_not_scanned_whole(self):
        z = self.tmp / 'game.zip'
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as f:
            f.writestr('nxcontent/nvsb/', b'')
            for i in range(10): f.writestr(f'nxcontent/nvsb/{i}.nvsb', bytes(100))
        (self.tmp / 'loose.bin').write_bytes(b'not a zip')
        jobs, zips = ss.zip_jobs([(str(z), 'game.zip'), (str(self.tmp / 'loose.bin'), 'loose.bin')], parts=4)
        self.assertEqual(zips, {str(z)})
        names = [n for _, _, (tag, ns) in jobs for n in ns]
        self.assertTrue(all(tag == ss.ZIP for _, _, (tag, _) in jobs))
        self.assertEqual(sorted(names), sorted(f'nxcontent/nvsb/{i}.nvsb' for i in range(10)))   # the directory entry is left out
        self.assertGreater(len(jobs), 1)
        out = []
        for p, r, (_, ns) in jobs: ss.zip_shaders(p, r, ns, out)
        self.assertEqual(out, [])   # zero-filled members hold no shader


if __name__ == '__main__':
    unittest.main()
