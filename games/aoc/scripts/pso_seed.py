#!/usr/bin/env python3
"""Merge recorded pipeline lists (DM_PSO_RECORD, pso_user.tsv) into the bundled warm-up seed, pso_manifest.tsv.

The host skips some unusable rows without a word (runtime/gfx/nvn/nvn_mtl.m warm_worker), so every row is checked
here and the first bad one stops the merge with its file and line: S <key> <stage 0-5>, R <pso_skey hex>, C <key>,
a key being 16 lowercase hex digits and "_b". An R row is the host's pso_skey (nvn_pso_skey.h) as lowercase hex of
exactly its size, which scripts/pso_skey_layout.m reports from the header; its vertex key must be a key, its
fragment key a key or empty, the bytes after each key zero, the stages 0-5 and the merged and fz flags 0 or 1.
Rows keep each file's own order, the files taken in the order given (independent sessions have no common clock, so
there is no global order of first use); repeats are dropped, and so is an R row whose key, normalized as the host does (pso_key_norm
in nvn_pso_norm.h: no colour state a pipeline does not use), repeats an earlier one; lines starting with # are skipped. The header names
every input (its last two path parts, no local folders) with its SHA-256, in that order.
More rows than --max-lines allows is an error and nothing is written: the seed is never cut to fit.
With --shader-map (a built shader_map.tsv), every key of every row, an R row's vertex and fragment keys included, must also
be one of its shaders: the host would otherwise skip that row while warming up.
Copied from games/la/scripts/pso_seed.py (Link's Awakening); only the header's script path, --shader-map and the title differ.

  pso_seed.py --output <new pso_manifest.tsv> [--shader-map <shader_map.tsv>] <recording> [<recording> ...]"""
import argparse
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
NVN = GAME / 'runtime/gfx/nvn'
KEY = re.compile(r'[0-9a-f]{16}_b')
HEX = re.compile(r'[0-9a-f]+')
HEADER = ('# Prewarm seed list (shipped in the app, next to shader_map.tsv). Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex) / C<TAB>compute shader key.\n'
          '# Recorded on the Mac (DM_PSO_RECORD, pso_user.tsv), merged by games/aoc/scripts/pso_seed.py: each file in its own order, the files in this order:\n')


def sources(paths):
    """One header line per input: its place, session folder and name, and SHA-256."""
    return ''.join(f'# source {i} {Path(p).parent.name}/{Path(p).name} sha256 {hashlib.sha256(Path(p).read_bytes()).hexdigest()}\n' for i, p in enumerate(paths, 1))


class SeedError(ValueError):
    pass


def layout():
    """pso_skey's size and the offsets pso_seed needs, from the header itself."""
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / 'layout'
        subprocess.run(['clang', '-arch', 'arm64', '-fobjc-arc', f'-I{NVN}', f'-I{NVN}/include', str(GAME / 'scripts/pso_skey_layout.m'),
                        '-framework', 'Foundation', '-o', str(exe)], check=True, capture_output=True)
        size, vs, fs, keylen, vs_stage, fs_stage, merged, fz, cfmt, blend_en, chmask, blend, blend_size = map(int, subprocess.check_output([str(exe)], text=True).split())
    return dict(size=size, vs=vs, fs=fs, keylen=keylen, vs_stage=vs_stage, fs_stage=fs_stage, merged=merged, fz=fz, cfmt=cfmt, blend_en=blend_en,
                chmask=chmask, blend=blend, blend_size=blend_size)


def norm(b, L):
    """pso_key_norm on a row's key: no blend bits or mask for a normal pass's absent attachments, no factors with blending off."""
    b = bytearray(b); cfmt = struct.unpack_from('<8I', b, L['cfmt'])
    be, cm = struct.unpack_from('<I', b, L['blend_en'])[0], struct.unpack_from('<I', b, L['chmask'])[0]
    for i in range(8):
        if not b[L['merged']] and not cfmt[i]: be &= ~(1 << i); cm &= ~(0xf << (4 * i))
        if not be >> i & 1: b[L['blend'] + L['blend_size'] * i:L['blend'] + L['blend_size'] * (i + 1)] = bytes(L['blend_size'])
    struct.pack_into('<I', b, L['blend_en'], be); struct.pack_into('<I', b, L['chmask'], cm)
    return bytes(b)


