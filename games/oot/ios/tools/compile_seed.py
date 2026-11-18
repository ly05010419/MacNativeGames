#!/usr/bin/env python3
"""Compile the verified startup MSL seed into device metallibs and refresh its manifest."""
import concurrent.futures, hashlib, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

# The options of totk_precise_opts() in nvn_mtl.m: the translated shaders need exactly rounded float arithmetic. The
# offline compiler defaults to fast math ("Default is 'fast'"), which breaks their float integer-division idioms.
METAL_FLAGS=['-fmetal-math-mode=safe']
METAL_BRACKET_LIMIT=256
# The app's lowest OS (project.yml deploymentTarget; build_ipa.project_for_platform for visionOS). Without it the compiler
# builds for its own SDK: Xcode 27.x's libraries are AIR for Metal 4.1 (air64_v29), which iOS / iPadOS 26 refuses to load,
# so every shader was compiled from source on the device (Tears of the Kingdom's #1845). -mtargetos names the platform:
# -mios-version-min with the xros SDK targets the iPhone instead ("using sysroot for 'XROS' but targeting 'iPhone'").
MIN_OS='26.0'
TARGET_OS={'iphoneos':'ios','xros':'xros'}
def target_flags(sdk): return [f'-mtargetos={TARGET_OS[sdk]}{MIN_OS}']
def seed_flags(sdk):
  """The options a seed's libraries were compiled with (seed-manifest.json ios_metal_flags; build_ipa.py reuses them only when equal)."""
  return ' '.join([*METAL_FLAGS,*target_flags(sdk)])
def metal_flags(src):
  """METAL_FLAGS, plus a higher parser limit for generated MSL nested past clang's 256 (as build_app.py metal_flags). It changes
  no generated code, so it stays out of METAL_FLAGS and every cache key. Tears of the Kingdom's copy has had it since 07d93f93;
  this one had not, and the first iOS build of every Ocarina of Time shader stopped at one 3.5 MB shader nested deeper
  (2026-11-10: "bracket nesting level exceeded maximum of 256")."""
  depth=deepest=0
  for c in Path(src).read_bytes():
    if c in b'([{': depth+=1; deepest=max(deepest,depth)
    elif c in b')]}': depth-=1
  return [*METAL_FLAGS,'-fbracket-depth=1024'] if deepest>METAL_BRACKET_LIMIT else METAL_FLAGS

def main():
  seed=Path(sys.argv[1]).resolve(); sdk=sys.argv[2]
  manifest=json.loads((seed/'seed-manifest.json').read_text())
  if manifest.get('chain') is None or not manifest.get('files'): raise SystemExit('invalid shader seed manifest')
  files=sorted(n for n in manifest['files'] if n.endswith('_b.metal'))
  # A library is kept by the hash of its source, the flags and the compiler (<seed>.ios-metallibs/): the default
  # variants of every shader are 33,000 files, an hour on 8 threads, and most of them are the same in the next build.
  version=subprocess.run(['xcrun','-sdk',sdk,'metal','--version'],check=True,capture_output=True,text=True).stdout
  cache=seed.with_name(seed.name+'.ios-metallibs'); cache.mkdir(exist_ok=True)
  # Different variants can have identical MSL. Compile each verified content hash once: two workers previously wrote
  # the same PID-named cache temporary and one removed the other's file (Bananza's first full iOS seed).
  groups={}
  for name in files: groups.setdefault(manifest['files'][name],[]).append(name)
  reused=[0]
  def compile_one(name):
    src=seed/name; air=seed/(name+'.air'); lib=seed/(name.removesuffix('.metal')+'.metallib')
    key=hashlib.sha256(src.read_bytes()+'\0'.join([sdk,version,*METAL_FLAGS,*target_flags(sdk)]).encode()).hexdigest()
    kept=cache/(key+'.metallib'); lib.unlink(missing_ok=True)
    if kept.is_file():
        shutil.copyfile(kept,lib); reused[0]+=1
    else:
        subprocess.run(['xcrun','-sdk',sdk,'metal',*metal_flags(src),*target_flags(sdk),'-c',str(src),'-o',str(air)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        subprocess.run(['xcrun','-sdk',sdk,'metallib',str(air),'-o',str(lib)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        air.unlink()
        with tempfile.NamedTemporaryFile(dir=cache,prefix=key+'.',suffix='.tmp',delete=False) as f: tmp=Path(f.name)
        try: shutil.copyfile(lib,tmp); os.replace(tmp,kept)
        finally: tmp.unlink(missing_ok=True)
    return lib.name,hashlib.sha256(lib.read_bytes()).hexdigest()
  def compile_group(names):
    name,digest=compile_one(names[0]); result=[(name,digest)]
    for alias in names[1:]:
        lib=seed/(alias.removesuffix('.metal')+'.metallib'); lib.unlink(missing_ok=True)
        shutil.copyfile(seed/name,lib); result.append((lib.name,digest))
    return result
  results=[]
  with concurrent.futures.ThreadPoolExecutor(max_workers=min(8,os.cpu_count() or 4)) as pool:
    for result in pool.map(compile_group,groups.values()):
        before=len(results);results.extend(result)
        if len(results)//100>before//100: print(f'compiled {len(results)}/{len(files)} iOS metallibs',flush=True)
  for name,digest in results: manifest['files'][name]=digest
  manifest['ios_metallibs']=len(results)
  manifest['ios_metal_flags']=seed_flags(sdk)   # build_ipa.py reuses the libraries only when these match
  # the seed's own files keep the hashes checked when it was prepared; the metallibs were hashed above
  (seed/'seed-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
  print(f'compiled {len(results)} iOS metallibs ({len(groups)} distinct sources, {reused[0]} source compilations from {cache.name})')

if __name__=='__main__': main()
