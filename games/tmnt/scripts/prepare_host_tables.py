#!/usr/bin/env python3
"""Generate private native-loader tables from the checked TMNT rewrite.

The output contains game-derived symbol names. Keep it outside the checkout.
This tool only reads files; it does not execute guest code.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

from module_metadata import verify_rewrite_report
from rewrite_modules import MODULES, REPO


def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def dynamic_tags(image):
    mod0 = struct.unpack_from('<I', image, 4)[0]
    if image[mod0:mod0 + 4] != b'MOD0':
        raise ValueError('missing MOD0')
    dynamic = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
    tags = {}
    for offset in range(dynamic, dynamic + 65536, 16):
        tag, value = struct.unpack_from('<qQ', image, offset)
        if tag == 0:
            return tags
        tags[tag] = value
    raise ValueError('unterminated dynamic table')


def ctor_flag(image, tags, layout):
    init = tags[12]
    words = [struct.unpack_from('<I', image, init + 4 * i)[0] for i in range(16)]
    for i, adrp in enumerate(words):
        if adrp & 0x9f000000 != 0x90000000:
            continue
        reg = adrp & 31
        for ldr in words[i + 1:]:
            if ldr & 0xffc00000 != 0xb9400000 or (ldr >> 5) & 31 != reg:
                continue
            imm = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7ffff) << 2)
            imm = (imm ^ (1 << 20)) - (1 << 20)
            flag = ((init + 4 * i) & ~4095) + (imm << 12) + ((ldr >> 10) & 0xfff) * 4
            if not layout['DATA_OFF'] <= flag < layout['KEY_PAGE_OFF']:
                raise ValueError('constructor state outside module data')
            return flag
    raise ValueError('constructor state load not found')


def read_gates(path):
    gates = []
    for line in path.read_text().splitlines():
        ret, kind, arg, original = line.split()
        if kind not in ('svc', 'cntpct', 'cntvct'):
            raise ValueError(f'unsupported gate kind {kind}')
        gates.append((int(ret, 0), int(original, 0), int(kind != 'svc'), int(arg)))
    return gates


def generate(exefs, rewritten, metadata, output):
    if output == REPO or output.is_relative_to(REPO):
        raise ValueError('generated tables must stay outside the repository')
    hashes, report = verify_rewrite_report(exefs, rewritten)
    doc = json.loads(metadata.read_text())
    if (doc['input_sha256'] != {name: hashes[name] for name in MODULES}
            or doc['tools'] != report['tools'] or doc['options'] != report['options']):
        raise ValueError('metadata does not match verified rewrite')
    if set(doc['modules']) != set(MODULES):
        raise ValueError('unexpected module set')
    names = ('main', 'subsdk0', 'subsdk2', 'subsdk1')
    if doc['modules']['sdk']['rewritten_sha256'] != doc['modules']['subsdk1']['rewritten_sha256']:
        raise ValueError('sdk/subsdk1 alias changed')
    lines = ['/* Generated from checked private game inputs. Do not commit. */']
    descriptors = []
    for index, name in enumerate(names):
        item = doc['modules'][name]
        path = rewritten / f'{name}.patched.bin'
        if sha(path) != item['rewritten_sha256'] or path.stat().st_size != item['file_size']:
            raise ValueError(f'{name}: rewritten image changed')
        image = path.read_bytes()
        tags = dynamic_tags(image)
        layout = item['layout']
        gates = read_gates(rewritten / f'{name}.gates.tsv')
        lines.append(f'static const Reloc tmnt_reloc_{index}[] = {{')
        lines.extend(f'{{{r["offset"]}ULL,{r["kind"]},{r["symbol"]},{r["addend"]}LL}},' for r in item['relocations'])
        lines.append('};')
        lines.append(f'static const Symbol tmnt_symbol_{index}[] = {{')
        lines.extend('{' + f'{json.dumps(s["name"])},{s["value"]}ULL,{s["size"]}ULL,{int(s["defined"])},{s["binding"]},{s["type"]}' + '},' for s in item['symbols'])
        lines.append('};')
        lines.append(f'static const Gate tmnt_gate_{index}[] = {{')
        lines.extend(f'{{{ret}ULL,{original}ULL,{timer},{arg}}},' for ret, original, timer, arg in gates)
        if not gates:
            lines.append('{0,0,0,0},')
        lines.append('};')
        descriptors.append('{' + ','.join((
            f'.name={json.dumps(name)}', f'.digest={json.dumps(item["rewritten_sha256"])}',
            f'.file_size={item["file_size"]}ULL',
            *(f'.{field}={layout[key]}ULL' for field, key in (
                ('text_end', 'TEXT_END'), ('ro_off', 'RO_OFF'), ('data_off', 'DATA_OFF'),
                ('patch_off', 'PATCH_OFF'), ('key_off', 'KEY_PAGE_OFF'))),
            f'.symbols=tmnt_symbol_{index}', f'.nsym={len(item["symbols"])}',
            f'.relocs=tmnt_reloc_{index}', f'.nreloc={len(item["relocations"])}',
            f'.gates=tmnt_gate_{index}', f'.ngate={len(gates)}',
            f'.init={tags.get(12, 0)}ULL', f'.ctor_flag={ctor_flag(image, tags, layout)}ULL',
            f'.init_array={tags.get(25, 0)}ULL', f'.init_size={tags.get(27, 0)}ULL')) + '},')
    lines.append('static Module modules[] = {')
    lines.extend(descriptors)
    lines.extend(('};', '#define PROBE_PLT 0ULL', '#define PROBE_SLOT 0ULL'))
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open('x') as f:
        f.write('\n'.join(lines) + '\n')
    return len(names)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('exefs', 'rewritten', 'metadata', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    count = generate(*(getattr(args, n).resolve() for n in ('exefs', 'rewritten', 'metadata', 'out')))
    print(f'Generated private tables for {count} unique modules; no guest code executed.')


if __name__ == '__main__':
    main()
