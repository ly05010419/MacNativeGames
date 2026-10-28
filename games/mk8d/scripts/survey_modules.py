#!/usr/bin/env python3
"""Verify Mario Kart 8 Deluxe NSO segments and inventory imports using the existing analyzer.

Uses the local liblz4 when Python's lz4 wheel is unavailable. Generated ELFs are
analysis carriers and cannot be launched as macOS programs.
"""
import argparse
import ctypes
import ctypes.util
import importlib.util
import json
import sys
import types
from pathlib import Path


def decompress(payload, uncompressed_size):
    lib = ctypes.CDLL(ctypes.util.find_library('lz4') or '/opt/homebrew/lib/liblz4.dylib')
    fn = lib.LZ4_decompress_safe
    fn.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    fn.restype = ctypes.c_int
    out = ctypes.create_string_buffer(uncompressed_size)
    src = ctypes.create_string_buffer(payload)
    length = fn(src, out, len(payload), uncompressed_size)
    if length != uncompressed_size:
        raise ValueError('LZ4 decompression size mismatch')
    return out.raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exefs', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    shim = types.ModuleType('lz4')
    block = types.ModuleType('lz4.block')
    block.decompress = decompress
    shim.block = block
    sys.modules['lz4'] = shim
    sys.modules['lz4.block'] = block
    script = Path(__file__).resolve().parents[1] / 'third_party/by3-scripts/prepare_modules.py'
    spec = importlib.util.spec_from_file_location('prepare_modules', script)
    analyzer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(analyzer)
    args.output.mkdir(parents=True, exist_ok=True)
    summary = []
    for name in ('rtld', 'main', 'sdk', 'subsdk0'):
        result = analyzer.prepare(args.exefs / name, args.output / (name + '.elf'))
        (args.output / (name + '-inventory.json')).write_text(json.dumps(result, indent=2) + '\n')
        summary.append({'module': name, 'build_id': result['build_id'],
                        'imports': len(result['imports']), 'exports': len(result['exports']),
                        'needed': result['needed'], 'text_bytes': result['segments'][0]['size'],
                        'svc_candidates': len(result['svc_instruction_candidates']),
                        'segment_hashes_verified': result['segment_hashes_verified']})
    (args.output / 'modules.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
