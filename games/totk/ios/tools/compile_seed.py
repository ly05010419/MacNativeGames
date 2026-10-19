#!/usr/bin/env python3
"""Compile the verified startup MSL seed into device metallibs and refresh its manifest."""
import concurrent.futures, hashlib, json, os, subprocess, sys, tempfile
from pathlib import Path

# The options of totk_precise_opts() in nvn_mtl.m: the translated shaders need exactly rounded float arithmetic. The
# offline compiler defaults to fast math ("Default is 'fast'"), which breaks their float integer-division idioms.
METAL_FLAGS=['-fmetal-math-mode=safe']

def main():
  seed=Path(sys.argv[1]).resolve(); sdk=sys.argv[2]
  manifest=json.loads((seed/'seed-manifest.json').read_text())
  if manifest.get('chain') is None or not manifest.get('files'): raise SystemExit('invalid shader seed manifest')
  files=sorted(n for n in manifest['files'] if n.endswith('_b.metal'))
  def compile_one(name):
    src=seed/name; air=seed/(name+'.air'); lib=seed/(name.removesuffix('.metal')+'.metallib')
    subprocess.run(['xcrun','-sdk',sdk,'metal',*METAL_FLAGS,'-c',str(src),'-o',str(air)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    subprocess.run(['xcrun','-sdk',sdk,'metallib',str(air),'-o',str(lib)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    air.unlink()
    return lib.name,hashlib.sha256(lib.read_bytes()).hexdigest()
  results=[]
  with concurrent.futures.ThreadPoolExecutor(max_workers=min(8,os.cpu_count() or 4)) as pool:
    for i,result in enumerate(pool.map(compile_one,files),1):
        results.append(result)
        if i%100==0: print(f'compiled {i}/{len(files)} iOS metallibs',flush=True)
  for name,digest in results: manifest['files'][name]=digest
  manifest['ios_metallibs']=len(results)
  manifest['ios_metal_flags']=' '.join(METAL_FLAGS)   # build_ipa.py reuses the libraries only when these match
  manifest['files']={n:hashlib.sha256((seed/n).read_bytes()).hexdigest() for n in sorted(manifest['files'])}
  (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
  print(f'compiled {len(results)} iOS metallibs')

if __name__=='__main__': main()
