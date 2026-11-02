#!/usr/bin/env python3
"""Verify and prepare any of the pinned, owner-supplied ACNH add-on packages."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
GAME = Path(__file__).resolve().parents[1]
ROOT = GAME.parents[1]
sys.path.insert(0, str(ROOT))
from games.acnh.tools.common import clone_file, outside_repository, verified_file
from games.acnh.tools.extract_exefs import common_tickets, container_entries, load_keys, nca_inventory
from games.acnh.tools.extract_romfs import index_romfs
from games.acnh.tools.identity import verified_cnmt


def pins():
    return json.loads((GAME / "assets/expected-dlc-v0.json").read_text())


def verify_assets(folder):
    """Recheck pinned images; derive file ranges from their verified bytes, never a caller's index."""
    expected = pins()
    verified_file(folder / "manifest.json")
    manifest = json.loads((folder / "manifest.json").read_text())
    entries = manifest.get("entries")
    ids = [row.get("title_id") for row in entries] if isinstance(entries, list) and all(isinstance(row, dict) for row in entries) else None
    # Any non-empty subset of the pinned add-ons, each row exactly as pinned and in pinned order.
    if (manifest.get("schema") != 1 or manifest.get("title_id") != expected["title_id"] or not ids
            or len(set(ids)) != len(ids)
            or entries != [row for row in expected["entries"] if row["title_id"] in ids]):
        raise ValueError("DLC manifest differs from the pinned add-ons")
    files = []
    for row in manifest["entries"]:
        image = folder / f'{row["title_id"]}.romfs'
        verified_file(image, row["romfs_sha256"])
        if image.stat().st_size != row["romfs_size"]:
            raise ValueError("DLC RomFS size differs")
        entries = index_romfs(image)
        if len(entries) != row["file_count"]:
            raise ValueError("DLC file count differs")
        files.append(entries)
    return manifest, files


def config_header(folder):
    manifest, files = verify_assets(folder)
    digest = verified_file(folder / "manifest.json")
    lines = [f'#define ACNH_DLC_COUNT {len(files)}',
             f'#define ACNH_DLC_DEFAULT_DIR {json.dumps(str(folder.resolve()))}',
             f'#define ACNH_DLC_MARKER "MNG_ACNH_DLC:{digest}"']
    for i, entries in enumerate(files):
        lines.append(f'static const DlcFile dlc_files_{i}[] = {{')
        lines.extend('{' + f'{json.dumps(e["path"])},{e["offset"]}ULL,{e["size"]}ULL' + '},' for e in entries)
        lines.append('};')
    lines.append('static const DlcImage dlc_images[] = {')
    for i, row in enumerate(manifest["entries"]):
        lines.append('{' + f'{row["index"]},"{row["title_id"]}",{row["romfs_size"]}ULL,'
                     f'"{row["romfs_sha256"]}",dlc_files_{i},{len(files[i])}' + '},')
    lines.append('};')
    return '\n'.join(lines) + '\n', digest


def prepare(sources, keys_path, output):
    output = outside_repository(output, ROOT)
    if output.exists():
        raise ValueError("DLC output already exists; use a new folder")
    expected = pins()
    wanted = {r["source_sha256"]: r for r in expected["entries"]}
    selected = {}
    keys = load_keys(keys_path)
    for source in sources:
        source = source.expanduser()
        digest = verified_file(source)
        source = source.resolve()
        if digest not in wanted or digest in selected:
            raise ValueError("Unknown or duplicate DLC package; exact pinned hashes required")
        row = wanted[digest]
        if source.stat().st_size != row["source_size"]:
            raise ValueError("DLC package size differs")
        with source.open('rb') as stream:
            kind, entries = container_entries(stream, source.stat().st_size)
            records = nca_inventory(stream, entries, keys)
            if (kind != 'nsp' or sorted(r['content_type'] for r, _ in records) != [1, 5]
                    or any(r['title_id'] != row['title_id'] for r, _ in records)):
                raise ValueError("Expected a separate single-title DLC Meta/PublicData NSP")
            meta, header = next((r, h) for r, h in records if r['content_type'] == 1)
            cnmt = verified_cnmt(stream, meta, header, keys, common_tickets(stream, entries), 0x82)
            if (cnmt is None or cnmt['application_id'] != expected['title_id']
                    or cnmt['cnmt_version_raw'] != row['version_raw']):
                raise ValueError("DLC CNMT version/application differs")
        selected[digest] = source
    chosen = [row for row in expected['entries'] if row['source_sha256'] in selected]
    if not chosen:
        raise ValueError("Provide at least one pinned DLC package")
    output.mkdir(parents=True)
    provenance = []
    for row in chosen:
        source = selected[row['source_sha256']]
        with tempfile.TemporaryDirectory(dir=output) as temp:
            extracted = Path(temp) / 'extracted'
            subprocess.run([sys.executable, '-B', str(GAME / 'tools/extract_romfs.py'),
                            '--source', str(source), '--title-id', row['title_id'], '--keys', str(keys_path),
                            '--output', str(extracted)], check=True)
            clone_file(extracted / 'romfs.bin', output / f'{row["title_id"]}.romfs')
        verified_file(source, row['source_sha256'])
        provenance.append({'path': str(source), 'sha256': row['source_sha256'], 'title_id': row['title_id']})
    (output / 'manifest.json').write_text(json.dumps({'schema': 1, 'title_id': expected['title_id'],
                                                    'entries': chosen}, indent=2) + '\n')
    verify_assets(output)
    (output / 'provenance.json').write_text(json.dumps({'sources': provenance,
        'integrity': 'Pinned NSP SHA-256, CNMT hash chains, FS headers and all six IVFC levels verified; RSA not checked'}, indent=2) + '\n')
    print(f'Prepared and verified {len(chosen)} DLC(s): {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', action='append', required=True, type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        prepare(args.source, args.keys.resolve(), args.output)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(2, f'DLC preparation failed: {error}\n')


if __name__ == '__main__':
    main()
