#!/usr/bin/env python3
# The translation chain's tools against their lock file dm/gfx/tools/dmsh_chain.lock: the translator (its dlls and the source digest it embeds), SPIRV-Cross and the Metal compilers (macOS / iOS).
# The lock is part of the chain version (dmsh_chain.py); these checks are not: they decide no shader's output, so a fix to a check or its message neither changes V nor the shaders' cache keys
# (dmsh_build.py CODE_FP). dmsh_build.py runs check_tools before it builds anything; own_compilers.py replaces it for a player's Mac.
# Usage: tool_lock.py --check-tools   verify that the actual tools match the lock file; on mismatch print [RED] lines and exit 1
#        tool_lock.py --update-lock   rewrite the lock file from the actual tools (run after changing Ryujinx / spirv-cross / Xcode, then dmsh_chain.py --header; both are committed)
import os, sys, hashlib, subprocess, shutil
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dmsh_chain import LOCK, XLATE_DIR, SPIRV_CROSS, xlate_source_sha

def sha16(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16]

def tools_now():
    """Actual tool fingerprints: translator dll (including the Ryujinx shader library), spirv-cross, metal compiler (macOS / iOS)"""
    t = {}
    for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll'): t[dll] = sha16(os.path.join(XLATE_DIR, dll))
    t['xlate_source'] = xlate_embedded()
    sc = shutil.which(SPIRV_CROSS)
    t['spirv-cross'] = sha16(sc) if sc else 'missing'
    for sdk in ('macosx', 'iphoneos'):
        t['metal_' + sdk] = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], capture_output=True, text=True).stdout.splitlines()[0:1] or ['missing']
        t['metal_' + sdk] = t['metal_' + sdk][0]
    return t

NO_RUN = 'does not run'
def xlate_embedded():
    """The source digest the translator embeds (dmsh_xlate --source-sha), or 'does not run (<its first error line>)': without a .NET 10 runtime for the architecture it starts as (a Python
    under Rosetta starts the universal apphost as x86_64) it exits 131 with nothing on stdout, which read as an empty digest and a stale translator (docs/reference/packing.md)"""
    try: r = subprocess.run([os.path.join(XLATE_DIR, 'dmsh_xlate'), '--source-sha'], capture_output=True, text=True, errors='replace', timeout=60)
    except (OSError, subprocess.SubprocessError) as e: return f'{NO_RUN} ({e})'
    if r.returncode == 0 and r.stdout.strip(): return r.stdout.strip()
    return '%s (%s)' % (NO_RUN, (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or [f'exit code {r.returncode}'])[0])

def no_run_message(embedded): return f'the translator {embedded}: it needs the .NET 10 runtime for this Mac\'s architecture (brew install --cask dotnet-sdk); unset DOTNET_ROOT if it names another one'

def check_tools():
    want = dict(l.rstrip('\n').split('\t', 1) for l in open(LOCK) if '\t' in l and not l.startswith('#'))
    now = tools_now(); ran = not now['xlate_source'].startswith(NO_RUN)
    bad = [f'{k}: lock {want.get(k)} / actual {v}' for k, v in now.items() if want.get(k) != v and (ran or k != 'xlate_source')]
    if not ran: bad.append(no_run_message(now['xlate_source']))
    elif now['xlate_source'] != xlate_source_sha(): bad.append(f'the translator was not built from the in-repo source: embedded source {now["xlate_source"]} / in repo {xlate_source_sha()} (rebuild dmsh_xlate)')
    return bad

if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['--update-lock']:
        t = tools_now()
        if t['xlate_source'].startswith(NO_RUN): sys.exit('[RED] ' + no_run_message(t['xlate_source']))
        if t['xlate_source'] != xlate_source_sha(): sys.exit(f'[RED] the translator was not built from the in-repo source (embedded {t["xlate_source"]} / in repo {xlate_source_sha()}): rebuild dmsh_xlate first')
        open(LOCK, 'w').write('# Translation chain tool fingerprints (written by tool_lock.py --update-lock; regenerate after changing tools and commit it)\n' + ''.join(f'{k}\t{v}\n' for k, v in t.items()))
        print('wrote', LOCK)
    elif a == ['--check-tools']:
        bad = check_tools()
        for b in bad: print('[RED] translation chain tools do not match the lock file: ' + b)
        sys.exit(1 if bad else 0)
    else: sys.exit('usage: tool_lock.py --check-tools | --update-lock')
