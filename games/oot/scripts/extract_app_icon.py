#!/usr/bin/env python3
"""The app icon from the game's own control data: the control NCA in the XCI or NSP carries
icon_<language>.dat (a 256x256 JPEG); it becomes build/AppIcon.icns (sips and iconutil, both part of macOS).
usage: extract_app_icon.py <game.xci|game.nsp> --keys <prod.keys> [--title 0100f2c0115b6000] [--output build/AppIcon.icns]"""
import argparse
import hashlib
import json
import subprocess
import tempfile
import urllib.request
from pathlib import Path
from extract_exefs import read_at, require, container_entries, common_tickets, nca_inventory
from extract_romfs import index_romfs
from extract_shared_fonts import Slice, load_keys, nca_romfs

LANGUAGES = ['AmericanEnglish', 'BritishEnglish', 'Japanese', 'French', 'German', 'LatinAmericanSpanish', 'Spanish',
             'Italian', 'Dutch', 'CanadianFrench', 'Portuguese', 'Russian', 'Korean', 'TraditionalChinese', 'SimplifiedChinese']


def control_nca(game, keys, title):
    with game.open('rb') as f:
        _, entries = container_entries(f, game.stat().st_size)
        tickets = common_tickets(f, entries)
        records = nca_inventory(f, entries, keys)
        controls = [r for r, _ in records if r['content_type'] == 2 and r['title_id'].lower() == title.lower()]
        require(controls, 'Expected a control NCA for this title in the game file')
        if len(controls) > 1:
            # a card image holding the base game and its update (Breath of the Wild) has a control NCA for each: the
            # update's follows its Program NCA (the one with a BKTR section) in the file; else the last one
            bktr = [i for i, (r, h) in enumerate(records) if r['content_type'] == 0 and r['title_id'].lower() == title.lower()
                    and any(h[0x400 + s * 512 + 4] == 4 for s in range(4))]
            later = [r for i, (r, _) in enumerate(records) if r in controls and bktr and i > bktr[0]]
            controls = later[:1] or controls[-1:]
        return controls[0]['nca_offset'], tickets


def write_icon(image, output):
    with tempfile.TemporaryDirectory() as tmp:
        iconset = Path(tmp) / 'AppIcon.iconset'
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', str(px), str(px), image, '--out', iconset / name],
                               check=True, capture_output=True)
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', output], check=True)


def source_icon(spec, output):
    """A dump without Control data can use a pinned official image; keep image bytes outside the source tree."""
    source = json.loads(spec.read_text())
    image = output.with_suffix('.source.jpg')
    receipt = output.with_suffix('.source.json')
    def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
    if not image.is_file() or digest(image) != source['sha256']:
        with urllib.request.urlopen(source['url'], timeout=30) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != source['sha256']:
            raise ValueError('official app icon hash mismatch')
        image.parent.mkdir(parents=True, exist_ok=True)
        image.write_bytes(data)
    old = json.loads(receipt.read_text()) if receipt.is_file() else {}
    if not output.is_file() or old.get('source_sha256') != source['sha256'] or old.get('icon_sha256') != digest(output):
        write_icon(image, output)
    receipt.write_text(json.dumps(dict(source_sha256=source['sha256'], icon_sha256=digest(output),
                                      source_url=source['url']), indent=2) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path, nargs='?')
    ap.add_argument('--keys', type=Path)
    ap.add_argument('--source-spec', type=Path, help='pinned official image metadata for a dump without Control data')
    ap.add_argument('--title', default='0100f2c0115b6000')
    ap.add_argument('--output', type=Path, default=Path('build/AppIcon.icns'))
    a = ap.parse_args()
    if a.source_spec:
        source_icon(a.source_spec, a.output)
        print(a.output)
        return
    if a.game is None or a.keys is None:
        ap.error('game and --keys are required unless --source-spec is given')
    keys = load_keys(a.keys)
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        image = t / 'control.romfs'
        offset, tickets = control_nca(a.game, keys, a.title)
        nca_romfs(Slice(a.game, offset), keys, image, tickets)
        entries = {e['path']: e for e in index_romfs(image)}
        icon = next((entries[f'icon_{lang}.dat'] for lang in LANGUAGES if f'icon_{lang}.dat' in entries), None)
        require(icon is not None, 'no icon in the control data')
        with image.open('rb') as f:
            (t / 'icon.jpg').write_bytes(read_at(f, icon['offset'], icon['size']))
        write_icon(t / 'icon.jpg', a.output)
    print(f'{a.output}')


if __name__ == '__main__':
    main()
