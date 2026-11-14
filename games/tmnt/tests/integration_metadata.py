#!/usr/bin/env python3
"""Compare loader metadata against an independent relocation reader without exporting game data."""
import argparse
import collections
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAME / 'scripts'))
from module_metadata import parse_metadata
from rewrite_modules import MODULES, verify_exefs
from survey_modules import data_bounds, load, relocs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', type=Path, required=True)
    exefs = parser.parse_args().exefs.resolve()
    verify_exefs(exefs)
    total = 0
    for name in MODULES:
        image = load(exefs / name, bss=False)
        data = parse_metadata(exefs / name, image, *data_bounds(exefs / name))
        actual = []
        for item in data['relocations']:
            symbol = data['symbols'][item['symbol']]
            symbol_name = symbol['name'] if item['symbol'] else None
            value = symbol['value'] if item['symbol'] and symbol['defined'] else None
            actual.append((item['offset'], item['kind'], symbol_name, value, item['addend']))
        expected = relocs(image)
        if collections.Counter(actual) != collections.Counter(expected):
            raise ValueError(f'{name}: independent relocation data disagrees')
        total += len(actual)
        print(f'{name}: {len(actual)} relocation records agree', flush=True)
    print(f'total: {total} relocation records agree')


if __name__ == '__main__':
    main()
