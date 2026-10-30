#!/usr/bin/env python3
"""Disassemble a range of an original module image, naming calls through the PLT (adrp/ldr/br stubs whose GOT slot
carries a symbol relocation) and calls to the module's own exported functions.

usage: disasm.py <module>+0x<offset> [count]        e.g. disasm.py main+0x274680c 48
Reads <work>/analysis/runtime-layout.json (scripts/prepare_runtime.py) and the original images from extracted/v110/exefs.
"""
import bisect, json, struct, subprocess, sys
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'third_party/d2r'))
import nsoimg

work = HERE / 'EoW_build'
layout = {m['name']: m for m in json.loads((work / 'analysis/runtime-layout.json').read_text())}


def demangle(name):
    try: return subprocess.run(['c++filt', name], capture_output=True, text=True).stdout.strip() or name
    except OSError: return name


def tables(mod):
    m = layout[mod]
    got = {r['where']: m['symbols'][r['symbol']]['name'] for r in m['relocs'] if r['kind'] in (1025, 1026) and r['symbol']}
    funcs = sorted((s['value'], s['name']) for s in m['symbols'] if s['defined'] and s['type'] == 2 and s['value'])
    return got, funcs


def plt_target(image, address, got):
    if address + 12 > len(image): return None
    a, b, _, d = struct.unpack_from('<IIII', image, address)
    if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400211 or d != 0xd61f0220: return None
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    imm = (imm ^ (1 << 20)) - (1 << 20)
    return got.get((address & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8)


def main():
    mod, off = sys.argv[1].split('+')
    off, count = int(off, 16), int(sys.argv[2]) if len(sys.argv) > 2 else 40
    image = nsoimg.load(str(work / f'extracted/v110/exefs/{mod}'))
    got, funcs = tables(mod)
    starts = [f[0] for f in funcs]
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for insn in cs.disasm(bytes(image[off:off + 4 * count]), off):
        note = ''
        if insn.mnemonic in ('bl', 'b') and insn.op_str.startswith('#'):
            target = int(insn.op_str[1:], 16)
            name = plt_target(image, target, got)
            if not name:
                i = bisect.bisect_right(starts, target) - 1
                if i >= 0 and funcs[i][0] == target: name = funcs[i][1]
            if name: note = '  ; ' + demangle(name)
        print(f'{mod}+{insn.address:#x}: {insn.mnemonic} {insn.op_str}{note}')


if __name__ == '__main__':
    main()
