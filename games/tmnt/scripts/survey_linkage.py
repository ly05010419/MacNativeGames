#!/usr/bin/env python3
"""Summarize candidate guest symbol providers without printing symbol names."""
import argparse
import hashlib
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

from rewrite_modules import GAME, MODULES, sha256

sys.path.insert(0, str(GAME / 'third_party/d2r'))
from nsoimg import load, mod0, relocs


def classify(unresolved, exports, bindings):
    counts = Counter()
    for entry in unresolved:
        module, symbol = entry[:2]
        kind = entry[2] if len(entry) > 2 else None
        providers = exports.get(symbol, set()) - {module}
        counts['no_candidate' if not providers else 'unique_candidate' if len(providers) == 1 else 'multiple_candidates'] += 1
        if not providers:
            weak = bindings[module].get(symbol, set()) == {2}
            counts['unmatched_weak_only' if weak else 'unmatched_not_proven_weak'] += 1
            if not weak and kind is not None:
                counts[f'unmatched_not_proven_weak_kind_{kind}'] += 1
        if {'sdk', 'subsdk1'} <= providers:
            counts['multiple_including_sdk_and_subsdk1'] += 1
    return dict(counts)


def slot_address_stubs(image, text_end, wanted):
    """Find a conservative subset of PLT stubs passing their slot address in x16."""
    if text_end < 0 or text_end > len(image) or text_end % 4:
        raise ValueError('invalid text range')
    words = memoryview(image)[:text_end].cast('I')
    found = set()
    for i in range(len(words) - 3):
        adrp = words[i]
        if adrp & 0x9f00001f != 0x90000010:
            continue
        ldr, add, branch = words[i + 1:i + 4]
        if ldr & 0xffc003ff != 0xf9400211 or add & 0xffc003ff != 0x91000210 or branch != 0xd61f0220:
            continue
        load_offset = ((ldr >> 10) & 0xfff) * 8
        if load_offset != ((add >> 10) & 0xfff):
            continue
        immediate = ((adrp >> 29) & 3) | (((adrp >> 5) & 0x7ffff) << 2)
        immediate = (immediate ^ (1 << 20)) - (1 << 20)
        target = ((i * 4) & ~4095) + (immediate << 12) + load_offset
        if target in wanted:
            found.add(target)
    return found


def add_nro_exports(romfs, exports, details=None):
    """Add NRO exports as hypothetical providers; this does not prove they load."""
    index = json.loads((romfs / 'index.json').read_text())
    image = romfs / 'romfs.bin'
    if sha256(image) != index['romfs_sha256']:
        raise ValueError('RomFS image differs from extraction index')
    size = image.stat().st_size
    candidates = [item for item in index['files'] if item['path'].lower().endswith('.nro')]
    registrations = [item for item in index['files'] if item['path'].lower().endswith('.nrr')]
    registered = set()
    matched = 0
    with image.open('rb') as source:
        for item in registrations:
            start, length = item['offset'], item['size']
            if not 0 <= start <= size or not 0x348 <= length <= min(size - start, 16 * 1024 * 1024):
                raise ValueError('NRR extent out of bounds')
            source.seek(start)
            nrr = source.read(length)
            if len(nrr) != length or nrr[:4] != b'NRR0':
                raise ValueError('invalid NRR')
            offset, count = struct.unpack_from('<II', nrr, 0x340)
            if count > 65536 or offset > length or count * 32 > length - offset:
                raise ValueError('NRR hash table out of bounds')
            registered.update(nrr[offset + 32 * i:offset + 32 * (i + 1)] for i in range(count))
        for number, item in enumerate(candidates):
            start, length = item['offset'], item['size']
            if not 0 <= start <= size or not 0x80 <= length <= min(size - start, 256 * 1024 * 1024):
                raise ValueError('NRO extent out of bounds')
            source.seek(start)
            data = source.read(length)
            if len(data) != length or data[16:20] != b'NRO0':
                raise ValueError('invalid NRO')
            matched += hashlib.sha256(data).digest() in registered
            module = mod0(data)
            pos = module + struct.unpack_from('<i', data, module + 4)[0]
            tags = {}
            for _ in range(4096):
                if not 0 <= pos <= len(data) - 16:
                    raise ValueError('NRO dynamic table out of bounds')
                tag, value = struct.unpack_from('<qQ', data, pos)
                pos += 16
                if not tag:
                    break
                tags[tag] = value
            else:
                raise ValueError('NRO dynamic table unterminated')
            if not all(key in tags for key in (4, 5, 6, 10)) or tags[4] > len(data) - 8:
                raise ValueError('unsupported NRO dynamic symbols')
            count = struct.unpack_from('<II', data, tags[4])[1]
            if (count > 1_000_000 or tags[6] > len(data) or count * 24 > len(data) - tags[6] or
                    tags[5] > len(data) or tags[10] > len(data) - tags[5]):
                raise ValueError('NRO symbol table out of bounds')
            imports = defaultdict(set)
            for i in range(count):
                offset, info, visibility, section, _, _ = struct.unpack_from('<IBBHQQ', data, tags[6] + 24 * i)
                if not offset:
                    continue
                if offset >= tags[10]:
                    raise ValueError('NRO symbol name out of bounds')
                pos = tags[5] + offset
                end = data.index(b'\0', pos, tags[5] + tags[10])
                symbol = data[pos:end]
                if not section:
                    imports[symbol].add(info >> 4)
                elif info >> 4 in (1, 2) and visibility & 3 in (0, 3):
                    exports[symbol].add(f'nro#{number}')
            if details is not None:
                records = relocs(data)
                module = f'nro#{number}'
                details['bindings'][module] = imports
                details['relocations'][module] = len(records)
                details['kinds'].update(kind for _, kind, _, _, _ in records)
                details['unresolved'].extend((module, symbol.encode('latin1'), kind, off) for off, kind, symbol, value, _ in records
                                             if symbol and value is None)
    return {'nro_files': len(candidates), 'nrr_files': len(registrations),
            'nro_digests_listed_in_nrr': matched, 'nrr_unique_digests': len(registered)}


