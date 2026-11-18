#!/usr/bin/env python3
"""Find what in an Ocarina of Time mesh shader crashes this Mac's Metal compiler (#2107, #2108, #2112).

On M1 / M2 GPUs with macOS 27.2-27.3 a few mesh pipelines make Apple's compiler service crash
(XPC_ERROR_CONNECTION_INTERRUPTED; the crash report's stack is in AGCModule<AGX::Impl::MeshProgramKey>::compileLLVMObject).
macOS 26's compiler builds them, so the crash can only be studied on an affected Mac. This script:

  1. takes the failing mesh pipelines' shaders from your own work folder (the translated MSL the build made),
     compiles them as the build does and builds each pipeline on this Mac's compiler service (mesh_probe.m), twice;
  2. for the first one that crashes every time, tries the build's compiler options one change at a time;
  3. removes statements from that mesh shader as long as it still crashes the compiler (delta debugging), so what is left
     is the smallest piece of code that still crashes it.

It runs no game code and writes only to --out. report.txt holds the results and the remaining statements with every number
replaced by K, so it can be pasted into the issue; the full shaders stay on your Mac.

  python3 games/oot/tools/mesh_compiler_reduce/reduce.py --app "/path/to/Ocarina of Time.app"
  python3 games/oot/tools/mesh_compiler_reduce/reduce.py --work <work folder> [--minutes 40] [--mesh <key prefix>]
"""
import argparse, hashlib, json, os, plistlib, re, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# The mesh shaders whose pipelines failed on affected Macs (#2107 M1 Max, #2108 M2 Pro, #2112 M1).
FAILING = ['ac2f3feb1142742661a038e6907a773304b34e0eaf104d3e1a238d8533e17dbd',
           'b903546b6b988e5fab8c6fc1002c4089b7094ba7dcb81cbf00e68dfbcdc23406',
           '9df3c6c01fec0193b5035ba7bfa9c33d0f8e8fde7c113c4de1cb742608b8b940',
           '57ed4680e3042998a0c9f18e586f8bc932748bd8fe3384df562db376308e5b53']
SUFFIX = '-b7530f698e76_b'
FLAGS = ['-fmetal-math-mode=safe', '-fpreserve-invariance', '-fwrapv']   # games/oot/scripts/build_shader_libs.py METAL_FLAGS
FLAG_TRIALS = [('without -fpreserve-invariance', ['-fmetal-math-mode=safe', '-fwrapv']),
               ('without -fwrapv', ['-fmetal-math-mode=safe', '-fpreserve-invariance']),
               ('math mode fast', ['-fmetal-math-mode=fast', '-fpreserve-invariance', '-fwrapv']),
               ('-Os', [*FLAGS, '-Os'])]
CRASH = re.compile(r'interrupt|XPC_ERROR', re.I)


def say(log, text):
    print(text, flush=True)
    log.write(text + '\n'); log.flush()


def work_folder(a):
    if a.work: return Path(a.work).expanduser()
    info = plistlib.loads((Path(a.app).expanduser() / 'Contents/Info.plist').read_bytes())
    if 'MNGWorkFolder' not in info: sys.exit('the app records no work folder (MNGWorkFolder); pass --work')
    return Path(info['MNGWorkFolder'])


def find(work, pattern):
    found = sorted(work.glob(pattern))
    if not found: sys.exit(f'{work}: no {pattern} (is this the work folder the app was built with?)')
    return found[0]


def pipelines(manifest):
    """mesh key -> (object key or None, fragment key), from the M rows of pso_manifest.tsv."""
    rows = {}
    for line in manifest.read_text(errors='replace').splitlines():
        cols = line.split('\t')
        if cols[0] != 'M' or len(cols) < 3: continue
        try: keys = [k.decode() for k in bytes.fromhex(cols[2]).split(b'\0') if k.endswith(b'_b')]
        except ValueError: continue
        mesh = next((k for k in keys if k.split('-')[0] in FAILING), None)
        if mesh and mesh not in rows:
            rest = [k for k in keys if k != mesh]
            rows[mesh] = (rest[0] if len(rest) == 2 else None, rest[-1])
    return rows


