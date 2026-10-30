#!/usr/bin/env python3
"""Turn the player's exefs patches for one game version (IPSwitch .pchtxt, e.g. from mods/1.0.4/<name>/exefs/) into checked
patch tables.

Each patch must name main's build ID and touch only bytes the Apple-silicon rewrite left as they were (a rewritten word
would be a branch into a rewrite block). Offsets are main image offsets (the files' `@flag offset_shift 0x100` is for the
IPS container). Writes <work>/patches/<version>/<slug>.tsv ("offset new-bytes original-bytes" in hex) and index.tsv there;
the runtime (runtime/game_patches.c) checks the original bytes again before writing. No game bytes come from the files
beyond the replacement words they already hold.
"""
import argparse
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'third_party/d2r'))
import nsoimg
sys.path.insert(0, str(HERE / 'scripts'))
import versions


def slug(name):
    return re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')


def parse(path, build_id, display_version):
    text = path.read_text(errors='replace').splitlines()
    ids = [l.split('-', 1)[1].strip().upper() for l in text if l.startswith('@nsobid-')]
    if [i.rstrip('0') for i in ids] != [build_id.rstrip('0')]:
        raise ValueError(f'{path}: build ID {ids} is not main {display_version} ({build_id})')
    patches, enabled = [], False
    for n, line in enumerate(text, 1):
        line = line.split('//', 1)[0].strip()
        if line.startswith('@enabled'): enabled = True; continue
        if line.startswith('@disabled') or line.startswith('@stop'): enabled = False; continue
        m = re.fullmatch(r'([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)', line)
        if not m: continue
        if not enabled: continue
        if len(m.group(2)) % 2: raise ValueError(f'{path}:{n}: odd number of hex digits')
        patches.append((int(m.group(1), 16), bytes.fromhex(m.group(2)), n))
    if not patches: raise ValueError(f'{path}: no enabled patch lines')
    return patches


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mods', type=Path, nargs='?', help='folder of <name>/exefs/*.pchtxt (default: mods/<display version>)')
    parser.add_argument('--work', type=Path, default=HERE / 'OT0_build')
    parser.add_argument('--version', required=True, choices=sorted(versions.VERSIONS))
    args = parser.parse_args()
    pinned = versions.expected(args.version)
    mods = args.mods or HERE / 'mods' / pinned['display_version']
    work = args.work.resolve()
    original = nsoimg.load(work / f'extracted/{args.version}/exefs/main')
    layout = {k: int(v, 0) for _, k, v in (l.split() for l in (work / f'rewritten/{args.version}/main.fixups.tsv').read_text().splitlines())}
    rewritten = (work / f'rewritten/{args.version}/main.patched.bin').read_bytes()[layout.get('IMAGE_FILE_OFF', 0):]
    out = work / 'patches' / args.version; out.mkdir(parents=True, exist_ok=True)
    index = []
    for f in sorted(mods.glob('*/exefs/*.pchtxt')):
        name = f.parents[1].name
        patches = parse(f, pinned['main_build_id'], pinned['display_version'])
        rows = []
        for off, new, line in patches:
            end = off + len(new)
            if end > len(original) or end > len(rewritten):
                raise ValueError(f'{f}:{line}: {off:#x}+{len(new)} beyond main')
            if rewritten[off:end] != original[off:end]:
                raise ValueError(f'{f}:{line}: {off:#x} lies on bytes the Apple-silicon rewrite changed; refusing')
            rows.append(f'{off:#x}\t{new.hex()}\t{original[off:end].hex()}')
        (out / f'{slug(name)}.tsv').write_text('# ' + name + ' (' + f.name + ')\n' + '\n'.join(rows) + '\n')
        index.append(f'{slug(name)}\t{name}\t{len(rows)}')
        print(f'{slug(name):24} {len(rows):3} patches  ({name})')
    (out / 'index.tsv').write_text('\n'.join(index) + '\n')


if __name__ == '__main__':
    main()
