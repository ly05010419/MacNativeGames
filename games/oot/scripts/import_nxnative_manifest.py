#!/usr/bin/env python3
"""Render pipelines recorded by seedds/nxnative (MIT) as rows of our shared shader list.

nxnative's dm/games/0400486024DEC000/pso_manifest.tsv was recorded over a first start on the Mac (boot, the title and its
movie, a new game and the opening): routes our own recordings had not covered. An R row is a hex pso_skey, the same
structure as ours (both come from d2r-mobile) with shorter shader names (24 bytes against 80) and three additions at the
end of its pso_key: atest, logic and attr_hi. Verified on the 2026-11-09 manifest (main 79741184af15): 636 of the 1,112
converted rows are byte for byte rows of our own pso_user.tsv.

A row is left out, with a reason, when it needs state this runtime's key cannot hold, or when the seed has no default
variant of its shaders. Its names become the default variant, the only one nxnative's names can stand for; on a first
start the game asks for those (fs1, 2026-11-09: 64 of 66 mesh shaders of the mesh pipelines). R and M rows are converted:
every compute variant is already planned from the seed (prepare_compute_pipelines.py), and warming a pipeline loads its
shaders (fill_row, warm_build_row), so C and S rows add nothing.

    gh api repos/seedds/nxnative/contents/dm/games/0400486024DEC000/pso_manifest.tsv -H 'Accept: application/vnd.github.raw' > pso_manifest.tsv
    import_nxnative_manifest.py pso_manifest.tsv <shader-seed dir> rows.tsv
    shader_list.py merge ../assets/shader_list.tsv rows.tsv
"""
import collections
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shader_list import DEFAULT_SUFFIX

NX_KEY, OUR_KEY = 504, 544
NX_NAME, OUR_NAME = 24, 80
HEADER = 4 + 34                     # vs_stage, fs_stage, merged, fz; pso_fz_desc
NX_STATE, OUR_STATE = 88, 200       # offset of pso_key, after two bytes of padding
STATE = 344                         # our pso_key; nxnative's goes on with atest, logic, attr_hi[16]
A2C, BLEND, BLEND_BITS = 68, 72, 37  # pso_key.a2c, blend[8] (8 bytes each), the bits our blend_st defines
ATEST, ATTR_HI = 344, 352
NAME = re.compile(r'^[0-9a-f]{16}_b$')


class Skip(Exception):
    """The row cannot be expressed as one of our keys."""


def seed_names(seed):
    """Short key -> full key, of the shaders whose default variant the seed holds."""
    seed = Path(seed)
    names = {}
    for index in seed.glob('*.variants'):
        key = index.stem
        if (seed / f'{key}-{DEFAULT_SUFFIX}_b.metal').exists():
            if key[:16] in names:
                raise ValueError(f'two seed shaders share the short key {key[:16]}')
            names[key[:16]] = key
    return names


def our_name(raw, names):
    """nxnative's NUL-padded name as our NUL-padded default-variant name; no shader stays empty."""
    text = raw.split(b'\0')[0].decode()
    if not text:
        return bytes(OUR_NAME)
    if not NAME.match(text) or text[:16] not in names:
        raise Skip('shader not in the seed')
    return f'{names[text[:16]]}-{DEFAULT_SUFFIX}_b'.encode().ljust(OUR_NAME, b'\0')


def convert_key(hexkey, names):
    """The hex of our 544-byte key for one of nxnative's, or Skip."""
    b = bytes.fromhex(hexkey)
    if len(b) != NX_KEY:
        raise Skip('key length')
    state = b[NX_STATE:]
    if int.from_bytes(state[ATEST:ATEST + 8], 'little'):
        raise Skip('alpha test or logic operation')
    if any(state[ATTR_HI:]):
        raise Skip('vertex attributes above 15')
    if any(int.from_bytes(state[BLEND + 8 * i:BLEND + 8 * i + 8], 'little') >> BLEND_BITS for i in range(8)):
        raise Skip('advanced blending')
    if int.from_bytes(state[A2C:A2C + 4], 'little'):
        raise Skip('a2c bits')   # its upper bits mean other things here (layered, MetalFX roles, HDR)
    key = (our_name(b[:NX_NAME], names) + our_name(b[NX_NAME:2 * NX_NAME], names) + b[2 * NX_NAME:2 * NX_NAME + HEADER]
           + b'\0\0' + state[:STATE])
    if len(key) != OUR_KEY:
        raise ValueError('converted key has the wrong length')
    return key.hex()


def task_name(field, names):
    """An M row's task shader as our default-variant name; '-' stays."""
    if field == '-':
        return field
    if not NAME.match(field) or field[:16] not in names:
        raise Skip('shader not in the seed')
    return f'{names[field[:16]]}-{DEFAULT_SUFFIX}_b'


def convert(manifest, names):
    """(rows, reasons left out) for every R and M row of a manifest, as rows of the shared list."""
    rows, skipped = [], collections.Counter()
    for line in Path(manifest).read_text().splitlines():
        fields = line.split('\t')
        try:
            if fields[0] == 'R':
                rows.append(f'R\t{convert_key(fields[1], names)}')
            elif fields[0] == 'M':
                rows.append(f'M\t{task_name(fields[1], names)}\t{convert_key(fields[2], names)}')
        except Skip as why:
            skipped[str(why)] += 1
    return rows, skipped


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    names = seed_names(argv[1])
    rows, skipped = convert(argv[0], names)
    Path(argv[2]).write_text('# pipelines from seedds/nxnative pso_manifest.tsv (MIT), converted by import_nxnative_manifest.py\n'
                             + ''.join(row + '\n' for row in rows))
    counts = collections.Counter(row[0] for row in rows)
    print(f'{counts["R"]} R and {counts["M"]} M rows from {len(names)} seed shaders; left out: {dict(skipped)}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
