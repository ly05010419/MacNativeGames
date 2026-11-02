#!/usr/bin/env python3
"""The verified shared-font manifest for the runtime: build/game-data/<version>/shared-fonts.txt (file names, sizes, SHA-256),
read at start (runtime/shared_font.c, runtime/game_data.h); the files stay in extracted/firmware/shared-fonts."""
import argparse,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--version', choices=('v0','v112','v121','v143','botw160','botw190'), default='v0')
version=parser.parse_args().version
folder=root/'extracted/firmware/shared-fonts';manifest=json.loads((folder/'manifest.json').read_text())
assert manifest['status']=='pass' and [f['type'] for f in manifest['fonts']]==list(range(6))
lines=[]
for f in manifest['fonts']:
    path=folder/f"{f['type']}-{f['name']}.bfttf";assert path.stat().st_size==f['bfttf_bytes']==f['font_bytes']+8 and ' ' not in path.name
    i=f['type'];lines.append(f"FONT{i}_FILE {path.name}\nFONT{i}_FILE_BYTES {f['bfttf_bytes']}\nFONT{i}_FONT_BYTES {f['font_bytes']}\nFONT{i}_SHA256 {f['sha256']}\n")
out=root/f'build/game-data/{version}';out.mkdir(parents=True,exist_ok=True)
(out/'shared-fonts.txt').write_text(''.join(lines))
print(json.dumps({'fonts':len(lines)}))
