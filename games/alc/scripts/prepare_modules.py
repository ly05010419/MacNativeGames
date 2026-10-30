#!/usr/bin/env python3
"""Verify NSO segments, inventory dynamic symbols, and write analysis-only ELF.

ELF preserves NSO virtual segment offsets; it is NOT a macOS executable.
Only defined dynsym section indices are remapped for ELF section numbering.
"""
import argparse
import collections
import hashlib
import json
import struct
from pathlib import Path
import lz4.block


def check(ok, message):
    if not ok:
        raise ValueError(message)


def align(value, boundary=4096):
    return (value + boundary - 1) & ~(boundary - 1)


def prepare(source, destination):
    raw = source.read_bytes()
    check(raw[:4] == b'NSO0' and len(raw) >= 256, 'Expected NSO0')
    flags = struct.unpack_from('<I', raw, 12)[0]
    compressed = struct.unpack_from('<III', raw, 0x60)
    segments = []
    for i, name in enumerate(['text', 'rodata', 'data']):
        fileoff, address, size, extra = struct.unpack_from('<IIII', raw, 0x10 + 16 * i)
        stored = compressed[i] if flags & (1 << i) else size
        check(fileoff + stored <= len(raw) and size <= 256 * 1024 * 1024, 'Invalid NSO segment')
        payload = raw[fileoff:fileoff + stored]
        data = lz4.block.decompress(payload, uncompressed_size=size) if flags & (1 << i) else payload
        digest = hashlib.sha256(data).digest()
        check(len(data) == size, 'NSO decompression size mismatch')
        if flags & (8 << i):
            check(digest == raw[0xA0 + 32 * i:0xC0 + 32 * i], f'{source.name}: {name} hash mismatch')
        segments.append({'name': name, 'address': address, 'size': size,
                         'sha256': digest.hex(), 'data': data, 'extra': extra})
    image = bytearray(max(s['address'] + s['size'] for s in segments))
    for s in segments:
        image[s['address']:s['address'] + s['size']] = s['data']

    def span(address, size):
        check(any(s['address'] <= address and address + size <= s['address'] + s['size'] for s in segments), 'Metadata outside initialized segments')
        return bytes(image[address:address + size])

    modoff = struct.unpack_from('<I', image, 4)[0]
    check(span(modoff, 4) == b'MOD0', 'Expected MOD0')
    dynoff = modoff + struct.unpack_from('<i', image, modoff + 4)[0]
    dynamic = []
    for index in range(4096):
        tag, value = struct.unpack('<qQ', span(dynoff + 16 * index, 16))
        dynamic.append((tag, value))
        if tag == 0:
            break
    else:
        raise ValueError('Unterminated dynamic table')
    tags = dict(dynamic)
    strings = span(tags[5], tags[10])

    def string(offset):
        check(offset < len(strings), 'Invalid dynstr offset')
        end = strings.find(b'\0', offset)
        check(end >= 0, 'Unterminated dynstr')
        return strings[offset:end].decode('utf-8', errors='replace')

    symoff = tags[6]
    check(tags[11] == 24, 'Expected ELF64 symbols')
    if 4 in tags:
        nbucket, nchain = struct.unpack('<II', span(tags[4], 8))
        symcount = nchain
    else:
        _, _, syms_offset, syms_size = struct.unpack_from('<IIII', raw, 0x90)
        check(segments[1]['address'] + syms_offset == symoff and syms_size % 24 == 0, 'Cannot establish symbol count')
        symcount = syms_size // 24
    symbols = []
    for i in range(symcount):
        no, info, other, section, value, size = struct.unpack('<IBBHQQ', span(symoff + 24 * i, 24))
        symbols.append({'index': i, 'name': string(no), 'value': value, 'size': size,
                        'binding': info >> 4, 'type': info & 15, 'section_index': section,
                        'defined': section != 0})
    needed = [string(value) for tag, value in dynamic if tag == 1]
    imports = [s for s in symbols if s['name'] and not s['defined']]
    exports = [s for s in symbols if s['name'] and s['defined']]
    svc = []
    text = segments[0]
    for offset in range(0, len(text['data']) - 3, 4):
        insn = struct.unpack_from('<I', text['data'], offset)[0]
        if insn & 0xFFE0001F == 0xD4000001:
            svc.append({'offset': text['address'] + offset, 'immediate': (insn >> 5) & 0xFFFF})

    # ELF headers are synthetic. LOAD payload offsets have the same 4K residue
    # as virtual addresses; runtime relocations and instruction bytes are kept.
    phnum = 4
    cursor = 4096
    loads = []
    elf = bytearray(4096)
    for i, s in enumerate(segments):
        cursor = align(cursor) + s['address'] % 4096
        s['elf_file_offset'] = cursor
        end = cursor + s['size']
        elf.extend(bytes(max(0, end - len(elf))))
        elf[cursor:end] = s['data']
        loads.append((1, [5, 4, 6][i], cursor, s['address'], s['address'], s['size'],
                      s['size'] + (s['extra'] if i == 2 else 0), 4096))
        cursor = end

    def file_offset(address, size=1):
        for s in segments:
            if s['address'] <= address and address + size <= s['address'] + s['size']:
                return s['elf_file_offset'] + address - s['address']
        raise ValueError('ELF metadata outside initialized segments')

    # name, type, flags, address, fileoff, size, link, info, alignment, entsize
    data = segments[2]
    dynsize = len(dynamic) * 16
    locals_count = sum(s['binding'] == 0 for s in symbols)
    metadata = [('.dynamic', 6, 3, dynoff, dynsize, '.dynstr', 0, 8, 16),
                ('.dynstr', 3, 2, tags[5], tags[10], None, 0, 1, 0),
                ('.dynsym', 11, 2, symoff, symcount * 24, '.dynstr', locals_count, 8, 24)]
    if 4 in tags:
        hashsize = 8 + 4 * (nbucket + nchain)
        metadata.append(('.hash', 5, 2, tags[4], hashsize, '.dynsym', 0, 8, 4))
    for name, address_tag, size_tag in [('.rela.dyn', 7, 8), ('.rela.plt', 23, 2)]:
        if address_tag in tags and size_tag in tags and tags[size_tag]:
            check(name != '.rela.plt' or tags.get(20) == 7, 'Unsupported PLT relocation format')
            size = tags[size_tag]
            metadata.append((name, 4, 2, tags[address_tag], size, '.dynsym', 0, 8, 24))
    sections = [('', 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    pending_links = {}
    # Split segment sections around metadata to avoid Ghidra overlay blocks.
    for segment_index, segment in enumerate(segments):
        begin, end = segment['address'], segment['address'] + segment['size']
        contained = sorted((m for m in metadata if begin <= m[3] and m[3] + m[4] <= end), key=lambda m: m[3])
        cursor_address, part = begin, 0
        for meta in contained:
            name, kind, attrs, address, size, link, info, alignment, entsize = meta
            check(address >= cursor_address, 'Overlapping dynamic metadata')
            if address > cursor_address:
                chunkname = '.' + segment['name'] + ('' if part == 0 else f'.{part}')
                sections.append((chunkname, 1, [6, 2, 3][segment_index], cursor_address,
                                 file_offset(cursor_address), address - cursor_address, 0, 0, 1, 0))
                part += 1
            sections.append((name, kind, attrs, address, file_offset(address, size), size, 0, info, alignment, entsize))
            if link:
                pending_links[len(sections) - 1] = link
            cursor_address = address + size
        if cursor_address < end:
            chunkname = '.' + segment['name'] + ('' if part == 0 else f'.{part}')
            sections.append((chunkname, 1, [6, 2, 3][segment_index], cursor_address,
                             file_offset(cursor_address), end - cursor_address, 0, 0, 1, 0))
    sections.append(('.bss', 8, 3, data['address'] + data['size'], data['elf_file_offset'] + data['size'], data['extra'], 0, 0, 1, 0))
    section_indices = {s[0]: i for i, s in enumerate(sections)}
    for i, link in pending_links.items():
        row = list(sections[i])
        row[6] = section_indices[link]
        sections[i] = tuple(row)
    changed = []
    for s in symbols:
        if s['section_index'] in (0, 0xFFF1, 0xFFF2):
            continue
        index = next((i for i, section in enumerate(sections)
                      if section[2] & 2 and section[3] <= s['value'] < section[3] + section[5]), None)
        if index is None:
            index = next((i for i, section in enumerate(sections)
                          if section[2] & 2 and s['size'] == 0 and s['value'] == section[3] + section[5]), None)
        check(index is not None, f'Cannot map defined symbol: {s["name"]}')
        if index != s['section_index']:
            location = file_offset(symoff + s['index'] * 24 + 6, 2)
            struct.pack_into('<H', elf, location, index)
            changed.append({'symbol': s['index'], 'file_offset': location,
                            'original': s['section_index'], 'elf': index})
    name_table = bytearray(b'\0')
    names = {}
    for name in [s[0] for s in sections] + ['.shstrtab']:
        if name not in names:
            names[name] = len(name_table)
            name_table.extend(name.encode() + b'\0')
    nameoff = align(len(elf), 8)
    elf.extend(bytes(nameoff - len(elf)) + name_table)
    sections.append(('.shstrtab', 3, 0, 0, nameoff, len(name_table), 0, 0, 1, 0))
    shoff = align(len(elf), 8)
    elf.extend(bytes(shoff - len(elf)))
    for section in sections:
        name, *fields = section
        elf.extend(struct.pack('<IIQQQQIIQQ', names[name] if name else 0, *fields))
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    elf[:64] = struct.pack('<16sHHIQQQIHHHHHH', ident, 3, 183, 1, 0, 64, shoff, 0, 64, 56, phnum, 64, len(sections), len(sections) - 1)
    loads.append((2, 6, file_offset(dynoff), dynoff, dynoff, dynsize, dynsize, 8))
    for i, ph in enumerate(loads):
        struct.pack_into('<IIQQQQQQ', elf, 64 + 56 * i, *ph)
    # Validate every LOAD byte against the NSO, allowing only recorded index edits.
    check_image = bytearray(elf)
    for change in changed:
        struct.pack_into('<H', check_image, change['file_offset'], change['original'])
    for s in segments:
        offset = s['elf_file_offset']
        check(bytes(check_image[offset:offset + s['size']]) == s['data'], 'ELF payload differs from NSO')
    destination.write_bytes(elf)
    result = {'module': source.name, 'nso_sha256': hashlib.sha256(raw).hexdigest(),
              'build_id': raw[0x40:0x60].hex(), 'elf_sha256': hashlib.sha256(elf).hexdigest(),
              'elf_purpose': 'analysis carrier only; not a native port or runnable program',
              'segments': [{k: v for k, v in s.items() if k != 'data'} for s in segments],
              'segment_hashes_verified': True, 'mod0_offset': modoff, 'dynamic_offset': dynoff,
              'dynamic_tags': dynamic, 'needed': needed, 'imports': imports, 'exports': exports,
              'svc_instruction_candidates': svc, 'elf_symbol_index_changes': changed,
              'limitations': ['Service permissions do not prove runtime usage.', 'SVC byte matches are candidates until control flow is analyzed.', 'ELF headers and symbol section indices are synthetic analysis metadata.']}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exefs', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    inventory = []
    for name in ['rtld', 'main', 'sdk', 'subsdk0']:
        if not (args.exefs / name).exists():
            continue   # Astral Chain has two modules plus rtld: no subsdk0
        result = prepare(args.exefs / name, args.output / (name + '.elf'))
        (args.output / (name + '-inventory.json')).write_text(json.dumps(result, indent=2) + '\n')
        summary = {k: result[k] for k in ['module', 'build_id', 'needed', 'segment_hashes_verified']}
        summary.update(imports=len(result['imports']), exports=len(result['exports']),
                       text_bytes=result['segments'][0]['size'], svc_candidates=len(result['svc_instruction_candidates']))
        inventory.append(summary)
    (args.output / 'modules.json').write_text(json.dumps(inventory, indent=2) + '\n')
    print(json.dumps(inventory, indent=2))


if __name__ == '__main__':
    main()
