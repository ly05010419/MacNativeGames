#!/usr/bin/env python3
"""Report NSP title-family metadata without printing secrets or game data."""
import argparse
import json
from pathlib import Path
from extract_exefs import container_entries, load_keys, nca_inventory


def application_id(title):
    return f'{int(title, 16) & ~0x1fff:016X}'


def inspect(path, keys):
    with path.open('rb') as source:
        kind, entries = container_entries(source, path.stat().st_size)
        records = nca_inventory(source, entries, keys)
    titles = sorted({record['title_id'] for record, _ in records})
    return {'container': kind, 'title_ids': titles,
            'metadata_ids': sorted({record['title_id'] for record, _ in records if record['content_type'] == 1}),
            'application_ids': sorted({application_id(title) for title in titles})}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('packages', nargs='+', type=Path)
    args = parser.parse_args()
    keys = load_keys(args.keys)
    results = [inspect(path, keys) for path in args.packages]
    for path, result in zip(args.packages, results):
        print(json.dumps({'file': path.name, **result}))
    families = {family for result in results for family in result['application_ids']}
    if len(families) != 1:
        print('Unable to establish one matching application family.')
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
