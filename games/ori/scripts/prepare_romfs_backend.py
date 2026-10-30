#!/usr/bin/env python3
"""Build a bounded binary index for the native read-only RomFS backend."""
import argparse,hashlib,json,os,struct
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.parse_args();version=os.environ.get('GAME_DIR','v101')
out=root/'build';out.mkdir(parents=True,exist_ok=True)
folder=root/f'extracted/{version}/romfs';manifest=json.loads((folder/'index.json').read_text())
assert manifest['status']=='pass' and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
strings=bytearray();rows=bytearray()
for item in manifest['files']:
    name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
index=struct.pack('<8sQQ',b'TOTKRFS1',len(manifest['files']),24+len(rows))+rows+strings
(folder/'runtime-index.bin').write_bytes(index)
(out/'romfs_config.h').write_text(f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ULL\n#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"]}"\n#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n')
print(json.dumps({'files':len(manifest['files']),'index_bytes':len(index)}))
