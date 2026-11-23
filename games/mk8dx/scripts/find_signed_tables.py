#!/usr/bin/env python3
"""List the `br x18` jump tables of a module that third_party/d2r/patch_nso.py's ldrsw_table does not recognise, as
SIGNED_TABLES rows (ADR base, table, offsets, dispatch bytes) to review and register for that build.

    python3 scripts/find_signed_tables.py <exefs module> [--only 0x1ae1f0,...]

The shape found: `adrp xT; add xT, xT, #lo` anywhere before the dispatch in the function (xT written by nothing else in
between), `cmp wI|wS, #n; b.hi|b.hs`, `ldrsw xE, [xT, xI, lsl #2]`, `add x18, xE, xT` and then any instructions that do
not write x18 before `br x18`. Every target must be an instruction of the same function; anything else is reported and
left out. The rows still need a human look: the rewriter trusts them (it re-checks the bytes and the entries)."""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/d2r'))
import nsoimg  # noqa: E402
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM  # noqa: E402


def reg(name):
    name = name.strip()
    if name in ('xzr', 'wzr', 'sp', 'wsp'):
        return None
    if name[0] in 'xw' and name[1:].isdigit():
        return int(name[1:])
    return None


def ops(p):
    return [o.strip() for o in p.op_str.replace('[', '').replace(']', '').replace('!', '').split(',')]


NO_WRITE = {'str', 'stur', 'stp', 'strb', 'strh', 'cmp', 'cmn', 'tst', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz',
            'tbz', 'tbnz', 'nop'}


def writes(p, r):
    if p.mnemonic in NO_WRITE or p.mnemonic.startswith('b.'):
        return False
    o = ops(p)
    return reg(o[0]) == r


def analyse(img, text_end, starts, at, md):
    import bisect
    i = bisect.bisect_right(starts, at) - 1
    fa, fb = starts[i], starts[i + 1] if i + 1 < len(starts) else text_end
    insl = list(md.disasm(bytes(img[fa:at + 4]), fa))
    j = len(insl) - 1
    assert insl[j].address == at and insl[j].op_str == 'x18'
    k = j - 1
    while k >= 0 and not writes(insl[k], 18):
        k -= 1
    p = insl[k]
    if p.mnemonic != 'add' or len(ops(p)) != 3:
        return None, 'no add x18'
    a, b = reg(ops(p)[1]), reg(ops(p)[2])
    k2 = k - 1
    while k2 >= 0:
        q = insl[k2]
        if q.mnemonic == 'ldrsw' and 'lsl #2' in q.op_str and len(ops(q)) == 4 and {reg(ops(q)[0]), reg(ops(q)[1])} == {a, b}:
            break
        if writes(q, a) or writes(q, b):
            return None, 'entry register written'
        k2 -= 1
    else:
        return None, 'no ldrsw'
    E, T, I = reg(ops(insl[k2])[0]), reg(ops(insl[k2])[1]), reg(ops(insl[k2])[2])
    k3 = k2 - 1
    while k3 >= 0 and not writes(insl[k3], T):
        k3 -= 1
    q = insl[k3]
    if q.mnemonic != 'add' or reg(ops(q)[1]) != T or not ops(q)[2].startswith('#'):
        return None, f'table base not add ({q.mnemonic} {q.op_str})'
    lo = int(ops(q)[2][1:], 0)
    k4 = k3 - 1
    while k4 >= 0 and not writes(insl[k4], T):
        k4 -= 1
    q = insl[k4]
    if q.mnemonic != 'adrp':
        return None, 'no adrp'
    table = q.operands[1].imm + lo
    src = {I}
    k5 = k2 - 1
    while k5 >= 0 and insl[k5].mnemonic not in ('b.hi', 'b.hs'):
        q = insl[k5]
        if q.mnemonic == 'mov' and reg(ops(q)[0]) in src and reg(ops(q)[1]) is not None:
            src = {reg(ops(q)[1])}
        elif q.mnemonic in ('sub', 'add') and reg(ops(q)[0]) in src and ops(q)[2].startswith('#'):
            pass   # the index is derived after the bound check: not this shape
        k5 -= 1
    k6 = k5 - 1
    while k6 >= 0 and insl[k6].mnemonic != 'cmp':
        k6 -= 1
    q = insl[k6]
    if reg(ops(q)[0]) not in src | {I} or not ops(q)[1].startswith('#'):
        return None, f'bound not on the index ({q.op_str})'
    n = int(ops(q)[1][1:], 0) + (1 if insl[k5].mnemonic == 'b.hi' else 0)
    entries = struct.unpack_from('<' + 'i' * n, img, table)
    targets = [table + e for e in entries]
    if not all(fa <= t < fb and t % 4 == 0 for t in targets):
        return None, 'a target outside the function'
    return (table, table, entries, bytes(img[at - 16:at + 4]).hex()), None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('module', type=Path)
    args = ap.parse_args()
    raw = args.module.read_bytes()
    img = nsoimg.load(str(args.module))
    text_end = struct.unpack_from('<I', raw, 0x18)[0]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    starts = sorted(nsoimg.func_starts(img, text_end))
    print(f"# build {raw[0x40:0x50].hex()}")
    for at in range(0, text_end, 4):
        if struct.unpack_from('<I', img, at)[0] == 0xd61f0240:   # br x18
            row, why = analyse(img, text_end, starts, at, md)
            print(f"    {at:#x}: {row}," if row else f"    # {at:#x}: not found ({why})")


if __name__ == '__main__':
    main()
