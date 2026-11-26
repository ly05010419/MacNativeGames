#!/usr/bin/env python3
"""Compile the verified startup MSL seed into device metallibs and refresh its manifest."""
import concurrent.futures, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

# The options of totk_precise_opts() in nvn_mtl.m: the translated shaders need exactly rounded float arithmetic. The
# offline compiler defaults to fast math ("Default is 'fast'"), which breaks their float integer-division idioms.
METAL_FLAGS=['-fmetal-math-mode=safe']

def main():
  seed=Path(sys.argv[1]).resolve(); sdk=sys.argv[2]
  manifest=json.loads((seed/'seed-manifest.json').read_text())
  if manifest.get('chain') is None or not manifest.get('files'): raise SystemExit('invalid shader seed manifest')
  files=sorted(n for n in manifest['files'] if n.endswith('_b.metal'))
  # A library is kept by the hash of its source, the flags and the compiler (<seed>.ios-metallibs/): the default
  # variants of every shader are 33,000 files, an hour on 8 threads, and most of them are the same in the next build.
  version=subprocess.run(['xcrun','-sdk',sdk,'metal','--version'],check=True,capture_output=True,text=True).stdout
  cache=seed.with_name(seed.name+'.ios-metallibs'); cache.mkdir(exist_ok=True)
  reused=[0]
  def compile_one(name):
    src=seed/name; air=seed/(name+'.air'); lib=seed/(name.removesuffix('.metal')+'.metallib')
    key=hashlib.sha256(src.read_bytes()+'\0'.join([sdk,version,*METAL_FLAGS]).encode()).hexdigest()
    kept=cache/(key+'.metallib')
    if kept.is_file():
        lib.unlink(missing_ok=True); shutil.copyfile(kept,lib); reused[0]+=1
    else:
        subprocess.run(['xcrun','-sdk',sdk,'metal',*METAL_FLAGS,'-c',str(src),'-o',str(air)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        subprocess.run(['xcrun','-sdk',sdk,'metallib',str(air),'-o',str(lib)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        air.unlink()
        tmp=cache/(key+f'.{os.getpid()}.tmp'); shutil.copyfile(lib,tmp); os.replace(tmp,kept)
    return lib.name,hashlib.sha256(lib.read_bytes()).hexdigest()
  results=[]
  with concurrent.futures.ThreadPoolExecutor(max_workers=min(8,os.cpu_count() or 4)) as pool:
    for i,result in enumerate(pool.map(compile_one,files),1):
        results.append(result)
        if i%100==0: print(f'compiled {i}/{len(files)} iOS metallibs',flush=True)
  for name,digest in results: manifest['files'][name]=digest
  manifest['ios_metallibs']=len(results)
  manifest['ios_metal_flags']=' '.join(METAL_FLAGS)   # build_ipa.py reuses the libraries only when these match
  # the seed's own files keep the hashes checked when it was prepared; the metallibs were hashed above
  (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
  print(f'compiled {len(results)} iOS metallibs ({reused[0]} from {cache.name})')

if __name__=='__main__': main()
