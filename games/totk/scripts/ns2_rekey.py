#!/usr/bin/env python3
"""Move a Switch 2 game's shader list to the shader keys of 2026-11-12 (nvn_ctl.h ctl_key_update, ns2_shader_corpus.py
shader_key): the key now hashes control +0x08..+0x0b and +0x20..+0x27 as zero too, as it did +0x30, because Pokopia writes
them again in memory and the translation reads none of them. Every key changes, so the list's rows are renamed: a V row's
shader, the variants an S / C / R / B / T row names ("<key>-<suffix>_b"), an L row's shader. The old key of each shader
comes from its bytes: every shader of the game files (the corpus's scan of the RomFS and the modules) and the shaders a
runtime dumped (TOTK_SHADER_DUMP_UNSEEDED: <key>.ctl and <key>.code, keyed by the old rule) for the ones in no game file.
A row naming a shader neither knows cannot be renamed and is left out (reported). A rows (bind states) stay.

usage: ns2_rekey.py --romfs <RomFS> --modules <ELF ...> [--dump <folder> ...] --list <shader_list.tsv> [--apply]
"""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import struct
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from shader_list import ShaderList, rename_warm, row_variants  # noqa: E402
import ns2_shader_corpus as corpus  # noqa: E402  (imported by name: the scan's worker processes find its functions so)


def old_key(ctl, code):
    """The key before 2026-11-12: only +0x30 hashed as zero."""
    k = bytearray(ctl[:0x48])
    k[0x30:0x34] = bytes(4)
    return hashlib.sha256(bytes(k) + code).hexdigest()


def renames_from(found):
    """{old key: new key} of (control, code) pairs."""
    return {old_key(ctl, code): corpus.shader_key(ctl, code) for ctl, code in found}


def scan(romfs, modules, jobs):
    """(control, code) of every shader in the game files, as the corpus finds them (all of them, not only the listed)."""
    src = corpus.Source(romfs)
    rdbs = sorted(f for f in src.files if f.endswith(('.fdata', '.file')))
    link = ('LI_C0_R.IDX', 'LD_C0_R.BIN') if {'LI_C0_R.IDX', 'LD_C0_R.BIN'} <= src.files.keys() else None
    nintendo = sorted(f for f in src.files if f.startswith(('Shader/', 'Lib/', 'Effect/', 'Layout/'))
                      and f.endswith(('.zs', '.bnsh', '_nvn2_shader.bin')))
    found = []
    with concurrent.futures.ProcessPoolExecutor(jobs) as ex:
        work = [ex.submit(corpus.scan_module, m) for m in modules] + [ex.submit(corpus.scan_rdb, romfs, f) for f in rdbs]
        work += [ex.submit(corpus.scan_nintendo, romfs, f) for f in nintendo]
        if link:
            work.append(ex.submit(corpus.scan_linkdata, romfs, *link))
        for j in concurrent.futures.as_completed(work):
            _, _, shaders = j.result()   # a file the corpus cannot read stops this, as it stops the corpus
            found += [(ctl, code) for _, _, ctl, code, _ in shaders]
    return found


def dumped(folder):
    """(control, code) of the shaders a runtime dumped, checked against the old key they are named by."""
    out = []
    for ctl_path in sorted(Path(folder).glob('*.ctl')):
        ctl, code = ctl_path.read_bytes(), ctl_path.with_suffix('.code').read_bytes()
        if old_key(ctl, code) != ctl_path.stem:
            raise SystemExit(f'{ctl_path}: its bytes do not hash to its name under the old key rule')
        out.append((ctl, code))
    return out


def rename_variant(name, renames):
    """"<old key>-<suffix>_b" with the new key, or None when the old key is unknown."""
    new = renames.get(name[:64])
    return new + name[64:] if new else None


def rekey(shared, renames):
    """Rename every row of a ShaderList in place; returns {kind: rows left out}."""
    lost = {'V': 0, 'L': 0}
    variants = set()
    for key, deps in shared.variants:
        if key in renames:
            variants.add((renames[key], deps))
        else:
            lost['V'] += 1
    shared.variants = variants
    warm = {}
    for row, flags in shared.warm.items():
        names = {n: rename_variant(n, renames) for n in row_variants(row) if n}
        if not all(names.values()):
            lost[row[0]] = lost.get(row[0], 0) + 1
            continue
        new = rename_warm(row, names)
        warm[new] = ''.join(f for f in 'tm' if f in warm.get(new, '') + flags)
    shared.warm = warm
    locators = set()
    for key, *rest in shared.locators:
        if key in renames:
            locators.add((renames[key], *rest))
        else:
            lost['L'] += 1
    shared.locators = locators
    return lost


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--modules', type=Path, nargs='+', required=True)
    ap.add_argument('--dump', type=Path, action='append', default=[], help="a runtime's TOTK_SHADER_DUMP_UNSEEDED folder")
    ap.add_argument('--list', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--apply', action='store_true', help='write the renamed list (its header kept, its key rule line set)')
    a = ap.parse_args()
    found = scan(a.romfs, a.modules, a.jobs)
    files = renames_from(found)
    extra = renames_from(sum((dumped(d) for d in a.dump), []))
    renames = {**extra, **files}
    print(f'{len(files)} shaders in the game files, {len(set(extra) - set(files))} more from the dumps; '
          f'{sum(1 for o, n in renames.items() if o != n)} keys change')
    shared = ShaderList.load(a.list)
    before = (len(shared.variants), len(shared.warm), len(shared.locators))
    lost = rekey(shared, renames)
    after = (len(shared.variants), len(shared.warm), len(shared.locators))
    print(f'{a.list}: variants {before[0]} -> {after[0]}, warm rows {before[1]} -> {after[1]}, locators {before[2]} -> {after[2]}; '
          f'left out (a shader no file or dump names): {dict(sorted((k, v) for k, v in lost.items() if v))}')
    if not a.apply:
        print('dry run: nothing written (--apply writes the list)')
        return
    old = a.list.read_text().splitlines(keepends=True)
    header = ''.join(line for line in old[:next((i for i, l in enumerate(old) if not l.startswith('#')), len(old))]
                     if not line.startswith(corpus.KEY_RULE_HEADER)) + f'{corpus.KEY_RULE_HEADER}{corpus.KEY_RULE}\n'
    shared.save(a.list)
    body = [line for line in a.list.read_text().splitlines(keepends=True) if not line.startswith('#')]
    a.list.write_text(header + ''.join(body))
    print(f'written: {a.list}')


if __name__ == '__main__':
    main()
