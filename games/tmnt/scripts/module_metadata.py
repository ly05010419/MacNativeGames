#!/usr/bin/env python3
"""Build private, checked module metadata for a future native loader.

The output includes game-derived symbol names and must stay outside the checkout.
This does not map modules, resolve imports, or execute game code.
"""
import argparse
import hashlib
import importlib.metadata
import json
import struct
import sys
from pathlib import Path

from rewrite_modules import GAME, MODULES, REPO, TITLE, TOOL_SOURCES, OUTPUTS, MAIN_RULE_OUTPUT, sha256, verify_exefs
from survey_modules import data_bounds, validate_relocations

sys.path.insert(0, str(GAME / 'third_party/d2r'))
from nsoimg import header as rewrite_layout, load, mod0
from relr import decode_relr


def checked_region(image, offset, size, what):
    if offset < 0 or size < 0 or offset > len(image) or size > len(image) - offset:
        raise ValueError(f'{what} outside module image')


def validate_layout(layout, data_start, data_end, file_size):
    required = ('TEXT_END', 'RO_OFF', 'DATA_OFF', 'KEY_PAGE_OFF', 'PATCH_OFF', 'PATCH_SIZE', 'IMAGE_SIZE')
    if any(key not in layout for key in required) or layout.get('PATCH_LEAD', 0):
        raise ValueError('unsupported or incomplete rewrite layout')
    if not (0 < layout['TEXT_END'] <= layout['RO_OFF'] <= layout['DATA_OFF'] == data_start
            <= layout['KEY_PAGE_OFF'] == data_end):
        raise ValueError('rewrite segments do not match original module data')
    if (layout['KEY_PAGE_OFF'] % 0x4000 or layout['IMAGE_SIZE'] != data_end + 0x4000
            or layout['PATCH_OFF'] < layout['IMAGE_SIZE'] or layout['PATCH_SIZE'] <= 0
            or layout['PATCH_OFF'] + layout['PATCH_SIZE'] != file_size):
        raise ValueError('invalid key-page or patch layout')


def parse_metadata(path, image, data_start, data_end):
    module = mod0(image)
    dynamic = module + struct.unpack_from('<i', image, module + 4)[0]
    tags = {}
    for offset in range(dynamic, dynamic + 4096 * 16, 16):
        checked_region(image, offset, 16, 'dynamic entry')
        tag, value = struct.unpack_from('<qQ', image, offset)
        if not tag:
            break
        tags[tag] = value
    else:
        raise ValueError('unterminated dynamic section')
    if tags.get(11) != 24 or tags.get(9, 24) != 24:
        raise ValueError('unsupported ELF table entry size')
    if 4 in tags:
        checked_region(image, tags[4], 8, 'symbol hash table')
        count = struct.unpack_from('<I', image, tags[4] + 4)[0]
    else:
        with path.open('rb') as source:
            header = source.read(0x100)
        if len(header) != 0x100:
            raise ValueError('truncated NSO header')
        ro = struct.unpack_from('<I', header, 0x24)[0]
        _, _, relative, size = struct.unpack_from('<IIII', header, 0x90)
        if size % 24 or ro + relative != tags[6]:
            raise ValueError('NSO and dynamic symbol tables disagree')
        count = size // 24
    if count > 1_000_000:
        raise ValueError('unreasonable dynamic symbol count')
    checked_region(image, tags[6], count * 24, 'symbol table')
    checked_region(image, tags[5], tags[10], 'string table')
    symbols = []
    for index in range(count):
        name, info, other, section, value, size = struct.unpack_from('<IBBHQQ', image, tags[6] + index * 24)
        if name >= tags[10] and name != 0:
            raise ValueError('symbol name outside string table')
        if name:
            start = tags[5] + name
            end = image.index(0, start, tags[5] + tags[10])
            text = image[start:end].decode('latin1')
        else:
            text = ''
        symbols.append({'name': text, 'binding': info >> 4, 'type': info & 15,
                        'visibility': other & 3, 'section_index': section,
                        'defined': bool(section), 'value': value, 'size': size})
    items = [{'offset': where, 'kind': 1027, 'symbol': 0, 'addend': addend}
             for where, addend in decode_relr(image, tags, data_start, data_end)]
    for table_tag, size_tag in ((7, 8), (23, 2)):
        if table_tag not in tags and size_tag not in tags:
            continue
        if table_tag not in tags or size_tag not in tags or tags[size_tag] % 24:
            raise ValueError('invalid relocation table')
        if table_tag == 23 and tags.get(20) != 7:
            raise ValueError('unsupported PLT relocation encoding')
        checked_region(image, tags[table_tag], tags[size_tag], 'relocation table')
        for offset in range(tags[table_tag], tags[table_tag] + tags[size_tag], 24):
            where, info, addend = struct.unpack_from('<QQq', image, offset)
            kind, symbol = info & 0xffffffff, info >> 32
            if symbol >= len(symbols) or (kind == 1027 and symbol != 0):
                raise ValueError('invalid relocation symbol index')
            items.append({'offset': where, 'kind': kind, 'symbol': symbol, 'addend': addend})
    validate_relocations([(item['offset'], item['kind'], symbols[item['symbol']]['name'],
                           symbols[item['symbol']]['value'] if symbols[item['symbol']]['defined'] else None,
                           item['addend']) for item in items], data_start, data_end)
    return {'symbols': symbols, 'relocations': items}


