#!/usr/bin/env python3
"""The index.json of a RomFS image that is already verified, without extracting it again: build_app.py adopt_game_data
takes the image an installed app carries when its SHA-256 is the verified one of the version (expected-<version>.json),
and this writes the same report extract_romfs.py / extract_update_romfs.py write (status, size, SHA-256, files).

usage: index_romfs.py <romfs.bin> --sha256 <the digest build_app.py checked> --source <where the image came from>"""
import argparse
import json
from pathlib import Path

from extract_romfs import index_romfs


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('image', type=Path)
    ap.add_argument('--sha256', required=True)
    ap.add_argument('--source', required=True)
    a = ap.parse_args()
    files = index_romfs(a.image)
    source = Path(a.source).stat()   # build_app.py adoptable_bundle: the app's bundle is taken only while this is unchanged
    report = dict(status='pass', romfs_size=a.image.stat().st_size, romfs_sha256=a.sha256, files=files,
                  adopted_from=a.source, adopted_size=source.st_size, adopted_mtime_ns=source.st_mtime_ns,
                  integrity='taken from an installed app; its SHA-256 equals the verified image')
    (a.image.parent / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status='pass', files=len(files), sha256=a.sha256)), flush=True)


if __name__ == '__main__':
    main()
