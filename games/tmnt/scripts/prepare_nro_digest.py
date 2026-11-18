#!/usr/bin/env python3
"""Generate private NRO fingerprints from the verified TMNT NRO rewrite."""
import argparse
import hashlib
from pathlib import Path

from rewrite_modules import REPO
from rewrite_nro import verify_nro_report


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def generate(prepared, rewritten, out):
    if out == REPO or out.is_relative_to(REPO):
        raise ValueError('generated NRO identity must stay outside the repository')
    verify_nro_report(prepared, rewritten)
    values = {
        'TMNT_NRO_ID': digest(prepared / 'module.nro'),
        'TMNT_NRO_LAYOUT': digest(rewritten / 'nro.fixups.tsv'),
        'TMNT_NRO_GATES': digest(rewritten / 'nro.gates.tsv'),
        'TMNT_NRO_PATCH': digest(rewritten / 'nro.patched.bin'),
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('x') as target:
        target.write('/* Generated from verified private NRO inputs. Do not commit. */\n')
        for name, value in values.items():
            target.write(f'#define {name} "{value}"\n')
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('prepared', 'rewritten', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    generate(*(getattr(args, n).resolve() for n in ('prepared', 'rewritten', 'out')))
    print('Generated private NRO identity; no guest code executed.')


if __name__ == '__main__':
    main()
