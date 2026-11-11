#!/usr/bin/env python3
"""Add the observed pins to assets/expected-1.0.1.json, once, after the extraction, composition and integrity evidence pass.
They say only that a later build reads the same files as this first one; they prove nothing about that first one, which is what the evidence is for."""
import argparse
import json
from pathlib import Path
import sys

from inspect_inputs import EXPECTED, inspect


def require(path):
    result = json.loads(path.read_text())
    if result.get('status') != 'pass':
        raise ValueError(f'evidence did not pass: {path}')


def observed(report):
    romfs = {k: v for k, v in report['romfs'].items() if k != 'entries'}
    return dict(modules=report['modules'], npdm=report['npdm'], romfs=romfs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', type=Path)
    parser.add_argument('--evidence', required=True, type=Path, nargs='+', help="each step's result.json")
    args = parser.parse_args()
    pins = json.loads(EXPECTED.read_text())
    if 'observed' in pins:
        raise ValueError('observed pins exist already: they are written once')
    for path in args.evidence:
        require(path)
    pins['observed'] = observed(inspect(args.inputs))
    EXPECTED.write_text(json.dumps(pins, indent=2)+'\n')
    print(f'observed pins written: {pins["observed"]["romfs"]["files"]} RomFS files')


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError) as error: sys.exit(f'error: {error}')