def survey(exefs, romfs=None, plt=False):
    manifest = json.loads((exefs.parent / 'extraction-manifest.json').read_text())
    if manifest['program']['title_id'] != '0100FE701475A000':
        raise ValueError('manifest is not the supported base game')
    expected = {entry['name']: entry['sha256'] for entry in manifest['files']}
    modules = ('rtld', *MODULES)
    exports, bindings = defaultdict(set), {}
    unresolved, kinds, totals = [], Counter(), {}
    for name in modules:
        path = exefs / name
        if sha256(path) != expected.get(name):
            raise ValueError('module differs from extraction manifest')
        with path.open('rb') as source:
            raw = source.read(256)
        ro = struct.unpack_from('<I', raw, 0x24)[0]
        _, _, symoff, symsize = struct.unpack_from('<IIII', raw, 0x90)
        image = load(path, bss=False)
        mod = mod0(image)
        pos = mod + struct.unpack_from('<i', image, mod + 4)[0]
        tags = {}
        for _ in range(4096):
            tag, value = struct.unpack_from('<qQ', image, pos)
            pos += 16
            if not tag:
                break
            tags[tag] = value
        else:
            raise ValueError('dynamic table unterminated')
        if symsize % 24 or ro + symoff != tags[6]:
            raise ValueError('symbol table mismatch')
        imports = defaultdict(set)
        for i in range(symsize // 24):
            offset, info, visibility, section, _, _ = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
            if not offset:
                continue
            pos = tags[5] + offset
            if not tags[5] <= pos < tags[5] + tags[10]:
                raise ValueError('symbol name out of bounds')
            end = image.index(0, pos, tags[5] + tags[10])
            symbol = bytes(image[pos:end])
            if not section:
                imports[symbol].add(info >> 4)
            elif info >> 4 in (1, 2) and visibility & 3 in (0, 3):
                exports[symbol].add(name)
        bindings[name] = imports
        records = relocs(image)
        kinds.update(kind for _, kind, _, _, _ in records)
        unresolved.extend((name, symbol.encode('latin1'), kind, off) for off, kind, symbol, value, _ in records if symbol and value is None)
        totals[name] = len(records)
        if sha256(path) != expected[name]:
            raise ValueError('module changed during inspection')
    result = {'relocations_by_module': totals, 'relocation_kinds': dict(sorted(kinds.items())),
              'undefined_reference_candidates': classify(unresolved, exports, bindings)}
    if plt:
        slots = {}
        for name in modules:
            wanted = {off for module, symbol, kind, off in unresolved if module == name and kind == 1026
                      and not exports.get(symbol, set()) - {name} and bindings[name].get(symbol, set()) != {2}}
            if not wanted:
                continue
            path = exefs / name
            with path.open('rb') as source:
                raw = source.read(256)
            text_end = struct.unpack_from('<I', raw, 0x18)[0]
            image = load(path, bss=False)
            found = slot_address_stubs(image, text_end, wanted)
            if sha256(path) != expected[name]:
                raise ValueError('module changed during PLT scan')
            slots[name] = {'unmatched_nonweak_jump_slots': len(wanted), 'standard_stubs_matched': len(found)}
        result['plt_stub_scan'] = slots
    if romfs is not None:
        details = {'bindings': {}, 'relocations': {}, 'kinds': Counter(), 'unresolved': []}
        result['nro_registration_inventory'] = add_nro_exports(romfs, exports, details)
        result['hypothetical_candidates_including_nros'] = classify(unresolved, exports, bindings)
        result['nro_relocations'] = details['relocations']
        result['nro_relocation_kinds'] = dict(sorted(details['kinds'].items()))
        result['nro_undefined_reference_candidates'] = classify(details['unresolved'], exports, details['bindings'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', required=True, type=Path)
    parser.add_argument('--romfs', type=Path, help='include private NRO exports as hypothetical providers')
    parser.add_argument('--plt', action='store_true', help='scan known PLT stub shapes without outputting game code')
    args = parser.parse_args()
    print(json.dumps(survey(args.exefs.resolve(), args.romfs.resolve() if args.romfs else None, args.plt), indent=2))
    print('Candidate providers do not establish that NROs load or prove correct linker behavior.')


if __name__ == '__main__':
    main()
