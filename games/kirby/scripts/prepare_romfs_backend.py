#!/usr/bin/env python3
"""Build the bounded binary index of the native read-only RomFS backend: <work>/romfs/runtime-index.bin and
<work>/build/romfs_config.h (games/mvdk/scripts/prepare_romfs_backend.py, reading this folder's layout)."""
import argparse,hashlib,json,struct
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--work',type=Path,required=True)
work=parser.parse_args().work
out=work/'build';out.mkdir(parents=True,exist_ok=True)
folder=work/'romfs';manifest=json.loads((folder/'index.json').read_text())
assert manifest['status']=='pass' and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
strings=bytearray();rows=bytearray()
for item in manifest['files']:
    name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
index=struct.pack('<8sQQ',b'TOTKRFS1',len(manifest['files']),24+len(rows))+rows+strings
(folder/'runtime-index.bin').write_bytes(index)
(out/'romfs_config.h').write_text(f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ULL\n#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"]}"\n#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n')
print(json.dumps({'files':len(manifest['files']),'index_bytes':len(index)}))
