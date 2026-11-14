import ctypes
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path


class ImageSpec(ctypes.Structure):
    _fields_ = [('file_size', ctypes.c_size_t), ('image_size', ctypes.c_size_t),
                ('key_offset', ctypes.c_size_t), ('patch_offset', ctypes.c_size_t),
                ('patch_size', ctypes.c_size_t), ('sha256', ctypes.c_ubyte * 32)]


class Image(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('mapped_size', ctypes.c_size_t),
                ('key_offset', ctypes.c_size_t), ('patch_offset', ctypes.c_size_t),
                ('patch_size', ctypes.c_size_t), ('sealed', ctypes.c_int)]


class ImageTests(unittest.TestCase):
    def test_synthetic_memory_protection(self):
        game = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'protection-probe'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-Wno-deprecated-declarations', '-I', str(game / 'runtime'),
                            str(game / 'runtime/image.c'), str(game / 'tests/image_protection_probe.c'),
                            '-o', str(probe)], check=True)
            subprocess.run([str(probe)], check=True)

    def test_digest_layout_and_private_mapping(self):
        game = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            library = root / 'image.dylib'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                            '-dynamiclib', '-I', str(game / 'runtime'), str(game / 'runtime/image.c'),
                            '-o', str(library)], check=True)
            api = ctypes.CDLL(str(library))
            api.tmnt_image_map.argtypes = [ctypes.c_char_p, ctypes.POINTER(ImageSpec), ctypes.POINTER(Image)]
            api.tmnt_image_map.restype = ctypes.c_int
            api.tmnt_image_unmap.argtypes = [ctypes.POINTER(Image)]
            original = b'A' * 0x8000 + b'patch-data-12345'
            path = root / 'module.bin'
            path.write_bytes(original)
            spec = ImageSpec(len(original), 0x8000, 0x4000, 0x8000, len(original) - 0x8000,
                             (ctypes.c_ubyte * 32).from_buffer_copy(hashlib.sha256(original).digest()))
            mapped = Image()
            self.assertEqual(api.tmnt_image_map(bytes(path), ctypes.byref(spec), ctypes.byref(mapped)), 0)
            self.assertGreaterEqual(mapped.mapped_size, len(original))
            self.assertEqual(ctypes.string_at(mapped.base, len(original)), original)
            ctypes.c_ubyte.from_address(mapped.base).value = ord('B')
            self.assertEqual(path.read_bytes(), original)
            api.tmnt_image_unmap(ctypes.byref(mapped))
            self.assertFalse(mapped.base)
            spec.sha256[0] ^= 1
            self.assertEqual(api.tmnt_image_map(bytes(path), ctypes.byref(spec), ctypes.byref(mapped)), 4)
            self.assertFalse(mapped.base)
            spec.image_size += 1
            self.assertEqual(api.tmnt_image_map(bytes(path), ctypes.byref(spec), ctypes.byref(mapped)), 2)
            self.assertFalse(mapped.base)
            spec.image_size -= 1
            spec.sha256[0] ^= 1
            link = root / 'link.bin'
            link.symlink_to(path)
            self.assertEqual(api.tmnt_image_map(bytes(link), ctypes.byref(spec), ctypes.byref(mapped)), 3)
            self.assertFalse(mapped.base)


if __name__ == '__main__':
    unittest.main()