def shader_key(raw, empty_ok):
    text, _, rest = raw.partition(b'\0')
    if any(rest): return 'bytes after the key are not zero'
    if not text and empty_ok: return None
    return None if KEY.fullmatch(text.decode('ascii', 'replace')) else f'not a shader key: {text!r}'


def shader_map_keys(path):
    """The 16-digit keys of the translated shaders (shader_map.tsv rows whose first column names a .bin)."""
    return {c[3] for line in Path(path).read_text().splitlines() if len(c := line.split('\t')) == 4 and c[0].endswith('.bin')}


def row_keys(row, L):
    """The shader keys a good row names, without their _b."""
    c = row.split('\t')
    if c[0] in ('S', 'C'): return [c[1][:16]]
    b = bytes.fromhex(c[1]); k = L['keylen']
    return [x.partition(b'\0')[0].decode()[:16] for x in (b[L['vs']:L['vs'] + k], b[L['fs']:L['fs'] + k]) if x.partition(b'\0')[0]]


def check(row, L):
    """None for a good row, or what is wrong with it."""
    c = row.split('\t')
    if c[0] in ('S', 'C') and len(c) == (3 if c[0] == 'S' else 2):
        if not KEY.fullmatch(c[1]): return f'not a shader key: {c[1]!r}'
        if c[0] == 'S' and c[2] not in '0 1 2 3 4 5'.split(): return f'stage not 0-5: {c[2]!r}'
        return None
    if c[0] == 'R' and len(c) == 2:
        if len(c[1]) != 2 * L['size'] or not HEX.fullmatch(c[1]): return f'not {2 * L["size"]} lowercase hex digits ({len(c[1])} characters)'
        b = bytes.fromhex(c[1]); k = L['keylen']
        why = shader_key(b[L['vs']:L['vs'] + k], False) or shader_key(b[L['fs']:L['fs'] + k], True)
        if why: return why
        if b[L['vs_stage']] > 5 or b[L['fs_stage']] > 5: return 'stage not 0-5'
        if b[L['merged']] > 1 or b[L['fz']] > 1: return 'merged or fz flag not 0 or 1'
        return None
    return f'not an S, R or C row with its columns: {row[:40]!r}'


def merge(paths, L, known=None):
    rows, seen = [], set()
    for path in paths:
        lines = Path(path).read_text().split('\n')
        if lines and lines[-1] == '': lines.pop()
        for n, row in enumerate(lines, 1):
            if row.startswith('#'): continue
            why = check(row, L) if row else 'empty line'
            if not why and known is not None:
                missing = [k for k in row_keys(row, L) if k not in known]
                if missing: why = f'shader key not in the shader map: {missing[0]}_b'
            if why: raise SeedError(f'{path}:{n}: {why}')
            key = ('R', norm(bytes.fromhex(row[2:]), L)) if row.startswith('R\t') else row
            if key not in seen: seen.add(key); rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-lines', type=int, default=1500, help='header included (the agreed exception for this file)')
    parser.add_argument('--shader-map', type=Path, help="a built shader_map.tsv: every row's keys must be among its shaders")
    parser.add_argument('recordings', nargs='+')
    args = parser.parse_args()
    if args.output.exists(): parser.error(f'{args.output} exists: write a new file')
    try: rows = merge(args.recordings, layout(), shader_map_keys(args.shader_map) if args.shader_map else None)
    except SeedError as error: sys.exit(f'error: {error}')
    head = HEADER + sources(args.recordings); total = head.count('\n') + len(rows)
    if total > args.max_lines: sys.exit(f'error: {total} lines, more than {args.max_lines}: nothing written (ask before raising the limit; never cut the list)')
    args.output.write_text(head + ''.join(r + '\n' for r in rows))
    kinds = {k: sum(r.startswith(k + '\t') for r in rows) for k in 'SRC'}
    print(f'{args.output}: {total} lines; S {kinds["S"]}, R {kinds["R"]}, C {kinds["C"]}')


if __name__ == '__main__':
    main()
