#!/usr/bin/env python3
"""Exercise mapping and relocation together without executing game code.

Symbols resolve to a synthetic address; this does not establish production behavior.
"""
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
from survey_modules import data_bounds, load, relocs, validate_relocations
from nsoimg import header
from integration_images import Image, Spec
from integration_relocations import Entry, Resolve, resolve


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--rewritten', type=Path, required=True)
    args = parser.parse_args()
    _, report = verify_rewrite_report(args.exefs.resolve(), args.rewritten.resolve())
    with tempfile.TemporaryDirectory() as tmp:
        library = Path(tmp) / 'loader-primitives.dylib'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                        '-dynamiclib', '-I', str(GAME / 'runtime'), str(GAME / 'runtime/image.c'),
                        str(GAME / 'runtime/relocate.c'), '-o', str(library)], check=True)
        api = ctypes.CDLL(str(library))
        api.tmnt_image_map.argtypes = [ctypes.c_char_p, ctypes.POINTER(Spec), ctypes.POINTER(Image)]
        api.tmnt_image_map.restype = ctypes.c_int
        api.tmnt_image_unmap.argtypes = [ctypes.POINTER(Image)]
        api.tmnt_relocate.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t,
                                       ctypes.POINTER(Entry), ctypes.c_size_t, Resolve, ctypes.c_void_p]
        api.tmnt_relocate.restype = ctypes.c_int
        total = 0
        for name in MODULES:
            folder = args.rewritten
            layout = header(folder, name)
            if layout.get('PATCH_LEAD', 0):
                raise ValueError(f'{name}: leading patch layout not supported')
            path = folder / f'{name}.patched.bin'
            digest = bytes.fromhex(report['modules'][name]['outputs']['patched.bin'])
            spec = Spec(path.stat().st_size, layout['IMAGE_SIZE'], layout['KEY_PAGE_OFF'],
                        layout['PATCH_OFF'], layout['PATCH_SIZE'], (ctypes.c_ubyte * 32).from_buffer_copy(digest))
            original = args.exefs / name
            start, end = data_bounds(original)
            if start != layout['DATA_OFF'] or end != layout['KEY_PAGE_OFF']:
                raise ValueError(f'{name}: original and rewritten data bounds differ')
            records = relocs(load(original, bss=False))
            validate_relocations(records, start, end)
            entries = (Entry * len(records))(*(Entry(off, kind, 1, addend, 0)
                                                for off, kind, _, _, addend in records))
            mapped = Image()
            status = api.tmnt_image_map(bytes(path), ctypes.byref(spec), ctypes.byref(mapped))
            if status:
                raise ValueError(f'{name}: mapping failed with status {status}')
            try:
                code_before = hashlib.sha256(ctypes.string_at(mapped.base, start)).digest()
                suffix_before = hashlib.sha256(ctypes.string_at(mapped.base + end, spec.file_size - end)).digest()
                status = api.tmnt_relocate(mapped.base, spec.image_size, start, end,
                                            entries, len(entries), resolve, None)
                if status:
                    raise ValueError(f'{name}: relocation failed with status {status}')
                if (hashlib.sha256(ctypes.string_at(mapped.base, start)).digest() != code_before or
                        hashlib.sha256(ctypes.string_at(mapped.base + end, spec.file_size - end)).digest() != suffix_before):
                    raise ValueError(f'{name}: relocation changed code, read-only data, or patch area')
            finally:
                api.tmnt_image_unmap(ctypes.byref(mapped))
            total += len(records)
            print(f'{name}: mapped and applied {len(records)} synthetic relocations', flush=True)
        print(f'total: {total} synthetic relocations; no code executed')


if __name__ == '__main__':
    main()
