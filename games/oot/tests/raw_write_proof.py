#!/usr/bin/env python3
"""Check the native interval analyzer against an independent uint32 oracle.

Synthetic SPIR-V uses the same attested low/high argument ABI as the translator.
This validates range analysis, not a complete game's image or frame rate.
"""
import argparse
import base64
import ctypes
import json
from pathlib import Path
import random
import struct

MASK = 0xffffffff


def instruction(op, *operands):
    return [((len(operands) + 1) << 16) | op, *operands]


def string_words(text):
    data = text.encode() + b'\0'
    data += bytes((-len(data)) % 4)
    return list(struct.unpack('<' + 'I' * (len(data) // 4), data))


def fixture(operation=128, literal=3, hidden=False, atomic=False, wide_branch=False):
    # Types: uint, void, bool, uvec3, uint64, function pointers, uniform block.
    rows = [instruction(17, 1), instruction(17, 11), instruction(17, 5347),
            instruction(10, *string_words('SPV_KHR_physical_storage_buffer')),
            instruction(14, 5348, 1), instruction(15, 5, 100, *string_words('main'), 30, 31),
            instruction(16, 100, 17, 32, 1, 1),
            instruction(5, 200, *string_words('other_writer' if hidden else 'dmsh_raw_write_fixture')),
            instruction(5, 25, *string_words('u')),
            instruction(71, 30, 11, 26), instruction(71, 31, 11, 27),
            instruction(71, 20, 2), instruction(72, 20, 0, 35, 0),
            instruction(71, 25, 34, 0), instruction(71, 25, 33, 0),
            instruction(19, 1), instruction(21, 2, 32, 0), instruction(20, 3),
            instruction(23, 4, 2, 3), instruction(21, 5, 64, 0),
            instruction(32, 6, 7, 2), instruction(32, 7, 5349, 2),
            instruction(33, 8, 1), instruction(33, 9, 1, 6, 6),
            instruction(30, 20, 2), instruction(32, 21, 2, 20),
            instruction(32, 22, 2, 2), instruction(32, 23, 1, 4),
            instruction(59, 21, 25, 2), instruction(59, 23, 30, 1), instruction(59, 23, 31, 1)]
    for ident, word in [(40, 0), (41, 1), (42, literal), (43, 7), (44, 14), (45, 30), (46, 32), (47, 2)]:
        rows.append(instruction(43, 2, ident, word & MASK))
    rows.extend([instruction(43, 5, 48, 0, 256),  # uint64(1 << 40)
                 instruction(54, 1, 200, 0, 9), instruction(55, 6, 201), instruction(55, 6, 202), instruction(248, 203),
                 instruction(61, 2, 204, 201), instruction(61, 2, 205, 202),
                 instruction(113, 5, 206, 204), instruction(113, 5, 207, 205),
                 instruction(196, 5, 208, 207, 46), instruction(197, 5, 209, 206, 208),
                 instruction(120, 7, 210, 209)])
    rows.append(instruction(228, 210, 41, 40, 40) if atomic else instruction(62, 210, 40, 2, 4))
    rows.extend([instruction(253), instruction(56), instruction(54, 1, 100, 0, 8), instruction(248, 101),
                 instruction(59, 6, 102, 7), instruction(59, 6, 103, 7)])
    if wide_branch:
        rows.extend([instruction(45, 5, 104), instruction(176, 3, 105, 104, 48),
                     instruction(247, 108, 0), instruction(250, 105, 106, 107),
                     instruction(248, 106), instruction(62, 102, 40), instruction(62, 103, 44),
                     instruction(57, 1, 109, 200, 102, 103), instruction(249, 108),
                     instruction(248, 107), instruction(62, 102, 40), instruction(62, 103, 45),
                     instruction(57, 1, 110, 200, 102, 103), instruction(249, 108), instruction(248, 108)])
    else:
        rows.extend([instruction(65, 22, 104, 25, 40), instruction(61, 2, 105, 104),
                     instruction(61, 4, 106, 30), instruction(81, 2, 107, 106, 0),
                     instruction(61, 4, 108, 31), instruction(81, 2, 109, 108, 0),
                     instruction(128, 2, 110, 107, 109), instruction(128, 2, 111, 110, 105),
                     instruction(operation, 2, 112, 111, 42), instruction(199, 2, 113, 112, 43),
                     instruction(128, 2, 114, 113, 44), instruction(62, 102, 40), instruction(62, 103, 114),
                     instruction(57, 1, 115, 200, 102, 103)])
    rows.extend([instruction(253), instruction(56)])
    words = [0x07230203, 0x00010300, 0, 256, 0] + [w for row in rows for w in row]
    return struct.pack('<' + 'I' * len(words), *words)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--library', type=Path, required=True)
    ap.add_argument('--work', type=Path, required=True)
    args = ap.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    library = ctypes.CDLL(str(args.library.resolve()))
    prove = library.oot_raw_write_proof
    prove.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int]
    prove.restype = ctypes.c_int
    rng = random.Random(0x60090)
    comparisons = 0
    reports = []

    def run(code, word, immutable=True, bind=True):
        request = dict(abi=1, spirv=base64.b64encode(code).decode(), grid=[16, 1, 1], local=[32, 1, 1], simd_width=32,
                       bindings=[dict(name='u', reg=0, arg_id=0, immutable=immutable, data=base64.b64encode(struct.pack('<I', word)).decode())] if bind else [])
        reply = ctypes.create_string_buffer(131072)
        n = prove(json.dumps(request).encode(), reply, len(reply))
        assert n > 0
        return json.loads(reply.value)

    operations = {128: lambda x, y: x+y, 130: lambda x, y: x-y, 132: lambda x, y: x*y,
                  197: lambda x, y: x|y, 198: lambda x, y: x^y, 199: lambda x, y: x&y,
                  196: lambda x, y: x << y, 194: lambda x, y: x >> y,
                  195: lambda x, y: (x if x < 0x80000000 else x-(1 << 32)) >> y}
    for op, oracle in operations.items():
        for trial in range(4):
            word = rng.getrandbits(32)
            literal = rng.randrange(32) if op in (194, 195, 196) else rng.getrandbits(32)
            code = fixture(op, literal, atomic=trial == 3)
            result = run(code, word)
            assert result['ok'], result
            assert len(result['calls']) == 1
            assert dict(reg=0, arg_id=0, offset=0, word=word) in result['guards']
            for group in range(16):
                for local in range(32):
                    value = (group + local + word) & MASK
                    high = 14 + (oracle(value, literal) & MASK & 7)
                    assert any(a <= high <= b for a, b in result['high_intervals']), (op, trial, high, result)
                    comparisons += 1
                    # The fixture writes the word at low 0: the exact byte refinement must cover it.
                    word_at = high << 32
                    assert any(a <= word_at and word_at + 4 <= b for a, b in result['byte_intervals']), (op, trial, high, result)
                    comparisons += 1
            assert result['byte_guards'] and all(g in result['byte_guards'] for g in result['guards']), result
            span = sum(b - a for a, b in result['byte_intervals'])
            assert span <= 4 * 2 * len(result['byte_intervals']) * 16, (op, trial, result['byte_intervals'])
            reports.append(dict(op=op, trial=trial, intervals=result['high_intervals']))
            (args.work / f'op-{op}-{trial}.spv').write_bytes(code)
    wide = run(fixture(wide_branch=True), 0)
    assert wide['ok'] and len(wide['calls']) == 2, wide
    for high in (14, 30):
        assert any(a <= high <= b for a, b in wide['high_intervals']), wide
        comparisons += 1
        assert any(a <= high << 32 and (high << 32) + 4 <= b for a, b in wide['byte_intervals']), wide
        comparisons += 1
    for name, result in [('unattested-store', run(fixture(hidden=True), 7)),
                         ('mutable-uniform', run(fixture(), 7, immutable=False)),
                         ('missing-uniform', run(fixture(), 7, bind=False)),
                         ('truncated-module', run(fixture()[:-3], 7))]:
        assert not result['ok'], (name, result)
        reports.append(dict(negative=name, error=result['error']))
        comparisons += 1
    output = dict(ok=True, comparisons=comparisons, cases=reports, scope='synthetic range/ABI checks; no game FPS claim')
    (args.work / 'results.json').write_text(json.dumps(output, indent=2) + '\n')
    print(f'raw-write-proof: pass {comparisons} independent comparisons; exact byte refinement, ordinary/atomic stores, uint32 wrap/shift, 64-bit unknown branch and four rejection controls')


if __name__ == '__main__':
    main()
