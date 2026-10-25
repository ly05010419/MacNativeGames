#!/usr/bin/env python3
"""The app icon from the game's own control data: the control NCA in the XCI or NSP carries
icon_<language>.dat (a 256x256 JPEG); it becomes build/AppIcon.icns (sips and iconutil, both part of macOS).
usage: extract_app_icon.py <game.xci|game.nsp> --keys <prod.keys> [--title 010061d00db74000] [--output build/AppIcon.icns]
       [--info game.json]: also the game's name and display version from control.nacp, as JSON"""
import argparse
import json
import subprocess
import tempfile
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
        controls = [r for r, _ in nca_inventory(f, entries, keys)
                    if r['content_type'] == 2 and r['title_id'].lower() == title.lower()]
        require(controls, 'No control NCA for this title in the game file')
        return [c['nca_offset'] for c in controls], tickets   # a card with the base game and its update holds two


def display_version(image, entries):
    """The control data's display version as numbers, for picking the newest of several ("1.4.0" > "1.0.0")."""
    if 'control.nacp' not in entries:
        return ()
    with image.open('rb') as f:
        text = read_at(f, entries['control.nacp']['offset'] + 0x3060, 16).split(b'\0')[0].decode('ascii', 'replace')
    return tuple(int(p) for p in text.split('.') if p.isdigit())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--title', default='010061d00db74000')
    ap.add_argument('--output', type=Path, default=Path('build/AppIcon.icns'))
    ap.add_argument('--info', type=Path)
    a = ap.parse_args()
    keys = load_keys(a.keys)
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        offsets, tickets = control_nca(a.game, keys, a.title)
        found = []
        for n, offset in enumerate(offsets):
            image = t / f'control-{n}.romfs'
            nca_romfs(Slice(a.game, offset), keys, image, tickets)
            entries = {e['path']: e for e in index_romfs(image)}
            found.append((display_version(image, entries), n, image, entries))
        _, _, image, entries = max(found)   # the newest control data: the update's on a combined card
        if a.info and 'control.nacp' in entries:
            with image.open('rb') as f:
                nacp = read_at(f, entries['control.nacp']['offset'], 0x3070)
            # 16 title entries of 0x300 bytes (name, publisher) in LANGUAGES order, then the display version at 0x3060
            names = [nacp[i * 0x300:i * 0x300 + 0x200].split(b'\0')[0].decode('utf-8', 'replace') for i in range(16)]
            a.info.write_text(json.dumps({'name': next((n for n in names if n), ''),
                                          'version': nacp[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace')}) + '\n')
        icon = next((entries[f'icon_{lang}.dat'] for lang in LANGUAGES if f'icon_{lang}.dat' in entries), None)
        require(icon is not None, 'no icon in the control data')
        with image.open('rb') as f:
            (t / 'icon.jpg').write_bytes(read_at(f, icon['offset'], icon['size']))
        iconset = t / 'AppIcon.iconset'
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png'
                subprocess.run(['sips', '-s', 'format', 'png', '-z', str(px), str(px), t / 'icon.jpg', '--out', iconset / name],
                               check=True, capture_output=True)
        a.output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', a.output], check=True)
    print(f'{a.output}')


if __name__ == '__main__':
    main()
