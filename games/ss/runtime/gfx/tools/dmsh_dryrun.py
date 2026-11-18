#!/usr/bin/env python3
# Route B whole-library dry run: every variant in the Switch shader packages (deduplicated by control segment hash) → SPIR-V via Ryujinx ShaderTools → MSL via spirv-cross → metal compile.
# Records pass/fail and the first error line at each stage, and summarizes the success rate and the distribution of failure reasons. The dry run uses ShaderTools' default queries (texture types etc. are not answered truthfully) and only measures "can it be converted".
# Usage: dmsh_dryrun.py <Switch package dir> <ShaderTools executable> <output dir>   Environment J=concurrency
import os, re, sys, json, hashlib, subprocess, struct, tempfile, collections, concurrent.futures as cf
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from dmsh_chain import SPIRV_CROSS   # the in-repo spirv-cross build
dm_dir, shtools, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
XC = ['--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native']

def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]

def first_err(s):
    for ln in s.splitlines():
        if 'error' in ln.lower() or 'exception' in ln.lower(): return re.sub(r'/[^ :]*/', '', ln.strip())[:160]
    return (s.strip().splitlines() or ['(no output)'])[-1][:160]

def one(job):
    key, name, comp, code = job
    with tempfile.TemporaryDirectory() as w:
        bi, spv, msl, air = (os.path.join(w, n) for n in ('c.bin', 'c.spv', 'c.metal', 'c.air'))
        open(bi, 'wb').write(code)
        r = subprocess.run([shtools, '--target-language', 'Spirv', '--target-api', 'Vulkan'] + (['--compute'] if comp else []) + [bi, spv], capture_output=True, text=True)
        if r.returncode or not os.path.exists(spv) or os.path.getsize(spv) == 0: return key, name, 'spirv', first_err(r.stdout + r.stderr)
        r = subprocess.run([SPIRV_CROSS, *XC, spv, '--output', msl], capture_output=True, text=True)
        if r.returncode: return key, name, 'msl', first_err(r.stdout + r.stderr)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', msl, '-o', air], capture_output=True, text=True)
        if r.returncode: return key, name, 'metal', first_err(r.stdout + r.stderr)
    return key, name, 'ok', ''

jobs, seen = [], set()
for name in sorted(os.listdir(dm_dir)):
    if not name.endswith('.bin'): continue
    for b in records(open(os.path.join(dm_dir, name), 'rb').read()):
        ci = b.find(bytes.fromhex('34127698')); csz = struct.unpack_from('<I', b, 0x28)[0]
        key = hashlib.sha256(b[ci:ci + csz]).hexdigest()[:16]
        if key in seen: continue
        seen.add(key); comp = name.startswith('cs_'); code = b[0x38:ci]
        jobs.append((key, name, comp, code if comp else code[0x30:]))
with cf.ThreadPoolExecutor(int(os.environ.get('J', '8'))) as ex: res = list(ex.map(one, jobs))
json.dump(res, open(os.path.join(out, 'dryrun.json'), 'w'), ensure_ascii=False, indent=0)
st = collections.Counter(r[2] for r in res)
print(f'distinct shaders {len(res)}: ok {st["ok"]} ({st["ok"] / len(res):.1%}), SPIR-V failed {st["spirv"]}, MSL failed {st["msl"]}, metal compile failed {st["metal"]}')
for lvl in ('spirv', 'msl', 'metal'):
    c = collections.Counter(r[3] for r in res if r[2] == lvl)
    for e, n in c.most_common(12): print(f'  [{lvl}] {n:5d}  {e}')
    byf = collections.Counter(r[1] for r in res if r[2] == lvl)
    if byf: print(f'  [{lvl}] by file:', dict(byf.most_common(12)))