class Bench:
    def __init__(self, out, log):
        self.out, self.log, self.cache = out, log, {}
        self.probe = out / 'mesh_probe'
        if not self.probe.exists() or self.probe.stat().st_mtime < (HERE / 'mesh_probe.m').stat().st_mtime:
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-O1', str(HERE / 'mesh_probe.m'), '-framework', 'Metal',
                            '-framework', 'Foundation', '-o', str(self.probe)], check=True)
        (out / 'libs').mkdir(exist_ok=True)
        self.compiles = self.probes = 0

    def library(self, source, flags=FLAGS):
        """The .metallib of this source with these flags, or None when the front end rejects it."""
        digest = hashlib.sha256((' '.join(flags) + '\n').encode() + source.encode()).hexdigest()
        lib = self.out / 'libs' / f'{digest}.metallib'
        if lib.exists(): return lib
        if (self.out / 'libs' / f'{digest}.bad').exists(): return None
        src = self.out / 'libs' / f'{digest}.metal'
        src.write_text(source)
        self.compiles += 1
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', *flags, '-fbracket-depth=1024', '-o', str(lib), str(src)],
                           capture_output=True, text=True)
        src.unlink()
        if r.returncode or not lib.exists():
            (self.out / 'libs' / f'{digest}.bad').write_text(r.stderr[-2000:])
            return None
        return lib

    def build(self, obj, mesh, frag, entry='main0'):
        """'crash' (the compiler service was interrupted), 'ok', or 'error: <text>' for any other failure."""
        self.probes += 1
        try:
            r = subprocess.run([str(self.probe), str(obj) if obj else '-', str(mesh), str(frag), entry],
                               capture_output=True, text=True, timeout=300)
            res = json.loads(r.stdout.strip().splitlines()[-1])
        except (subprocess.TimeoutExpired, ValueError, IndexError) as e:
            return f'error: probe {e!r}'
        if res['ok']: return 'ok'
        return 'crash' if CRASH.search(res['error']) else 'error: ' + res['error'][:200]


def removable(lines, entry_line):
    """Indices of statements inside function bodies that may be removed: indented lines ending in ';', not declarations of
    struct members (before the first function) and not the mesh entry's own signature."""
    first_fn = next((i for i, l in enumerate(lines) if re.match(r'^(static |inline |\[\[|template|kernel|fragment|vertex)', l)
                     and '(' in l), 0)
    return [i for i, l in enumerate(lines) if i > first_fn and i != entry_line and l.startswith((' ', '\t'))
            and l.rstrip().endswith(';')]


