"""The shader seed's image (2026-11-13): ios/tools/seed_image.py writes the per-shader files into parts beside the seed
folder, and the runtime (games/totk/third_party/d2r_gfx/gfx/nvn/totk_seed_image.h) reads them in place. An iPad
unpacked about 128,000 files after every update and removed the last seed's 128,000; nxnative keeps a game's shader
libraries as one image (dm/gfx/nvn/msl_image.h)."""
import hashlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
sys.path.insert(0, str(ROOT / 'games/oot/ios/tools'))
import seed_image  # noqa: E402

KEY = 'a' * 64


def seed_files(folder):
    files = {
        f'{KEY}-b7530f698e76_b.metal': b'#include <metal_stdlib>\n' * 300,
        f'{KEY}-b7530f698e76_b.json': b'{"chain": "x"}',
        f'{KEY}-b7530f698e76_b.metallib': bytes(range(256)) * 40,
        f'{KEY}.variants': b'[]',
        f'{"b" * 64}-b7530f698e76_b.metal': b'',
        f'{"c" * 64}-b7530f698e76_b.metal': b'x' * 5000,
        'seed-manifest.json': b'{}', 'pso_title.tsv': b'R\tx\n', 'variant-' + 'd' * 64 + '.metallib': b'MTLB',
        'pipelines-applegpu_g15p-g0-0.metalarchive': b'archive',
    }
    for name, data in files.items(): (folder / name).write_bytes(data)
    return files


class Writer(unittest.TestCase):
    def test_only_the_shaders_own_files(self):
        names = seed_image.image_names(['seed-manifest.json', f'{KEY}.variants', f'{KEY}-b7530f698e76_b.json', 'variant-x.metallib',
                                        'pipelines-applegpu_g15p-g0-1.metalarchive', 'pipelines-applegpu_g15p-g0-1.keys'])
        self.assertEqual(names, [f'{KEY}-b7530f698e76_b.json', f'{KEY}.variants'])

    def test_parts_hold_every_file_once(self):
        with tempfile.TemporaryDirectory() as d:
            seed = Path(d) / 'seed'; seed.mkdir(); files = seed_files(seed)
            names = seed_image.image_names(files)
            done = []
            parts = seed_image.write_parts(seed, names, Path(d) / 'shader-seed-x', 6000, done.append)
            self.assertGreater(len(parts), 1, 'small parts: several')
            self.assertEqual(done, parts)
            self.assertEqual([p.name for p in parts], [f'shader-seed-x.img.{i:03d}' for i in range(len(parts))])
            got = {}
            for p in parts:
                part = seed_image.read_part(p)
                self.assertFalse(set(part) & set(got), 'no file in two parts')
                got.update(part)
            self.assertEqual(got, {n: files[n] for n in names})

    def test_every_copy_is_the_same_file(self):
        copies = sorted((ROOT / 'games').glob('*/ios/tools/seed_image.py'))
        self.assertGreaterEqual(len(copies), 2)
        self.assertEqual(len({p.read_bytes() for p in copies}), 1, [str(p.relative_to(ROOT)) for p in copies])


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class RuntimeReader(unittest.TestCase):
    def test_runtime_reads_what_the_writer_wrote(self):
        with tempfile.TemporaryDirectory() as d:
            seed = Path(d) / 'seed'; seed.mkdir(); files = seed_files(seed)
            names = seed_image.image_names(files)
            parts = seed_image.write_parts(seed, names, Path(d) / 'shader-seed-x', 6000)
            exe = Path(d) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Werror', f'-I{NVN}', str(ROOT / 'tests/seed_image_probe.m'),
                                    '-framework', 'Foundation', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            asked = names + ['seed-manifest.json', 'nothing']
            out = subprocess.run([str(exe), str(Path(d) / 'shader-seed-x'), *asked], capture_output=True, text=True, timeout=60).stdout.splitlines()
            self.assertEqual(out[0], f'parts {len(parts)}')
            want = [f'{n} {len(files[n])} {hashlib.sha256(files[n]).hexdigest()}' for n in names] + ['seed-manifest.json missing', 'nothing missing']
            self.assertEqual(out[1:], want)
            # A damaged part ends the image there: the folder's files are read instead
            raw = bytearray(parts[0].read_bytes()); raw[16] ^= 0xff; parts[0].write_bytes(bytes(raw))
            out = subprocess.run([str(exe), str(Path(d) / 'shader-seed-x')], capture_output=True, text=True, timeout=60).stdout.splitlines()
            self.assertEqual(out[0], 'parts 0')

    def test_seed_reads_go_through_the_image(self):
        """Every read of a seed file by its totk_rt_file path can find it in the image."""
        rt = (NVN / 'totk_shader_rt.m').read_text(); mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('seed_image_open(&g_seed_img, source);', rt)
        for text in (rt, mtl, (NVN / 'nvn_raw_write_domain.inc').read_text(), (NVN / 'nvn_mesh_translation.inc').read_text()):
            self.assertNotRegex(text, r'(stringWithContentsOfFile|dataWithContentsOfFile|fileExistsAtPath):\s*totk_rt_file\(')
            self.assertNotRegex(text, r'dataWithContentsOfFile:\[\[(sh|ms)->src_path')
        self.assertIn('NSData *img = totk_rt_read(path);', mtl, 'lib_from_file falls back to the image')


class Install(unittest.TestCase):
    def test_install_pushes_image_parts(self):
        for g in ('oot', 'totk'):
            text = (ROOT / f'games/{g}/ios/tools/install_device.py').read_text()
            self.assertIn("write_parts(seed, imaged, Path(d)/folder, PART, push)", text, g)
            self.assertIn("write_pack(seed, loose, Path(d), folder, PART, push)", text, g)


if __name__ == '__main__':
    unittest.main()
