#!/usr/bin/env python3
"""Verify the exact local v112/v143 visibility shader and its atomic side effect on Metal.
1.2.1 runs the very same shader as 1.1.2 (same key and Maxwell bytes, seen in 1.2.1 field sessions on 2026-10-19)."""
import argparse, hashlib, json, os, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CASES={
 'v112':('b1e4d0906d1658f64825f570ed6a2abd08ceeec82d537cd3a5fc1c17f38fb238-641742ac59e3','eabe9fdd6fc737b8414e94cc3e14ec0031ab3a156511953abe5e3a04128524ad','vertex'),
 'v121':('b1e4d0906d1658f64825f570ed6a2abd08ceeec82d537cd3a5fc1c17f38fb238-641742ac59e3','eabe9fdd6fc737b8414e94cc3e14ec0031ab3a156511953abe5e3a04128524ad','vertex'),
 'v143':('1eee2726aae7493ec0506b96a54c89cb18bf6bd98f64c29641aa8e67ed642326-641742ac59e3','f5f4d23149565acd852f6b7091f64cc8cb3aa019ccb33ba3f88ced62c6d1ff7b','instance')}
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--version',choices=CASES,required=True);p.add_argument('--code',type=Path);p.add_argument('--compare-cache',type=Path)
 a=p.parse_args();os.chdir(ROOT);key,digest,mode=CASES[a.version]
 code=(a.code or ROOT/f'build/shader-cache/rt-totk-native-v4-unverified/{key}.code').resolve()
 assert sha(code)==digest,'unrecognised Maxwell shader bytes'
 out=ROOT/f'analysis/{a.version}-port/counter-native';out.mkdir(parents=True,exist_ok=True)
 service=ROOT/'build/shader-service/totk_xlate'
 rq=dict(variant=key,stage='vs',code=str(code),depth_minus_w=True)
 cmd=[str(service),'--service','--spirv-cross',str(ROOT/'build/spirv-cross/spirv-cross'),'--out',str(out),'--chain','totk-native-v4-unverified']
 r=subprocess.run(cmd,input=json.dumps(rq)+'\n',text=True,capture_output=True,check=True);assert json.loads(r.stdout)['ok'],r.stdout
 if a.compare_cache:
  for suffix in ('.spv','_b.metal','_b.json'):
   x=(out/f'{key}{suffix}').read_bytes();y=(a.compare_cache/f'{key}{suffix}').read_bytes()
   assert json.loads(x)==json.loads(y) if suffix.endswith('.json') else x==y,suffix
 subprocess.run(['xcrun','clang','-fobjc-arc','-O1','-Wall','-Wextra','-Werror','scripts/vertex_counter_probe.m','-framework','Metal','-framework','Foundation','-o','build/vertex-counter-probe'],check=True)
 cmd=['python3','scripts/dev/game_slot.py','run','--purpose',f'{a.version} exact visibility counter GPU validation','--expect-seconds','15','--','build/vertex-counter-probe',str(out/f'{key}_b.metal'),mode]
 env=os.environ.copy();env.setdefault('TOTK_SLOT_OWNER','codex')
 r=subprocess.run(cmd,env=env,text=True,capture_output=True,check=True);(out/'gpu.log').write_text(r.stdout+r.stderr)
 report=json.loads(r.stdout);assert report['cases']==96
 neg=subprocess.run(cmd,env=env|{'TOTK_PROBE_NEGATIVE':'1'},text=True,capture_output=True);(out/'gpu-negative.log').write_text(neg.stdout+neg.stderr)
 assert neg.returncode==1 and 'got=15 want=14' in neg.stderr,neg.stderr
 report.update(version=a.version,indexing=mode,code_sha256=digest,translator_sha256=sha(service),probe_source_sha256=sha(ROOT/'scripts/vertex_counter_probe.m'),negative_control='doubled atomic increment rejected',previous_cache_parity=bool(a.compare_cache))
 (out/'validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
if __name__=='__main__':main()
