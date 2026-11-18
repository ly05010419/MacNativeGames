#!/usr/bin/env python3
"""Compile the iOS host's sources for the iPhone SDK without game files, signing or an Xcode project (CI's iOS build check).

The source list, header paths and -D flags come from the expanded compile commands of dm/host/Makefile, as gen_build.py takes them for the Xcode project:
the _mac sources stay out, main.c is compiled with -Dmain=dm_main, and dm/ios/Sources (Objective-C; Launcher.swift is not compiled) is added.
Each file is compiled to nothing (-c -o /dev/null), so the assembler sees entry.S and the embedded-segment files (Gen/embed.S) are not needed.
Usage: DM_GAME=<base game title ID> compile_check.py   (or GAME=)   Exit status 1 when any file fails."""
import concurrent.futures, os, shlex, subprocess, sys
repo = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../..'))
host, ios = f'{repo}/dm/host', f'{repo}/dm/ios'
game = os.environ.get('GAME') or os.environ.get('DM_GAME')
if not game: sys.exit('set GAME (or DM_GAME) to a base game title ID')
env = dict(os.environ, GAME=game)
subprocess.run(['make', '-s', '-C', host, 'game_profile', 'host_msl', 'MSL_SDK=iphoneos'], check=True, env=env)
cmd = subprocess.run(['make', '-s', '-n', '-B', '-C', host, 'dmhost', 'MSL_SDK=iphoneos'], capture_output=True, text=True, check=True, env=env).stdout
lines = [shlex.split(l) for l in cmd.splitlines() if l.lstrip().startswith('clang')]
lines.sort(key=lambda t: 'dmhost' not in t)   # the host's own command first, as gen_build.py does, so its header paths come before libvpx's
srcs, incs, defs = [], [], []
for toks in lines:
    for t in toks:
        if t.startswith('-I'):
            d = os.path.normpath(os.path.join(host, t[2:]))
            if d not in incs: incs.append(d)
        elif t.startswith('-D'): defs.append(t)
        elif t.endswith(('.c', '.m', '.S')): srcs.append(os.path.normpath(os.path.join(host, t)))
assert srcs, 'no source files in the compile commands'
ios_src = os.path.normpath(f'{ios}/Sources')
srcs = [s for s in dict.fromkeys(srcs) if not os.path.splitext(s)[0].endswith('_mac') and os.path.dirname(s) != ios_src]
srcs += sorted(os.path.join(ios_src, f) for f in os.listdir(ios_src) if f.endswith(('.c', '.m')))
dirs = list(dict.fromkeys(os.path.dirname(s) for s in srcs))
flags = ['-arch', 'arm64', '-miphoneos-version-min=26.0', '-mcpu=apple-a18', '-fobjc-arc', '-O1', '-w'] + defs + [f'-I{d}' for d in incs + dirs]
def one(s):
    c = ['xcrun', '-sdk', 'iphoneos', 'clang'] + flags + (['-Dmain=dm_main'] if s == os.path.join(host, 'main.c') else []) + ['-c', s, '-o', '/dev/null']
    if s.endswith('.c'): c[4:4] = ['-std=gnu11']
    p = subprocess.run(c, capture_output=True, text=True)
    return s, p.returncode, p.stderr
bad = 0
with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
    for s, rc, err in ex.map(one, srcs):
        if rc: bad += 1; print(f'FAIL {os.path.relpath(s, repo)}\n{err}', file=sys.stderr)
print(f'iOS compile check: {len(srcs)} files, {bad} failed')
sys.exit(1 if bad else 0)
