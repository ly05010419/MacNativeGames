#!/usr/bin/env python3
"""The Il2CppDumper ScriptMethod an address lies in: the nearest preceding method, named only when the module's .eh_frame
function that contains the address starts at or before it; an address in a function no method starts is shown as that function.
Use an original NSO/NRO and script.json from that exact module/build. All addresses
are module-relative hex offsets, never file offsets or absolute guest/Ghidra VAs.
"""
import argparse, bisect, json, re, struct, sys
from pathlib import Path
import nso
from nso_code import fde_ranges


class Parser(argparse.ArgumentParser):
    def error(self, message): self.exit(2, f'[RED] {message}\n')


def address(text):
    if not re.fullmatch(r'(?:0[xX])?[0-9a-fA-F]+', text): raise ValueError(f'not a module-relative hexadecimal offset: {text}')
    return int(text, 16)


def symbols(path, text_end):
    data = json.loads(Path(path).read_text())
    if not isinstance(data, dict) or not isinstance(data.get('ScriptMethod'), list) or not data['ScriptMethod']:
        raise ValueError('script.json needs a nonempty ScriptMethod array')
    names = {}
    for row in data['ScriptMethod']:
        if not isinstance(row, dict): raise ValueError('each ScriptMethod must be an object with Address and Name')
        a, name = row.get('Address'), row.get('Name')
        if type(a) is not int or not 0 <= a < text_end or a % 4:
            raise ValueError(f'ScriptMethod Address {a!r} is not an aligned offset in module text; use the matching module-relative script.json')
        if not isinstance(name, str) or not name: raise ValueError('each ScriptMethod needs a nonempty Name')
        names.setdefault(a, set()).add(name)
    return sorted(names), names


def functions(module):
    """Sorted [(start, end)] of the .eh_frame functions in text: IL2CPP code has unwind tables for its C++ exceptions (98% of BALL x PIT's text)"""
    try:
        ranges = fde_ranges(module.image)
    except (AssertionError, IndexError, ValueError, struct.error) as e:
        raise ValueError(f'cannot read the .eh_frame function ranges ({e or type(e).__name__}); they bound the method names') from None
    ranges = [(s, e) for s, e in ranges if 0 <= s < e <= module.text_end]
    if not ranges: raise ValueError('the module has no .eh_frame function ranges in text; they bound the method names')
    return ranges


def main():
    p = Parser(description=__doc__)
    p.add_argument('module', help='original NSO or NRO')
    p.add_argument('script', help='matching Il2CppDumper script.json')
    p.add_argument('addresses', nargs='*', help='module-relative hexadecimal offsets')
    p.add_argument('--dis', metavar='START[:END]', help='disassemble a text range (exclusive END), or START with --count')
    p.add_argument('--count', type=int, help='instruction count for --dis START (default 32)')
    args = p.parse_args()
    try:
        if not args.addresses and args.dis is None: raise ValueError('supply an address or --dis START[:END]')
        if args.count is not None and args.dis is None: raise ValueError('--count requires --dis START')
        module = nso.load(Path(args.module).read_bytes(), args.module)
        starts, names = symbols(args.script, module.text_end)
        fstarts, fends = zip(*functions(module))
        queries = [address(a) for a in args.addresses]
        if any(a >= module.text_end for a in queries): raise ValueError('lookup address is outside module text; use module-relative offsets')
        start = end = 0
        if args.dis is not None:
            parts = args.dis.split(':')
            if len(parts) > 2 or (len(parts) == 2 and args.count is not None): raise ValueError('use --dis START:END or --dis START --count N')
            start = address(parts[0]); count = 32 if args.count is None else args.count
            end = address(parts[1]) if len(parts) == 2 else start + count * 4
            if count <= 0 or start % 4 or end % 4 or not 0 <= start < end <= module.text_end:
                raise ValueError('disassembly needs an aligned, nonempty range within module text and a positive count')

        def name(a):
            if a in names: return ' | '.join(sorted(names[a]))
            j = bisect.bisect_right(fstarts, a) - 1
            if j < 0 or a >= fends[j] or a >= module.text_end: return 'unknown'
            i = bisect.bisect_right(starts, a) - 1
            if i < 0 or starts[i] < fstarts[j]: return f'unknown (function {fstarts[j]:#x}' + (f'+{a - fstarts[j]:#x})' if a > fstarts[j] else ')')
            label = ' | '.join(sorted(names[starts[i]]))
            return (f'({label})' if len(names[starts[i]]) > 1 else label) + f'+{a - starts[i]:#x}'

        for a in queries: print(f'{a:#x} {name(a)}')
        if args.dis is not None:
            from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
            from capstone.arm64 import ARM64_OP_IMM
            md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
            cursor = start
            for ins in md.disasm(bytes(module.image[start:end]), start):
                ann = ''
                if ins.mnemonic in ('b', 'bl') and ins.operands[0].type == ARM64_OP_IMM: ann = '  ; ' + name(ins.operands[0].imm)
                print(f'{ins.address:#x} {ins.mnemonic:<7} {ins.op_str}{ann}')
                cursor = ins.address + ins.size
            if cursor != end: raise ValueError(f'cannot disassemble instruction at {cursor:#x}')
        return 0
    except (OSError, ValueError, ImportError) as e:
        print(f'[RED] {e}', file=sys.stderr); return 1


if __name__ == '__main__':
    sys.exit(main())
