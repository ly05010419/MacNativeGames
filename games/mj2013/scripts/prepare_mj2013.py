#!/usr/bin/env python3
"""Prepare 明星三缺一 2013 (title 01004D4A53510000) for the Zelda-line host from the player's own NSP.

Steps (each skipped when its output is there and checks out):
  1. ExeFS and RomFS out of the NSP with the player's prod.keys (games/pla/scripts/extract_exefs.py / extract_romfs.py,
     which take any title by id) into <work>/extracted/v0/{exefs,romfs}.
  2. The ExeFS is checked against assets/expected.json (this game's main and main.npdm; the RomFS image's size and hash).
  3. main is rewritten ahead of time with the Zelda line's rewriter (third_party/d2r -> games/totk/third_party/d2r,
     DM_RULESET=native DM_PRIVATE_GATES=0) into <work>/rewritten/v0.
  4. <work>/build/runtime_tables.h: the rewritten module's layout, digest and host gates, the title id, the main thread's
     stack size (main.npdm) and the RomFS image size, for runtime/loader.c, svc_memory.c and hb_fs.c.

Nothing of the game goes into the repository: the work folder is outside the checkout."""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
REPO = HERE.parents[1]
EXPECTED = json.loads((HERE / 'assets/expected.json').read_text())
TITLE = EXPECTED['title_id']


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def extract(work, game, keys, py):
    v0 = work / 'extracted/v0'
    scripts = REPO / 'games/pla/scripts'
    if not (v0 / 'exefs/main').exists():
        if not game or not keys:
            sys.exit('extracted/v0/exefs is missing: give --game <the NSP> and --keys <prod.keys>')
        tmp = work / 'extracted/v0-tmp'
        subprocess.run([py, str(scripts / 'extract_exefs.py'), str(game), '--keys', str(keys), '--title', TITLE,
                        '--output', str(tmp / 'exefs')], check=True)
        v0.mkdir(parents=True, exist_ok=True)
        (tmp / 'exefs').rename(v0 / 'exefs')
        (tmp / 'extraction-manifest.json').rename(v0 / 'extraction-manifest.json')
        tmp.rmdir()
    if not (v0 / 'romfs/romfs.bin').exists():
        if not keys:
            sys.exit('extracted/v0/romfs is missing: give --keys <prod.keys>')
        subprocess.run([py, str(scripts / 'extract_romfs.py'), '--keys', str(keys), '--title', TITLE,
                        '--manifest', str(v0 / 'extraction-manifest.json'), '--output', str(v0 / 'romfs')], check=True)


def check_game(work):
    v0 = work / 'extracted/v0'
    for name, digest in EXPECTED['exefs_sha256'].items():
        if sha(v0 / 'exefs' / name) != digest:
            sys.exit(f'exefs/{name} is not 明星三缺一 2013 as prepared here (SHA-256 differs)')
    index = json.loads((v0 / 'romfs/index.json').read_text())
    if index['romfs_sha256'] != EXPECTED['romfs_sha256'] or (v0 / 'romfs/romfs.bin').stat().st_size != EXPECTED['romfs_size']:
        sys.exit('romfs/romfs.bin differs from this game\'s RomFS')
    return index


def rewrite(work, py):
    out = work / 'rewritten/v0'
    image = out / 'main.patched.bin'
    if image.exists() and sha(image) == EXPECTED['rewritten_sha256']:
        return
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0')
    subprocess.run([py, str(HERE / 'third_party/d2r/patch_nso.py'), str(work / 'extracted/v0/exefs/main'), str(out), 'main'],
                   check=True, env=env, cwd=str(HERE / 'third_party/d2r'))
    got = sha(image)
    if got != EXPECTED['rewritten_sha256']:
        sys.exit(f'the rewrite of main differs from the verified one ({got}); the rewriter changed: check it, then the pin')


def layout(work):
    values = {}
    for line in (work / 'rewritten/v0/main.fixups.tsv').read_text().splitlines():
        _, key, value = line.split()
        values[key] = int(value, 0)
    return values


