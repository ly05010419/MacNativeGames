#!/usr/bin/env python3
"""Verify private image mapping without executing the mapped game code."""
import argparse
import ctypes
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'scripts'))
sys.path.insert(0, str(GAME / 'third_party/d2r'))
from rewrite_modules import MODULES
from module_metadata import verify_rewrite_report
from nsoimg import header


class Spec(ctypes.Structure):
    _fields_ = [('file_size', ctypes.c_size_t), ('image_size', ctypes.c_size_t),
                ('key_offset', ctypes.c_size_t), ('patch_offset', ctypes.c_size_t),
                ('patch_size', ctypes.c_size_t), ('sha256', ctypes.c_ubyte * 32)]


class Image(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('mapped_size', ctypes.c_size_t),
                ('key_offset', ctypes.c_size_t), ('patch_offset', ctypes.c_size_t),
                ('patch_size', ctypes.c_size_t), ('sealed', ctypes.c_int)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--rewritten', type=Path, required=True)
    args = parser.parse_args()
    _, report = verify_rewrite_report(args.exefs.resolve(), args.rewritten.resolve())
    with tempfile.TemporaryDirectory() as tmp:
        library = Path(tmp) / 'image.dylib'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                        '-dynamiclib', '-I', str(GAME / 'runtime'), str(GAME / 'runtime/image.c'),
                        '-o', str(library)], check=True)
        api = ctypes.CDLL(str(library))
        api.tmnt_image_map.argtypes = [ctypes.c_char_p, ctypes.POINTER(Spec), ctypes.POINTER(Image)]
        api.tmnt_image_map.restype = ctypes.c_int
        api.tmnt_image_unmap.argtypes = [ctypes.POINTER(Image)]
        for name in MODULES:
            folder = args.rewritten
            layout = header(folder, name)
            if layout.get('PATCH_LEAD', 0):
                raise ValueError(f'{name}: leading patch layout not supported')
            image_path = folder / f'{name}.patched.bin'
            digest = bytes.fromhex(report['modules'][name]['outputs']['patched.bin'])
            spec = Spec(image_path.stat().st_size, layout['IMAGE_SIZE'], layout['KEY_PAGE_OFF'],
                        layout['PATCH_OFF'], layout['PATCH_SIZE'], (ctypes.c_ubyte * 32).from_buffer_copy(digest))
            mapped = Image()
            status = api.tmnt_image_map(bytes(image_path), ctypes.byref(spec), ctypes.byref(mapped))
            if status:
                raise ValueError(f'{name}: mapping failed with status {status}')
            try:
                actual = hashlib.sha256(ctypes.string_at(mapped.base, spec.file_size)).digest()
                if actual != digest:
                    raise ValueError(f'{name}: mapped digest mismatch')
            finally:
                api.tmnt_image_unmap(ctypes.byref(mapped))
            print(f'{name}: non-executable private mapping verified', flush=True)


if __name__ == '__main__':
    main()
