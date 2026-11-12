#!/usr/bin/env python3
"""Compile one graphics layer for every game configuration that builds from it, Mac and iOS, and report the warnings each
adds over a reference tree (default: the same files at HEAD). For the graphics layer unification (one shared layer for
every game): a change to the shared layer must keep every configuration compiling, with no new warnings.

  python3 tools/gfx_matrix.py                       # games/totk's graphics layer, working tree vs HEAD
  python3 tools/gfx_matrix.py --base <commit>       # vs another commit
  python3 tools/gfx_matrix.py --only totk,dkb       # some configurations

Syntax and semantic checks only (-fsyntax-only), so no objects are written and nothing is linked; a link or run check
stays with the game's own build and tests (tests/test_nvn_semantics.py for the NVN semantics). Exit 1 when any
configuration fails to compile or adds a warning."""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME_DIR = ROOT / 'games/totk'
GFX = 'third_party/d2r_gfx'
# games/totk/Makefile: build/gfx (TOTK 1.0.0 / 1.2.1), build/gfx-v143 (only zeldaVR differs), build/gfx-botw,
# build/gfx-botw190, build/gfx-dkb, build/gfx-pkp
CONFIGS = {
    'totk': [],
    'totk143': ['-DTOTK_GAME_V143=1'],
    'botw160': ['-DGAME_BOTW=1', '-DGAME_BOTW160=1'],
    'botw190': ['-DGAME_BOTW=1', '-DGAME_BOTW190=1'],
    'dkb': ['-DGAME_DKB=1', '-DGAME_NS2=1'],
    'pokopia': ['-DGAME_POKOPIA=1', '-DGAME_NS2=1'],
    'oot': ['-DGAME_OOT=1', '-DGAME_NS2=1'],   # games/oot/Makefile GFX_FLAGS (Ocarina of Time moves onto this layer)
}
FLAGS = ['-std=gnu11', '-Wall', '-Wextra', '-Wno-error', '-Wno-unused-function', '-Wno-unused-parameter', '-fobjc-arc']
TARGETS = {
    'mac': ['-target', 'arm64-apple-macos26'],
    'ios': ['-target', 'arm64-apple-ios26', '-mcpu=apple-a15'],   # games/totk/ios/project.yml OTHER_CFLAGS
}


def sdk(name):
    r = subprocess.run(['xcrun', '--sdk', name, '--show-sdk-path'], capture_output=True, text=True)
    if r.returncode: sys.exit(f'no {name} SDK: point DEVELOPER_DIR at an Xcode (e.g. DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer), or pass --targets mac')
    return r.stdout.strip()


def sources(tree):
    out = []
    for sub in ('gfx/nvn', 'gfx/tex'):
        d = tree / GFX / sub
        out += sorted(p for p in d.iterdir() if p.suffix in ('.c', '.m'))
    return out


def ios_excluded():
    """Files the iOS project leaves out (games/totk/ios/project.yml excludes), by name."""
    out, inside = set(), False
    for line in (GAME_DIR / 'ios/project.yml').read_text().splitlines():
        if line.strip() == 'excludes:': inside = True; continue
        m = re.match(r'\s*- ([A-Za-z0-9_./*]+\.(?:c|m))\s*$', line)
        if inside and m: out.add(Path(m.group(1)).name); continue
        inside = False
    return out


def compile_one(tree, src, defines, target, sysroot):
    cmd = ['xcrun', 'clang', '-fsyntax-only', *FLAGS, *TARGETS[target], '-isysroot', sysroot, *defines,
           f'-I{tree / GFX / "gfx/nvn"}', f'-I{tree / GFX / "gfx/nvn/include"}', f'-I{tree / "runtime"}', f'-I{GAME_DIR / "build"}', str(src)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=tree)
    rel = src.relative_to(tree / GFX)
    warns = []
    for line in r.stderr.splitlines():
        m = re.match(r'(.+?):(\d+):\d+: (warning|error): (.*)', line)
        if m: warns.append((m.group(3), Path(m.group(1)).name, m.group(4)))   # line numbers move between trees: compare text only
    return str(rel), r.returncode, warns


def run(tree, configs, targets, jobs):
    roots = {t: sdk('macosx' if t == 'mac' else 'iphoneos') for t in targets}
    skip_ios = ios_excluded()
    work = []
    for t in targets:
        for c in configs:
            for s in sources(tree):
                if t == 'ios' and s.name in skip_ios: continue
                work.append((c, t, s))
    res = {}
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(compile_one, tree, s, CONFIGS[c], t, roots[t]): (c, t) for c, t, s in work}
        for f in cf.as_completed(futs):
            c, t = futs[f]; rel, rc, warns = f.result()
            res.setdefault((c, t), {})[rel] = (rc, warns)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--base', default='HEAD', help='reference commit (default HEAD)')
    ap.add_argument('--only', help='comma-separated configurations')
    ap.add_argument('--targets', default='mac,ios')
    ap.add_argument('-j', type=int, default=os.cpu_count())
    a = ap.parse_args()
    configs = a.only.split(',') if a.only else list(CONFIGS)
    targets = a.targets.split(',')
    with tempfile.TemporaryDirectory() as d:
        base = Path(d)
        arch = subprocess.run(['git', 'archive', a.base, 'games/totk', 'zeldaVR'], cwd=ROOT, capture_output=True, check=True).stdout
        subprocess.run(['tar', '-x', '-C', str(base)], input=arch, check=True)
        before = run(base / 'games/totk', configs, targets, a.j)
        after = run(GAME_DIR, configs, targets, a.j)
    bad = 0
    for key in sorted(after):
        c, t = key; fails = []; new = []
        for rel, (rc, warns) in sorted(after[key].items()):
            if rc: fails.append(rel)
            old = list(before.get(key, {}).get(rel, (0, []))[1])
            for w in warns:
                if w in old: old.remove(w)
                else: new.append((rel, w))
        base_fails = [rel for rel, (rc, _) in before.get(key, {}).items() if rc]
        if base_fails: print(f'note {c} {t}: the reference tree does not compile {", ".join(sorted(base_fails))}; its warnings are incomplete')
        status = 'FAIL' if fails or new else 'ok'
        print(f'{status:4s} {c:8s} {t:3s}  {len(after[key])} files' + (f'  compile errors: {", ".join(fails)}' if fails else '') + (f'  new warnings: {len(new)}' if new else ''))
        for rel, w in new[:20]: print(f'       {rel}: {w[0]}: {w[2]}')
        bad += status != 'ok'
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