def ddmin(items, test, deadline, log):
    n = 2
    while len(items) >= 2 and time.time() < deadline:
        size = -(-len(items) // n)
        chunks = [items[i:i + size] for i in range(0, len(items), size)]
        for c in chunks:
            if time.time() >= deadline: break
            drop = set(c)
            rest = [x for x in items if x not in drop]
            if test(rest):
                items, n = rest, max(n - 1, 2)
                say(log, f'   {len(items)} statements still crash it')
                break
        else:
            if n >= len(items): break
            n = min(len(items), n * 2)
    return items


def masked(line):
    return re.sub(r'\b(0x[0-9a-fA-F]+u?|\d+\.\d*(e[+-]?\d+)?f?|\d+u?)\b', 'K', line.strip())


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument('--app'); g.add_argument('--work')
    p.add_argument('--out', default=str(Path.home() / 'Library/Caches/MacNativeGames/oot-mesh-reduce'))
    p.add_argument('--minutes', type=float, default=40, help='time for the statement reduction (default 40)')
    p.add_argument('--mesh', help='only this mesh key (prefix)')
    a = p.parse_args()
    work = work_folder(a)
    out = Path(a.out).expanduser(); out.mkdir(parents=True, exist_ok=True)
    log = (out / 'report.txt').open('w')
    seed = find(work, '**/shader-seed/oot/seed-manifest.json').parent
    manifest = Path(a.app).expanduser() / 'Contents/Resources/totk/build/pso_manifest.tsv' if a.app else None
    if not manifest or not manifest.exists(): manifest = find(work, '**/warm-lists/pso_manifest.tsv')
    mac = subprocess.run(['sysctl', '-n', 'machdep.cpu.brand_string'], capture_output=True, text=True).stdout.strip()
    os_v = subprocess.run(['sw_vers', '-productVersion'], capture_output=True, text=True).stdout.strip()
    metal = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '--version'], capture_output=True, text=True).stdout.splitlines()[:1]
    say(log, f'# OOT mesh compiler reduction (#2107)\nMac: {mac}, macOS {os_v}, {metal}\nseed: chain {json.loads((seed / "seed-manifest.json").read_text()).get("chain", "?")}')
    bench = Bench(out, log)
    rows = pipelines(manifest)
    targets = [k for k in rows if not a.mesh or k.startswith(a.mesh)]
    if not targets: sys.exit('none of the failing mesh pipelines is in this pso_manifest.tsv')

    say(log, '\n## 1. Each pipeline as built (twice)')
    libs, crashing = {}, None
    for mesh in sorted(targets, key=lambda k: FAILING.index(k.split('-')[0])):
        obj, frag = rows[mesh]
        built = [bench.library((seed / f'{k}.metal').read_text()) if k else None for k in (obj, mesh, frag)]
        if (obj and not built[0]) or not built[1] or not built[2]:
            say(log, f'{mesh[:16]}: a library did not compile (see {out}/libs/*.bad)'); continue
        libs[mesh] = built
        results = [bench.build(*built) for _ in range(2)]
        say(log, f'{mesh[:16]}: {results[0]}, {results[1]}')
        if crashing is None and results == ['crash', 'crash']: crashing = mesh
    if not crashing:
        say(log, '\nNo pipeline crashed the compiler twice in a row here. Please paste this report into the issue.'); return

    obj_lib, _, frag_lib = libs[crashing]
    source = (seed / f'{crashing}.metal').read_text()
    say(log, f'\n## 2. {crashing[:16]} with other compiler options')
    for name, flags in FLAG_TRIALS:
        lib = bench.library(source, flags)
        say(log, f'{name}: {bench.build(obj_lib, lib, frag_lib) if lib else "does not compile"}')
    say(log, f'without the object stage: {bench.build(None, libs[crashing][1], frag_lib)}')

    say(log, f'\n## 3. Smallest part of {crashing[:16]} that still crashes the compiler ({a.minutes:g} minutes at most)')
    lines = source.split('\n')
    entry_line = next(i for i, l in enumerate(lines) if '[[mesh]]' in l and ' main0(' in l)
    cand = removable(lines, entry_line)
    say(log, f'{len(cand)} removable statements of {len(lines)} lines')

    def test(kept):
        drop = set(cand) - set(kept)
        text = '\n'.join(l for i, l in enumerate(lines) if i not in drop)
        lib = bench.library(text)
        if not lib or bench.build(obj_lib, lib, frag_lib) != 'crash': return False
        (out / 'reduced.metal').write_text(text)
        return True

    kept = ddmin(cand, test, time.time() + a.minutes * 60, log)
    say(log, f'\nkept {len(kept)} of {len(cand)} statements ({bench.compiles} compiles, {bench.probes} pipeline builds); '
             f'full file: {out}/reduced.metal (keep it on your Mac)')
    say(log, '\n### Remaining statements, numbers replaced by K')
    for i in kept[:200]: say(log, '    ' + masked(lines[i]))
    say(log, f'\nPlease paste {out}/report.txt into the issue.')


if __name__ == '__main__':
    main()