def verify_rewrite_report(exefs, rewritten):
    hashes = verify_exefs(exefs)
    report = json.loads((rewritten / 'report.json').read_text())
    if report.get('options') != {'ruleset': 'native', 'private_gates': False,
                                  'tmnt_graphics_gates': True}:
        raise ValueError('rewrite report has incompatible options')
    tools = GAME / 'third_party/d2r'
    if report.get('tools') != {name: sha256(tools / name) for name in TOOL_SOURCES}:
        raise ValueError('rewriter sources do not match report')
    if (report.get('dependencies') != {name: importlib.metadata.version(name) for name in ('capstone', 'lz4')}
            or report.get('python') != sys.version.split()[0]):
        raise ValueError('rewriter dependency versions do not match report')
    if set(report.get('modules', {})) != set(MODULES):
        raise ValueError('rewrite report does not cover every required module')
    for name in MODULES:
        item = report['modules'][name]
        if item.get('exit_code') != 0 or item.get('input_sha256') != hashes[name]:
            raise ValueError(f'{name}: rewrite is missing, failed, or has mismatched input')
        expected_outputs = set(OUTPUTS) | ({MAIN_RULE_OUTPUT} if name == 'main' else set())
        if set(item.get('outputs', {})) != expected_outputs:
            raise ValueError(f'{name}: rewrite outputs are incomplete')
        for suffix, digest in item['outputs'].items():
            if sha256(rewritten / f'{name}.{suffix}') != digest:
                raise ValueError(f'{name}: rewrite output hash mismatch')
    return hashes, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    parser.add_argument('--rewritten', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='new private JSON file outside this checkout')
    args = parser.parse_args()
    exefs, rewritten, out = args.exefs.resolve(), args.rewritten.resolve(), args.out.resolve()
    if out == REPO or out.is_relative_to(REPO):
        parser.error('output must be outside this repository')
    hashes, report = verify_rewrite_report(exefs, rewritten)
    modules = {}
    for name in MODULES:
        image = load(exefs / name, bss=False)
        data_start, data_end = data_bounds(exefs / name)
        layout = rewrite_layout(rewritten, name)
        file_size = (rewritten / f'{name}.patched.bin').stat().st_size
        validate_layout(layout, data_start, data_end, file_size)
        modules[name] = {'layout': layout, 'file_size': file_size,
                         'rewritten_sha256': report['modules'][name]['outputs']['patched.bin'],
                         **parse_metadata(exefs / name, image, data_start, data_end)}
    document = {'title_id': TITLE, 'input_sha256': {name: hashes[name] for name in MODULES},
                'tools': report['tools'], 'options': report['options'], 'modules': modules}
    hashes_after, report_after = verify_rewrite_report(exefs, rewritten)
    if hashes_after != hashes or report_after != report:
        raise ValueError('input or rewrite report changed during metadata preparation')
    with out.open('x') as destination:
        json.dump(document, destination, separators=(',', ':'))
    print(f'Checked private metadata prepared for {len(modules)} modules.')


if __name__ == '__main__':
    main()
