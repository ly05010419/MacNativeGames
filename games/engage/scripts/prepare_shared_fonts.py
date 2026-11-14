#!/usr/bin/env python3
"""Bind the verified shared-font manifest into a runtime header (paths, sizes, SHA-256)."""
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
folder=root/'extracted/firmware/shared-fonts';manifest=json.loads((folder/'manifest.json').read_text())
assert manifest['status']=='pass' and [f['type'] for f in manifest['fonts']]==list(range(6))
rows=[]
for f in manifest['fonts']:
    path=folder/f"{f['type']}-{f['name']}.bfttf";assert path.stat().st_size==f['bfttf_bytes']==f['font_bytes']+8
    rows.append(f'{{"{path}",{f["bfttf_bytes"]}ULL,{f["font_bytes"]}ULL,"{f["sha256"]}"}}')
(root/'build/shared_fonts_config.h').write_text('#define SHARED_FONT_COUNT 6\n#define SHARED_FONT_ROWS '+','.join(rows)+'\n')
print(json.dumps({'fonts':len(rows)}))
