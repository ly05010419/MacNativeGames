#!/usr/bin/env python3
"""Check a work folder against assets/expected-v<version>.json: ExeFS, rewritten modules and the merged RomFS.

usage: check_expected.py [--work SSBU_build] [--version v1302]
Prints one line per file and exits 1 at the first mismatch or missing file. Reads only; hashes, never game data, are shown.
"""
import argparse, hashlib, json, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
VERSIONS = {'v1302': 'expected-v13.0.2.json', 'v1305': 'expected-v13.0.5.json'}


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(8 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=HERE / 'SSBU_build')
    parser.add_argument('--version', default='v1302', choices=sorted(VERSIONS))
    args = parser.parse_args()
    expected = json.loads((HERE / 'assets' / VERSIONS[args.version]).read_text())
    rows = [(args.work / f'extracted/{args.version}/exefs' / name, digest) for name, digest in expected['exefs_sha256'].items()]
    rows += [(args.work / f'rewritten/{args.version}' / name, digest) for name, digest in expected['rewritten_sha256'].items()]
    rows.append((args.work / f'extracted/{args.version}/romfs/romfs.bin', expected['romfs']['sha256']))
    for path, digest in rows:
        actual = sha256(path) if path.is_file() else None
        print(f'{"ok  " if actual == digest else "FAIL"} {path.relative_to(args.work)}', flush=True)
        if actual != digest:
            sys.exit(f'{path}: ' + ('missing' if actual is None else f'SHA-256 {actual[:16]}... is not {digest[:16]}...'))


if __name__ == '__main__':
    main()
