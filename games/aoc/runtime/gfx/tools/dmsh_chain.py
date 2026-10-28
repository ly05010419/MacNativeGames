#!/usr/bin/env python3
# Translation chain version of the B shader library: one version for every game, which the host is compiled with (dm/gfx/nvn/dmsh_chain.h). The build result must be determined by source alone.
# Chain version V = first 12 digits of the sha256 of the contents of the following in-repo files + the translation options: this script, dmsh_build.py, dmsh_fetch.py (readable-tile version), dmsh_vin.py (shadow vertex inputs),
# dmsh_geometry.py, dmsh_facts.py, the dmsh_xlate sources (Program.cs, csproj), the iOS compile convention ios_metal.py, and the tool lock dmsh_chain.lock (measured fingerprints of the Ryujinx shader dll / spirv-cross / metal compiler).
# A game's own inputs are not part of V: its profile's shader tables and nvn_* / shader_* keys are in each shader's cache key (dmsh_build.py shader_key), so a change rebuilds the shaders it affects,
# and their digest (inputs below) stamps the library: <cache dir>/b-<V>/inputs.tsv says which game and which inputs it was built from. c1_full.tsv / cb_extent.tsv name V and that digest in their
# header (table_head), and the host takes them only when both match its library; build_ipa.sh packs a library only when its digest is the profile's.
# The build (dmsh_build.py) writes into <cache dir>/b-<V>/ and the reflection json records chain = V; at compile time the host takes V from dm/gfx/nvn/dmsh_chain.h, loads only from b-<V>/ and verifies each json.
# Usage: dmsh_chain.py                 print V (default options)
#       dmsh_chain.py --header        rewrite dm/gfx/nvn/dmsh_chain.h (run after changing any file listed above, and commit it with the change)
#       dmsh_chain.py --inputs        print the digest of the current game profile's inputs (DM_GAME), as b-<V>/inputs.tsv records it
#       dmsh_chain.py --check-tools   verify that the actual tools match the lock file; on mismatch print [RED] lines and exit 1
#       dmsh_chain.py --update-lock   rewrite the lock file from the actual tools (run after changing Ryujinx / spirv-cross / Xcode; committed to the repo)
# Environment: XLATE_DEBUG / XLATE_REDUCED (same meaning as in dmsh_build.py, default 1 / 1; non-default options yield a different V, which the host will not load)
import os, sys, hashlib, subprocess
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # Workspace root (game files, build outputs and run artifacts are all under it)
HERE = os.path.dirname(os.path.abspath(__file__)); GFX = os.path.dirname(HERE); HOME = os.path.expanduser('~')
FILES = ['tools/dmsh_chain.py', 'tools/dmsh_build.py', 'tools/dmsh_fetch.py', 'tools/dmsh_vin.py', 'tools/dmsh_geometry.py', 'tools/dmsh_facts.py', '../ios/tools/ios_metal.py', 'tools/dmsh_chain.lock']
INPUT_TABLES = ['shader_facts.tsv', 'shader_map.tsv', 'shader_map.tsv.conflicts.tsv', 'shader_pairs.tsv', 'bf_want.tsv']   # in the game profile folder
LOCK = os.path.join(HERE, 'dmsh_chain.lock'); HEADER = os.path.join(GFX, 'nvn/dmsh_chain.h')
STAMP = 'inputs.tsv'   # in the library directory b-<V>/
# the translator defaults to the in-repo build that matches the lock file (a rebuilt dll has different fingerprints)
XLATE_DIR = os.path.dirname(os.environ.get('XLATE', os.path.join(HERE, 'dmsh_xlate_bin/dmsh_xlate')))

def options():
    return 'debug=%s reduced=%s' % (os.environ.get('XLATE_DEBUG', '1') != '0', os.environ.get('XLATE_REDUCED', '1') == '1')

def version():
    h = hashlib.sha256()
    for f in FILES:
        h.update(f.encode() + b'\0' + open(os.path.join(GFX, f), 'rb').read() + b'\0')
    h.update(options().encode())
    return h.hexdigest()[:12]

def load_game():   # the current game profile (DM_GAME), only for what needs one: the version itself does not
    sys.path.insert(0, os.path.join(GFX, '../games')); import game
    return game.load()

def profile_rows(g):   # the profile keys that change how a shader is built (nvn_*, shader_*)
    return ''.join(f'{k}\t{chr(9).join(v)}\n' for k, v in g.rows if k.startswith(('nvn_', 'shader_'))).encode()

