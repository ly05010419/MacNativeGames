#!/usr/bin/env python3
"""Build a bounded binary index for the native read-only RomFS backend, and the image's size and hashes the runtime checks
it against: build/game-data/<version>/romfs-config.txt (aoc-config.txt with --aoc), read at start (runtime/game_data.h)."""
import argparse,hashlib,json,struct
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--version', choices=('v0','v112','v121','v143','botw160','botw190'), default='v0')
parser.add_argument('--aoc', action='store_true', help='Breath of the Wild: the add-on content volumes extracted/<version>/aoc/<index> -> aoc-config.txt')
args=parser.parse_args();version=args.version
out=root/f'build/game-data/{version}';out.mkdir(parents=True,exist_ok=True)
if args.aoc:
    # One volume per add-on content index (games/botw/scripts/extract_aoc_romfs.py); a missing index has size 0 and is not listed.
    lines=[];present=[]
    for index in (1,2):
        folder=root/f'extracted/{version}/aoc/{index}';size=0;sha='';isha=''
        if (folder/'index.json').exists():
            manifest=json.loads((folder/'index.json').read_text())
            assert manifest['status']=='pass' and manifest['index']==index and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
            strings=bytearray();rows=bytearray()
            for item in manifest['files']:
                name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
            index_bytes=struct.pack('<8sQQ',b'TOTKRFS1',len(manifest['files']),24+len(rows))+rows+strings
            (folder/'runtime-index.bin').write_bytes(index_bytes)
            size=manifest['romfs_size'];sha=manifest['romfs_sha256'];isha=hashlib.sha256(index_bytes).hexdigest();present.append(index)
            lines.append(f'AOC{index}_IMAGE_SIZE {size}\nAOC{index}_IMAGE_SHA256 {sha}\nAOC{index}_INDEX_SHA256 {isha}\n')
    (out/'aoc-config.txt').write_text(f'AOC_PRESENT_MASK {sum(1<<(i-1) for i in present)}\n'+''.join(lines))
    print(json.dumps({'aoc_present':present}));raise SystemExit
folder=root/f'extracted/{version}/romfs';manifest=json.loads((folder/'index.json').read_text())
assert manifest['status']=='pass' and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
strings=bytearray();rows=bytearray()
for item in manifest['files']:
    name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
index=struct.pack('<8sQQ',b'TOTKRFS1',len(manifest['files']),24+len(rows))+rows+strings
(folder/'runtime-index.bin').write_bytes(index)
(out/'romfs-config.txt').write_text(f'ROMFS_IMAGE_SIZE {manifest["romfs_size"]}\nROMFS_IMAGE_SHA256 {manifest["romfs_sha256"]}\nROMFS_INDEX_SHA256 {hashlib.sha256(index).hexdigest()}\n')
print(json.dumps({'files':len(manifest['files']),'index_bytes':len(index)}))
