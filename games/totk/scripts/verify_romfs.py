#!/usr/bin/env python3
"""Compare native filesystem reads with independent local-image reads."""
import argparse,json,subprocess,tempfile,os
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--version',choices=('v0','v112','v121'),default='v0');version=p.parse_args().version
exe=root/('build/totk-native' if version=='v0' else f'build/totk-native-{version}');out=root/('analysis' if version=='v0' else f'analysis/{version}-port')
run=subprocess.run([str(exe),f'rewritten/{version}','probe-fs'],cwd=root,capture_output=True,text=True,check=True)
report=json.loads(run.stdout);files=json.loads((root/f'extracted/{version}/romfs/index.json').read_text())['files']
digest=0;count=0
with (root/f'extracted/{version}/romfs/romfs.bin').open('rb') as image:
    for item in files[::len(files)//64]:
        image.seek(item['offset']);data=image.read(min(257,item['size']))
        for byte in data:digest=(digest*131+byte)&((1<<64)-1)
        count+=1
assert report['sampled_files']==count and report['read_digest']==f'{digest:016x}'
with tempfile.TemporaryDirectory(dir=root/'build') as folder:
    (Path(folder)/'runtime-index.bin').write_bytes(b'invalid index data'*20)
    env=os.environ.copy();env['TOTK_ROMFS']=folder
    failed=subprocess.run([str(exe),f'rewritten/{version}','probe-fs'],cwd=root,capture_output=True,text=True,env=env)
    assert failed.returncode==1 and 'RomFS integrity mismatch' in failed.stderr
report['independent_read_comparison']='pass';report['corrupted_index_negative_control']='rejected'
(out/'native-romfs-probe.json').write_text(json.dumps(report,indent=2)+'\n')
(out/'native-romfs-probe.log').write_text(run.stderr)
print(json.dumps(report))
