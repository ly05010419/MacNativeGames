#!/usr/bin/env python3
"""Embed the locally verified TMNT NRO in signed iOS executable sections.

The output contains game code and must stay in a private build directory.
"""
import argparse
import hashlib
import json
from pathlib import Path

PAGE = 0x4000


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def page_up(value):
    return (value + PAGE - 1) & ~(PAGE - 1)


def generate(prepared, rewritten, output):
    repository_game = Path(__file__).resolve().parents[2]
    if output == repository_game or repository_game in output.parents:
        raise ValueError('generated NRO game code must stay outside the repository')
    manifest = json.loads((prepared / 'manifest.json').read_text())
    report = json.loads((rewritten / 'report.json').read_text())
    if manifest.get('status') != 'pass' or report.get('exit_code') != 0:
        raise ValueError('NRO preparation or rewrite did not pass')
    if report.get('options') != {'ruleset': 'native', 'private_gates': False}:
        raise ValueError('NRO rewrite options mismatch')
    inputs = {'nro': digest(prepared / 'module.nro'),
              'nso': digest(prepared / 'synthetic.nso')}
    if inputs != report.get('input_sha256') or inputs != {
            'nro': manifest['nro_sha256'],
            'nso': manifest['synthetic_nso_sha256']}:
        raise ValueError('NRO input differs from verified preparation')
    required = {'patched.bin', 'fixups.tsv', 'gates.tsv'}
    if not required.issubset(report.get('outputs', {})):
        raise ValueError('NRO rewrite report lacks required output hashes')
    for suffix, expected in report['outputs'].items():
        if digest(rewritten / f'nro.{suffix}') != expected:
            raise ValueError(f'NRO {suffix} differs from verified rewrite')
    if (rewritten / 'nro.gates.tsv').stat().st_size:
        raise ValueError('NRO with host gates is not supported')

    values = dict(line[2:].split() for line in
                  (rewritten / 'nro.fixups.tsv').read_text().splitlines()
                  if line.startswith('# '))
    image = rewritten / 'nro.patched.bin'
    size = image.stat().st_size
    text = int(values['TEXT_END'], 16)
    data = int(values['DATA_OFF'], 16)
    key = int(values['KEY_PAGE_OFF'], 16)
    patch = int(values['PATCH_OFF'], 16)
    original_size = (prepared / 'module.nro').stat().st_size
    rx = page_up(text)
    end = page_up(size)
    if not (0 < text <= rx <= data <= key and key % PAGE == 0 and
            key + PAGE <= patch < size and patch % PAGE == 0 and
            original_size <= key and size <= 512 << 20):
        raise ValueError('NRO signed-section layout is invalid')

    sections = [('__TMNTNROX,__nrotext', 0, rx),
                ('__TMNTNROW,__image', rx, patch),
                ('__TMNTNROX,__nropatch', patch, end)]
    assembly = ['// Generated locally from verified TMNT NRO; never commit.']
    hashes = []
    with image.open('rb') as source:
        for section, start, stop in sections:
            assembly.extend((f'    .section {section}', '    .p2align 14'))
            actual = min(stop, size) - start
            assembly.append(f'    .incbin "{image.resolve()}", {start:#x}, {actual:#x}')
            if stop > size:
                assembly.append(f'    .space {stop - size:#x}')
            assembly.append('    .p2align 14')
            source.seek(start)
            contents = source.read(actual) + bytes(stop - start - actual)
            hashes.append(hashlib.sha256(contents).hexdigest())

    header = ('/* Private generated NRO section fingerprints; never commit. */\n'
              f'#define TMNT_IOS_NRO_FILE_SIZE {size:#x}u\n'
              f'#define TMNT_IOS_NRO_TEXT_END {text:#x}u\n'
              f'#define TMNT_IOS_NRO_DATA_OFF {data:#x}u\n'
              f'#define TMNT_IOS_NRO_KEY_OFF {key:#x}u\n'
              f'#define TMNT_IOS_NRO_PATCH_OFF {patch:#x}u\n'
              f'#define TMNT_IOS_NRO_TEXT "{hashes[0]}"\n'
              f'#define TMNT_IOS_NRO_DATA "{hashes[1]}"\n'
              f'#define TMNT_IOS_NRO_PATCH "{hashes[2]}"\n')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(assembly) + '\n')
    (output.parent / 'ios_nro_embed.h').write_text(header)
    print('-Wl,-segprot,__TMNTNROW,rw,rw -Wl,-segprot,__TMNTNROX,r-x,r-x')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', required=True, type=Path)
    parser.add_argument('--rewritten', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    generate(args.prepared.resolve(), args.rewritten.resolve(), args.out.resolve())


if __name__ == '__main__':
    main()