def gates(work):
    rows = []
    for line in (work / 'rewritten/v0/main.gates.tsv').read_text().splitlines():
        ret, kind, arg, original = line.split()
        if kind not in ('cntpct', 'cntvct', 'svc'):
            raise ValueError(f'gate kind {kind}')
        rows.append((int(ret, 0), int(original, 0), int(kind != 'svc'), int(arg)))
    return rows


def npdm_stack(work):
    data = (work / 'extracted/v0/exefs/main.npdm').read_bytes()
    if data[:4] != b'META':
        raise ValueError('main.npdm: no META header')
    return struct.unpack_from('<I', data, 0x1c)[0]


def stdnull_devoptab(work, l):
    """libnx's dotab_stdnull (devoptab_t: name, struct size, open_r, close_r, write_r, ...), which stdout and stderr use when
    the game sets up no console: the data word whose (unrelocated) value is the offset of the string "stdnull". The loader
    points its write_r at a host function, so what the game prints reaches the log. 0 when there is none."""
    image = (work / 'rewritten/v0/main.patched.bin').read_bytes()
    name = image.find(b'stdnull\x00', l['RO_OFF'], l['DATA_OFF'])
    if name < 0:
        return 0
    hits = [o for o in range(l['DATA_OFF'], l['KEY_PAGE_OFF'] - 0x28, 8)
            if struct.unpack_from('<Q', image, o)[0] == name and struct.unpack_from('<Q', image, o + 0x20)[0]]
    return hits[0] if len(hits) == 1 else 0


def tables(work):
    l = layout(work)
    image = work / 'rewritten/v0/main.patched.bin'
    g = gates(work)
    lines = ['/* Generated by scripts/prepare_mj2013.py from the player\'s game; do not commit. */',
             f'#define GAME_TITLE_ID 0x{TITLE}ULL',
             f'#define GAME_MAIN_STACK_SIZE {npdm_stack(work):#x}u',
             f'#define ROMFS_SIZE {EXPECTED["romfs_size"]}ULL',
             f'#define GUEST_STDNULL_DEVOPTAB {stdnull_devoptab(work, l):#x}ULL',
             'static const Gate gate_0[] = {']
    lines += [f'{{{ret:#x}ULL,{orig:#x}ULL,{timer},{arg}}},' for ret, orig, timer, arg in g]
    lines.append('};')
    lines.append('static Module modules[] = {{' + f'.name="main",.digest="{sha(image)}",.file_size={image.stat().st_size}ULL,'
                 f'.text_end={l["TEXT_END"]:#x}ULL,.ro_off={l["RO_OFF"]:#x}ULL,.data_off={l["DATA_OFF"]:#x}ULL,'
                 f'.patch_off={l["PATCH_OFF"]:#x}ULL,.key_off={l["KEY_PAGE_OFF"]:#x}ULL,.gates=gate_0,.ngate={len(g)}' + '}};')
    build = work / 'build'
    build.mkdir(parents=True, exist_ok=True)
    (build / 'runtime_tables.h').write_text('\n'.join(lines) + '\n')
    return dict(gates=len(g), layout={k: hex(v) for k, v in l.items()})


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, required=True, help='work folder outside the checkout')
    p.add_argument('--game', type=Path, help='the player\'s NSP (only for the first extraction)')
    p.add_argument('--keys', type=Path, help='the player\'s prod.keys (only for the first extraction)')
    p.add_argument('--py', default=sys.executable, help='Python with lz4, capstone and cryptography')
    a = p.parse_args()
    work = a.work.resolve()
    if REPO in work.parents or work == REPO:
        sys.exit('the work folder holds game data: keep it outside the checkout')
    extract(work, a.game, a.keys, a.py)
    check_game(work)
    rewrite(work, a.py)
    print(json.dumps(dict(status='pass', **tables(work))))


if __name__ == '__main__':
    main()
