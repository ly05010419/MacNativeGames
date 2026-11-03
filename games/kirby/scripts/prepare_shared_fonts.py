#!/usr/bin/env python3
"""Bind the verified shared-font manifest into a runtime header (paths, sizes, SHA-256): <work>/build/shared_fonts_config.h
(games/mvdk/scripts/prepare_shared_fonts.py, reading <work>/fonts)."""
import argparse,json
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--work',type=Path,required=True)
work=parser.parse_args().work.resolve()
folder=work/'fonts';manifest=json.loads((folder/'manifest.json').read_text())
assert manifest['status']=='pass' and [f['type'] for f in manifest['fonts']]==list(range(6))
rows=[]
for f in manifest['fonts']:
    path=folder/f"{f['type']}-{f['name']}.bfttf";assert path.stat().st_size==f['bfttf_bytes']==f['font_bytes']+8
    rows.append(f'{{"{path}",{f["bfttf_bytes"]}ULL,{f["font_bytes"]}ULL,"{f["sha256"]}"}}')
(work/'build').mkdir(parents=True,exist_ok=True)
(work/'build/shared_fonts_config.h').write_text('#define SHARED_FONT_COUNT 6\n#define SHARED_FONT_ROWS '+','.join(rows)+'\n')
print(json.dumps({'fonts':len(rows)}))
