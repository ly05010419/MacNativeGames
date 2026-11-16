#!/usr/bin/env python3
"""Exercise private NRO mapping and synthetic relocations without executing code."""
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
from rewrite_nro import verify_nro_report
from survey_modules import data_bounds, load, relocs, validate_relocations
from nsoimg import header
from integration_images import Image, Spec
from integration_relocations import Entry, Resolve, resolve, SYNTHETIC_SYMBOL


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', required=True, type=Path)
    parser.add_argument('--rewritten', required=True, type=Path)
    args = parser.parse_args()
    report = verify_nro_report(args.prepared, args.rewritten)
    layout = header(args.rewritten, 'nro')
    if layout.get('PATCH_LEAD', 0):
        raise ValueError('leading patch layout not supported')
    start, end = data_bounds(args.prepared / 'synthetic.nso')
    if start != layout['DATA_OFF'] or end != layout['KEY_PAGE_OFF']:
        raise ValueError('NRO and rewritten data bounds differ')
    records = relocs(load(args.prepared / 'synthetic.nso', bss=False))
    validate_relocations(records, start, end)
    entries = (Entry * len(records))(*(Entry(off, kind, 1, addend, 0) for off, kind, _, _, addend in records))
    path = args.rewritten / 'nro.patched.bin'
    digest = bytes.fromhex(report['outputs']['patched.bin'])
    spec = Spec(path.stat().st_size, layout['IMAGE_SIZE'], layout['KEY_PAGE_OFF'],
                layout['PATCH_OFF'], layout['PATCH_SIZE'], (ctypes.c_ubyte * 32).from_buffer_copy(digest))
    with tempfile.TemporaryDirectory() as tmp:
        library = Path(tmp) / 'primitives.dylib'
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
        mapped = Image()
        if api.tmnt_image_map(bytes(path), ctypes.byref(spec), ctypes.byref(mapped)):
            raise ValueError('NRO mapping failed')
        try:
            code = hashlib.sha256(ctypes.string_at(mapped.base, start)).digest()
            suffix = hashlib.sha256(ctypes.string_at(mapped.base + end, spec.file_size - end)).digest()
            status = api.tmnt_relocate(mapped.base, spec.image_size, start, end, entries, len(entries), resolve, None)
            if status:
                raise ValueError(f'NRO relocation failed: {status}')
            for off, kind, _, _, addend in records:
                expected = mapped.base + addend if kind == 1027 else SYNTHETIC_SYMBOL + addend
                if ctypes.c_uint64.from_address(mapped.base + off).value != expected:
                    raise ValueError('NRO relocation value mismatch')
            if (hashlib.sha256(ctypes.string_at(mapped.base, start)).digest() != code or
                hashlib.sha256(ctypes.string_at(mapped.base + end, spec.file_size - end)).digest() != suffix):
                raise ValueError('NRO relocation changed code or patch area')
        finally:
            api.tmnt_image_unmap(ctypes.byref(mapped))
    print(f'NRO: {len(records)} synthetic relocations checked; no game code executed')


if __name__ == '__main__':
    main()
