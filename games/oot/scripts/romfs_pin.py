#!/usr/bin/env python3
"""The RomFS of update 131072, file by file (assets/romfs-v131072.tsv.gz: path, size and the first 16 hex digits of the
file's SHA-256, sorted by path). Identifiers and hashes only, no game data.

Two retail dumps of the same update hold the same files but can pack to different images (#2009, #2011: another
player's NUP, TOC c607…, gave a RomFS image 2,112 bytes larger than ours, TOC 0cb6…, with 186,587 files against our
186,583), so the build checks every listed file instead of the hash of one image: each listed file must be there with
its size and hash; files the list does not name are allowed and named in the log. The runtime still checks the image
this build imported (prepare_romfs_backend.py writes its own hash into romfs-config.txt).

  romfs_pin.py make <romfs folder> <out.tsv.gz>   the list from a verified import (index.json + romfs.bin)
  romfs_pin.py check <romfs folder>               check an import against assets/romfs-v131072.tsv.gz
"""
import gzip
import hashlib
import io
import json
from pathlib import Path
import sys

PIN = Path(__file__).resolve().parents[1] / 'assets/romfs-v131072.tsv.gz'
SHORT = 16   # hex digits kept per file: enough to tell a damaged or different file, a third of the size of whole hashes


def file_hashes(romfs):
    """index.json's rows, each with the file's SHA-256: the importers record it; an import made before they did is read
    from romfs.bin once and the hashes are written into index.json."""
    path = Path(romfs) / 'index.json'
    index = json.loads(path.read_text())
    rows = index['files']
    if all('sha256' in row for row in rows):
        return rows
    with (Path(romfs) / 'romfs.bin').open('rb') as image:
        for row in rows:
            if 'sha256' in row:
                continue
            image.seek(row['offset']); digest = hashlib.sha256(); left = row['size']
            while left:
                chunk = image.read(min(left, 8 << 20))
                if not chunk:
                    raise ValueError(f"RomFS image ends inside {row['path']}")
                digest.update(chunk); left -= len(chunk)
            row['sha256'] = digest.hexdigest()
    path.write_text(json.dumps(index) + '\n')
    return rows


def load_pin(pin=PIN):
    """{path: (size, short hash)}."""
    rows = gzip.decompress(Path(pin).read_bytes()).decode().splitlines()
    return {path: (int(size), short) for path, size, short in (row.split('\t') for row in rows)}


def write_pin(romfs, out):
    rows = sorted(file_hashes(romfs), key=lambda row: row['path'])
    text = ''.join(f"{row['path']}\t{row['size']}\t{row['sha256'][:SHORT]}\n" for row in rows)
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode='wb', mtime=0, compresslevel=9) as f:   # the same bytes from the same files
        f.write(text.encode())
    Path(out).write_bytes(buffer.getvalue())
    return len(rows)


def verified_by_source(romfs):
    """Whether the importer checked every file against the source's own hashes: a NUP's table of contents records each
    file's SHA-256, and import_nup.py stops at the first file that does not match (files_verified_by_source; an import
    made before that flag says so in its integrity line). A dumped folder carries no hashes of its own."""
    index = json.loads((Path(romfs) / 'index.json').read_text())
    return bool(index.get('files_verified_by_source')) or str(index.get('integrity', '')).startswith('NUP TOC')


def named(label, paths):
    return f'{label} {len(paths)} ({", ".join(paths[:10])}{", …" if len(paths) > 10 else ""})'


def check(romfs, pin=PIN):
    """{'files': listed files found, 'extra': paths the list does not name, 'variant': listed files of the listed size with
    other bytes}. Every listed file must be there at its listed size: a missing one or one of another size is a ValueError
    (another version or an incomplete import). One of the listed size with other bytes is another retail package of
    131072 when the source verified it (#2011, 2026-11-11: a NUP with 9 such files, each matching its own table of
    contents, whose game plays): it passes and is named. From a dumped folder, which carries no hashes, it cannot be told
    from a damaged file, so it is a ValueError that says so."""
    want = load_pin(pin)
    have = {row['path']: (row['size'], row['sha256'][:SHORT]) for row in file_hashes(romfs)}
    missing = sorted(p for p in want if p not in have)
    resized = sorted(p for p in want if p in have and have[p][0] != want[p][0])
    different = sorted(p for p in want if p in have and have[p][0] == want[p][0] and have[p][1] != want[p][1])
    if missing or resized:
        raise ValueError('RomFS is not update 131072: '
                         + '; '.join(named(label, paths) for label, paths in (('missing', missing), ('other size', resized),
                                                                              ('different', different)) if paths)
                         + ' — the dump is incomplete or of another version: dump the game again (docs/install.md)')
    if different and not verified_by_source(romfs):
        raise ValueError('RomFS is not update 131072 as listed: ' + named('different', different)
                         + ' — a dumped folder has no hashes of its own, so these cannot be told from damaged files: '
                         'build from the game\'s .nup instead, whose own hashes are checked (docs/install.md)')
    return dict(files=len(want), extra=sorted(p for p in have if p not in want), variant=different)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'make':
        print(f'{write_pin(sys.argv[2], sys.argv[3])} files listed in {sys.argv[3]}')
    elif len(sys.argv) == 3 and sys.argv[1] == 'check':
        report = check(sys.argv[2])
        print(json.dumps(report))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