def inputs(g):
    """Digest of a game profile's inputs to its shader library: the shader tables (a missing one counts as such) and the nvn_* / shader_* keys"""
    h = hashlib.sha256()
    for f in INPUT_TABLES:
        p = g.table(f)
        h.update(f'game/{f}'.encode() + b'\0' + (p.read_bytes() if p.exists() else b'missing') + b'\0')
    h.update(profile_rows(g))
    return h.hexdigest()[:12]

def read_stamp(lib):   # {'game': title ID, 'inputs': digest} of a library directory; {} when it has none
    try: return dict(l.rstrip('\n').split('\t', 1) for l in open(os.path.join(lib, STAMP)) if '\t' in l and not l.startswith('#'))
    except OSError: return {}

def write_stamp(lib, game_id, digest):
    tmp = os.path.join(lib, STAMP + '.tmp')
    with open(tmp, 'w') as f: f.write(f'# the game and the profile inputs this library was built from (dm/gfx/tools/dmsh_chain.py inputs)\ngame\t{game_id}\ninputs\t{digest}\n')
    os.replace(tmp, os.path.join(lib, STAMP))

def table_head(lib):   # the header c1_full.tsv / cb_extent.tsv start with: the library's chain version and its inputs ("none" when it has no stamp, which nothing takes)
    return f'# chain {os.path.basename(os.path.normpath(lib))} inputs {read_stamp(lib).get("inputs", "none")}'

def sha16(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()[:16]

def tools_now():
    """Actual tool fingerprints: translator dll (including the Ryujinx shader library), spirv-cross, metal compiler (macOS / iOS)"""
    t = {}
    for dll in ('dmsh_xlate.dll', 'Ryujinx.Graphics.Shader.dll', 'Ryujinx.Common.dll'): t[dll] = sha16(os.path.join(XLATE_DIR, dll))
    t['xlate_source'] = subprocess.run([os.path.join(XLATE_DIR, 'dmsh_xlate'), '--source-sha'], capture_output=True, text=True).stdout.strip()
    sc = subprocess.run(['which', 'spirv-cross'], capture_output=True, text=True).stdout.strip()
    t['spirv-cross'] = sha16(sc) if sc else 'missing'
    for sdk in ('macosx', 'iphoneos'):
        t['metal_' + sdk] = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '--version'], capture_output=True, text=True).stdout.splitlines()[0:1] or ['missing']
        t['metal_' + sdk] = t['metal_' + sdk][0]
    return t

def xlate_source_sha():
    """Expected embedded source hash of the binary-only pinned translator."""
    return dict(l.rstrip('\n').split('\t', 1) for l in open(LOCK)
                if '\t' in l and not l.startswith('#'))['xlate_source']

def check_tools():
    want = dict(l.rstrip('\n').split('\t', 1) for l in open(LOCK) if '\t' in l and not l.startswith('#'))
    now = tools_now(); bad = [f'{k}: lock {want.get(k)} / actual {v}' for k, v in now.items() if want.get(k) != v]
    if now['xlate_source'] != xlate_source_sha(): bad.append(f'translator embedded source differs from its lock: embedded {now["xlate_source"]} / pinned {xlate_source_sha()}')
    return bad

def header_text(v):
    return (f'// Generated file (dm/gfx/tools/dmsh_chain.py --header): the B shader library translation chain version this source tree expects (default options), one for every game;\n'
            f'// the host loads only <library dir>/b-<V>/. Run dmsh_chain.py --header after changing the chain\'s tools and commit this file with the change\n'
            f'#define DMSH_CHAIN "{v}"\n')

if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['--update-lock']:
        t = tools_now()
        if t['xlate_source'] != xlate_source_sha(): sys.exit(f'[RED] translator embedded source differs from its pin (embedded {t["xlate_source"]} / pinned {xlate_source_sha()})')
        open(LOCK, 'w').write('# Translation chain tool fingerprints (written by dmsh_chain.py --update-lock; regenerate after changing tools and commit it)\n' + ''.join(f'{k}\t{v}\n' for k, v in t.items()))
        print('wrote', LOCK)
    elif a == ['--check-tools']:
        bad = check_tools()
        for b in bad: print('[RED] translation chain tools do not match the lock file: ' + b)
        sys.exit(1 if bad else 0)
    elif a == ['--header']:
        open(HEADER, 'w').write(header_text(version()))
        print(version())
    elif a == ['--inputs']: print(inputs(load_game()))
    elif not a: print(version())
    else: sys.exit(__doc__ if __doc__ else 'usage: see the file header')
