#!/usr/bin/env python3
"""Apply a verified game's relocation metadata to zero-filled private test images.

All symbolic imports resolve to a synthetic address. No game code is loaded or executed.
"""
import argparse
import ctypes
import subprocess
import tempfile
from pathlib import Path
import sys

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'scripts'))
from rewrite_modules import MODULES, verify_exefs
from survey_modules import data_bounds, load, relocs, validate_relocations


class Entry(ctypes.Structure):
    _fields_ = [('offset', ctypes.c_uint64), ('kind', ctypes.c_uint32), ('symbol', ctypes.c_uint32),
                ('addend', ctypes.c_int64), ('weak', ctypes.c_int)]


Resolve = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_size_t))
SYNTHETIC_SYMBOL = 0x1000000000


@Resolve
def resolve(context, symbol, output):
    del context, symbol
    output[0] = SYNTHETIC_SYMBOL
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    exefs = parser.parse_args().exefs.resolve()
    verify_exefs(exefs)
    with tempfile.TemporaryDirectory() as temp:
        library = Path(temp) / 'relocate.dylib'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
                        '-I', str(GAME / 'runtime'), str(GAME / 'runtime/relocate.c'),
                        '-o', str(library)], check=True)
        function = ctypes.CDLL(str(library)).tmnt_relocate
        function.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t,
                             ctypes.POINTER(Entry), ctypes.c_size_t, Resolve, ctypes.c_void_p]
        function.restype = ctypes.c_int
        total = 0
        for name in MODULES:
            records = relocs(load(exefs / name, bss=False))
            start, end = data_bounds(exefs / name)
            validate_relocations(records, start, end)
            entries = (Entry * len(records))(*(Entry(off, kind, 1, addend, 0)
                                                for off, kind, _, _, addend in records))
            image = ctypes.create_string_buffer(end)
            address = ctypes.addressof(image)
            status = function(image, end, start, end, entries, len(entries), resolve, None)
            if status:
                raise ValueError(f'{name}: relocation failed with status {status}')
            for off, kind, _, _, addend in records:
                actual = ctypes.c_uint64.from_buffer(image, off).value
                expected = address + addend if kind == 1027 else SYNTHETIC_SYMBOL + addend
                if actual != expected:
                    raise ValueError(f'{name}: relocated value mismatch')
            total += len(records)
            print(f'{name}: {len(records)} synthetic relocations verified', flush=True)
        print(f'total: {total} synthetic relocations verified')


if __name__ == '__main__':
    main()
