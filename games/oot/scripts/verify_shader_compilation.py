#!/usr/bin/env python3
"""Reproduce four resource-free original pixel shader translations on Metal."""
import hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1];source=root/'extracted/v0/shaders-v2';work=root/'build/shader-trial';out=work/'spv';out.mkdir(parents=True,exist_ok=True)
inv=json.loads((source/'inventory.json').read_text());unique={x['key']:x for x in inv['rows'] if x['stage']=='ps'}
selected=sorted(unique.values(),key=lambda x:(x['code_size'],x['key']))[:4];jobs=[]
for row in selected:
 key=row['key'];control=(source/(key+'.control')).read_bytes();code=(source/(key+'.code')).read_bytes()
 assert hashlib.sha256(control+code).hexdigest()==key
 path=work/(key+'.bin');path.write_bytes(code[0x30:])
 jobs.append(dict(key=key,stage='ps',code=str(path),c1='',debug=False,reduced=False,tex={},img={},attr={},fout={}))
job=work/'jobs.json';job.write_text(json.dumps(jobs,indent=2)+'\n')
subprocess.run([str(root/'tooling/dotnet/dotnet'),str(root/'build/dmsh_xlate/dmsh_xlate.dll'),str(job),str(out)],check=True,cwd=root)
metal=[]
for row in selected:
 key=row['key'];reflection=json.loads((out/(key+'.res.json')).read_text())
 assert not reflection['notes'] and all(not reflection[field] for field in ['cbufs','sbufs','textures','images'])
 path=out/(key+'.metal');subprocess.run([str(root/'build/spirv-cross/spirv-cross'),str(out/(key+'.spv')),'--msl','--msl-version','20300','--output',str(path)],check=True);metal.append(path)
probe=root/'build/metal-shader-probe'
subprocess.run(['xcrun','clang','-O1','-fobjc-arc','-Wno-deprecated-declarations',str(root/'scripts/metal_shader_probe.m'),'-framework','Foundation','-framework','Metal','-o',str(probe)],check=True)
run=subprocess.run([str(probe)]+[str(x) for x in metal],check=True,capture_output=True,text=True);report=json.loads(run.stdout)
report['keys']=[x['key'] for x in selected];report['source_sha256']={x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in metal}
(root/'analysis/metal-shader-probe.json').write_text(json.dumps(report,indent=2)+'\n');print(run.stderr,end='');print(json.dumps(report))
