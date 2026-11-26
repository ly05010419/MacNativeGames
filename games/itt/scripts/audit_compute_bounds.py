#!/usr/bin/env python3
"""Read-only audit of direct constant Workgroup-array accesses in cached SPIR-V.

This is not a complete bounds proof: dynamic indices, nested arrays and pointer
aliases are not evaluated. Findings identify invalid translated array layouts,
not the visual effect using them or the guest's required allocation size.
"""
import argparse
import json
from pathlib import Path
import struct


def audit(raw):
    if len(raw) < 20 or len(raw) % 4:
        raise ValueError('invalid SPIR-V byte length')
    words = struct.unpack('<%dI' % (len(raw) // 4), raw)
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    integers, constants, arrays, pointers, variables, accesses = {}, {}, {}, {}, {}, []
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if not count or cursor + count > len(words):
            raise ValueError('invalid SPIR-V instruction length')
        args = words[cursor + 1:cursor + count]
        if opcode == 21 and len(args) == 3:  # OpTypeInt
            integers[args[0]] = args[1:]
        elif opcode == 43 and len(args) == 3:  # 32-bit OpConstant
            constants[args[1]] = (args[0], args[2])
        elif opcode == 28 and len(args) == 3:  # OpTypeArray
            arrays[args[0]] = args[1:]
        elif opcode == 32 and len(args) == 3:  # OpTypePointer
            pointers[args[0]] = args[1:]
        elif opcode == 59 and len(args) >= 3:  # OpVariable
            variables[args[1]] = (args[0], args[2])
        elif opcode in (65, 66) and len(args) == 4:  # direct, one-dimensional access
            accesses.append((cursor, args[2], args[3]))
        cursor += count

    def integer(ident):
        value = constants.get(ident)
        if value is None:
            return None
        kind, number = value
        width, signed = integers.get(kind, (0, 0))
        if width != 32:
            return None
        return number - (1 << 32) if signed and number & (1 << 31) else number

    findings = []
    for offset, base, index_id in accesses:
        pointer, storage = variables.get(base, (None, None))
        pointer_storage, target = pointers.get(pointer, (None, None))
        if storage != 4 or pointer_storage != 4 or target not in arrays:
            continue  # Workgroup only
        length, index = integer(arrays[target][1]), integer(index_id)
        if length is not None and index is not None and (index < 0 or index >= length):
            findings.append(dict(word_offset=offset, variable=base, length=length, index=index))
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cache', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    results, errors = [], []
    files = sorted(args.cache.glob('*.spv'))
    for file in files:
        try:
            results.extend(dict(file=file.name, **item) for item in audit(file.read_bytes()))
        except ValueError as error:
            errors.append(dict(file=file.name, error=str(error)))
    report = dict(scanned=len(files), findings=results, errors=errors,
                  scope='Direct constant indices only; no dynamic bounds or visual attribution')
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(files)} modules; {len(results)} out-of-bounds accesses; {len(errors)} parse errors')
    return 1 if results or errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
