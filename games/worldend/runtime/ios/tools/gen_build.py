# Take the host's source files / header paths / frameworks from the actually expanded compile commands of dm/host/Makefile (make -n -B dmhost, including the per-file compiles of the libraries it depends on) and generate the iOS project's build inputs,
# without hand-copying lists: when the Makefile changes, the next iOS build follows automatically.
#   Gen/src/<flat name>   symlinks to source files (excluding main.c and front_mac.m; main.c is linked separately to Gen/main/main.c and compiled with -Dmain=dm_main)
#   stdout            xcconfig lines: HEADER_SEARCH_PATHS (-I directories + the directory of every source file, so relative includes resolve as usual),
#                     OTHER_CFLAGS (-D), frameworks (Cocoa is replaced by what the UIKit side already has, skipped)
# Usage: gen_build.py <dm/host dir> <Gen dir>
import os, shlex, subprocess, sys
host, gen = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
subprocess.run(['make', '-s', '-C', host, 'game_profile'], check=True)   # generate the game profile header for real first (make -n below only prints; GAME comes from the environment)
cmd = subprocess.run(['make', '-s', '-n', '-B', '-C', host, 'dmhost'], capture_output=True, text=True, check=True).stdout
# The host's own link command (-o dmhost) first, then the per-file compile commands of the libraries it depends on (libvpx): header paths keep this order, so the host's headers are found first
lines = [shlex.split(l) for l in cmd.splitlines() if l.lstrip().startswith('clang')]
lines.sort(key=lambda t: 'dmhost' not in t)
srcs, incs, defs, fws = [], [], [], []
for toks in lines:
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == '-framework': fws.append(toks[i + 1]); i += 2; continue
        if t.startswith('-I'):
            d = os.path.normpath(os.path.join(host, t[2:]))
            if d not in incs: incs.append(d)
        elif t.startswith('-D'): defs.append(t)
        elif t.endswith(('.c', '.m', '.S')): srcs.append(os.path.normpath(os.path.join(host, t)))
        i += 1
assert srcs, 'no source files in the compile command'
dup = sorted({s for s in srcs if srcs.count(s) > 1})
if dup: sys.exit('duplicate source files in the compile command (explicit Makefile entries overlapping a wildcard?): ' + ' '.join(dup))
os.makedirs(f'{gen}/src'); os.makedirs(f'{gen}/main')
dirs = []
for s in srcs:
    d = os.path.dirname(s)
    if d not in dirs: dirs.append(d)
    b = os.path.basename(s)
    if os.path.splitext(b)[0].endswith('_mac'): continue   # Mac-only sources are kept out of iOS by the _mac naming convention (for front_mac.m, front_ios.m implements front.h; touch_mac.c is the Mac touch point script)
    if os.path.dirname(s) == os.path.normpath(os.path.join(host, '../ios/Sources')): continue   # iOS sources borrowed by the host (e.g. the touch module) are already compiled by the iOS project itself; collecting another copy would link them twice
    if s == os.path.join(host, 'main.c'): os.symlink(s, f'{gen}/main/main.c'); continue
    flat = os.path.relpath(s, os.path.dirname(host)).replace('/', '__')   # Same-named files in different directories do not collide
    os.symlink(s, f'{gen}/src/{flat}')
print('HEADER_SEARCH_PATHS = $(inherited) ' + ' '.join(f'"{d}"' for d in incs + dirs))
print('OTHER_CFLAGS = $(inherited) ' + ' '.join(defs))
print('DM_FRAMEWORKS = ' + ' '.join(f'-framework {f}' for f in fws if f != 'Cocoa'))
