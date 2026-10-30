#!/usr/bin/env python3
"""Rewrite every verified NRO (scripts/extract_nros.py) with the code rewriter, as main is rewritten.

third_party/d2r/patch_nso.py reads NSO files, so each NRO is first written as an uncompressed NSO with the same segments: an
NRO's file layout is its memory layout (text at 0 with the NRO header inside it, then rodata, data; bss after), so only the
0x100-byte NSO header is new and the image patch_nso builds is the NRO's own memory image. The NSO goes to a scratch folder
next to the output; the rewrite of <sha256>.nro is <output>/<sha256>.patched.bin with its .fixups/.gates/.sites/... tables.

    scripts/rewrite_nros.py --nro <work>/extracted/v1302/nro --output <work>/rewritten/v1302/nro [--jobs 6]

Runs the rewriter with the build's variables (DM_RULESET=native DM_PRIVATE_GATES=0); a module that does not rewrite stops
the script with its digest and the rewriter's last line. Two runs give identical bytes (the rewriter is deterministic).
"""
import argparse
import json
import os
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]


def nso_from_nro(nro):
    """Uncompressed NSO bytes with the NRO's segments at their memory offsets."""
    assert nro[0x10:0x14] == b'NRO0', 'not an NRO'
    size = struct.unpack_from('<I', nro, 0x18)[0]
    assert size == len(nro), 'NRO size field differs from the file'
    segs = [struct.unpack_from('<II', nro, 0x20 + 8 * i) for i in range(3)]   # (memory offset, size): text, rodata, data
    bss = struct.unpack_from('<I', nro, 0x38)[0]
    assert segs[0][0] == 0 and segs[1][0] == segs[0][1] and segs[2][0] == segs[1][0] + segs[1][1], 'NRO segments are not contiguous'
    assert segs[2][0] + segs[2][1] == size, 'NRO data segment does not end the file'
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    for i, (mem, length) in enumerate(segs):
        struct.pack_into('<III', hdr, 0x10 + 0x10 * i, 0x100 + mem, mem, length)
        struct.pack_into('<I', hdr, 0x60 + 4 * i, length)
    struct.pack_into('<I', hdr, 0x3c, bss)
    hdr[0x40:0x60] = nro[0x40:0x60]                    # build id
    hdr[0x88:0xa0] = nro[0x68:0x80]                    # api info, dynstr, dynsym (relative to rodata in both formats)
    return bytes(hdr) + nro


def rewrite(nro_path, out, scratch, env):
    digest = nro_path.stem
    nso = scratch / f'{digest}.nso'
    nso.write_bytes(nso_from_nro(nro_path.read_bytes()))
    p = subprocess.run([sys.executable, str(HERE / 'third_party/d2r/patch_nso.py'), str(nso), str(out), digest],
                       cwd=HERE, env=env, capture_output=True, text=True)
    nso.unlink()
    lines = (p.stdout + p.stderr).strip().splitlines()
    return digest, p.returncode, lines[-2] if p.returncode == 0 and len(lines) >= 2 else (lines[-1] if lines else '')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--nro', required=True, type=Path, help='folder of verified <sha256>.nro files with manifest.json')
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    args = ap.parse_args()
    manifest = json.loads((args.nro / 'manifest.json').read_text())
    if manifest.get('status') != 'pass':
        sys.exit('rewrite_nros: the NRO folder is not a complete verified extraction')
    args.output.mkdir(parents=True, exist_ok=True)
    scratch = args.output / 'nso-scratch'; scratch.mkdir(exist_ok=True)
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0')
    paths = [args.nro / f"{m['sha256']}.nro" for m in manifest['modules']]
    failed = []
    with ThreadPoolExecutor(args.jobs) as pool:
        for n, (digest, code, line) in enumerate(pool.map(lambda p: rewrite(p, args.output, scratch, env), paths), 1):
            print(f'{n}/{len(paths)} {digest[:16]} {"ok" if code == 0 else "FAILED"}: {line[:200]}', flush=True)
            if code: failed.append((digest, line))
    scratch.rmdir()
    if failed:
        sys.exit(f'rewrite_nros: {len(failed)} module(s) did not rewrite, first {failed[0][0]}: {failed[0][1]}')
    print(json.dumps(dict(status='pass', modules=len(paths))))


if __name__ == '__main__':
    main()
