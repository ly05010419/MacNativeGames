"""Ocarina of Time's Mac shader data bundle keeps the seed's and the libraries' shader files as image parts (2026-11-17).

From the USB drive the App is on, a library first used in play cost an open() of 100 ms or more inside the replay
(mtl4-2: 111 ms in lib_from_file), and the first camera turn into a new area stalled on batches of them: 64,699 library
files and 128,674 seed files. games/oot/build.py write_shader_images writes them as image parts beside the bundle's
folders (ios/tools/seed_image.py, the iPad's format) and keeps only the other files; the runtime finds the parts beside
the folder its build/ link points to (totk_seed_image.h seed_image_open) and takes a library from them by name
(nvn_mtl.m lib_image_data).
"""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
sys.path.insert(0, str(GAME / 'ios/tools'))
from seed_image import read_part  # noqa: E402


def load_build():
    spec = importlib.util.spec_from_file_location('oot_build_shader_images', GAME / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ShaderImages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.seed, self.libs, self.res = root / 'seed', root / 'libs', root / 'bundle/Contents/Resources'
        self.seed.mkdir(); self.libs.mkdir()
        self.key = 'a' * 64
        self.seed_files = {f'{self.key}.variants': b'[]', f'{self.key}-b7530f698e76_b.metal': b'kernel void main0() {}' * 8,
                           f'{self.key}-b7530f698e76_b.json': b'{"key": 1}'}
        for name, data in self.seed_files.items(): (self.seed / name).write_bytes(data)
        for name in ('seed-manifest.json', 'pso_title.tsv', 'shader-keys.json'): (self.seed / name).write_text(name)
        self.lib_files = {f'{c * 64}.metallib': b'MTLB' + c.encode() * 100 for c in 'bc'}
        for name, data in self.lib_files.items(): (self.libs / name).write_bytes(data)
        (self.libs / f'{"d" * 64}.metallib').write_bytes(b'MTLB not in the manifest')
        (self.libs / 'libraries-manifest.json').write_text(json.dumps({'files': sorted(self.lib_files)}))

    def tearDown(self):
        self.tmp.cleanup()

    def test_shader_files_go_into_image_parts_and_the_rest_stay_files(self):
        nseed, nlibs = load_build().write_shader_images(self.seed, self.libs, self.res)
        self.assertEqual((nseed, nlibs), (3, 2))
        seed_dir, libs_dir = self.res / 'shader-seed', self.res / 'shader-libs'
        self.assertEqual(sorted(p.name for p in seed_dir.iterdir()), ['pso_title.tsv', 'seed-manifest.json', 'shader-keys.json'])
        self.assertEqual(sorted(p.name for p in libs_dir.iterdir()), ['libraries-manifest.json'])
        self.assertEqual(read_part(self.res / 'shader-seed.img.000'), self.seed_files)
        self.assertEqual(read_part(self.res / 'shader-libs.img.000'), self.lib_files)   # only the manifest's libraries
        self.assertFalse((self.res / 'shader-seed.img.001').exists())

    def test_a_bundle_made_before_the_images_is_made_again(self):
        text = (GAME / 'build.py').read_text()
        self.assertIn("'layout':IMAGE_LAYOUT", text)   # part of the bundle's stamp (shader_bundle)

    def test_the_runtime_reads_both_images(self):
        image = (NVN / 'totk_seed_image.h').read_text()
        self.assertIn('stringByResolvingSymlinksInPath', image)   # build/shader-seed and shader-libs are links into the bundle
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('seed_image_open(&g_lib_img, dir)', mtl)
        self.assertIn('NSData *imaged = lib_image_data(path);', mtl)        # lib_from_file asks the image before open()
        self.assertIn('seed_image_find(&g_lib_img', mtl)                     # shader_lib_prebuilt_path: no stat on the drive
        self.assertIn('totk_contrib_seed_file(seedDir', (NVN / 'totk_shader_contrib.h').read_text())


if __name__ == '__main__':
    unittest.main()
