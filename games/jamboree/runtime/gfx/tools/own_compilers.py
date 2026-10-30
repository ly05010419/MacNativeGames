#!/usr/bin/env python3
# dmsh_build.py with this Mac's own compilers: the NSP script builds the iOS shader libraries through this file. dmsh_build.py stops when any tool differs from dmsh_chain.lock;
# here spirv-cross and the Metal compiler (macOS and iOS) may differ, with one note line naming them, while the translator (dmsh_xlate_bin) must still be the locked build.
# Neither the chain version nor any repository file changes: a player builds the shader libraries and the host on the same Mac, so the two agree whatever the compilers,
# and an Xcode or spirv-cross other than the maintainer's (a beta, a brew upgrade) no longer needs dmsh_chain.py --update-lock, which changes the chain version (one for every game).
# The lock stays the record of the compilers the libraries were tested with; on the maintainer's Mac dmsh_build.py keeps the strict check.
# The per-shader cache takes spirv-cross's version into its keys but the Metal compiler only through the lock, so after a Metal compiler change the shaders already in the cache
# keep the libraries the earlier compiler made and only new ones are compiled with this Mac's (the Metal compile cache, dmsh_mtlcache.py, is keyed by the compiler itself).
# Usage: own_compilers.py <dmsh_build.py arguments>   (the environment as for dmsh_build.py)
import os, re, sys, runpy
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); import dmsh_chain
OWN = re.compile(r'(spirv-cross|metal_macosx|metal_iphoneos): lock (.*) / actual (?!missing$)(.*)')   # a missing compiler still stops the build
strict = dmsh_chain.check_tools

def check_tools():
    bad, own = [], []
    for b in strict():
        if os.environ.get('DM_METAL_RUNTIME') == '1' and b.startswith(('metal_macosx:', 'metal_iphoneos:')):
            continue   # The MTLDevice runtime compiles MSL; no offline Metal compiler is needed.
        (own if OWN.fullmatch(b) else bad).append(b)
    if own and not bad: print('note: built with this Mac\'s compilers, not the ones the shaders were tested with (dmsh_chain.lock): ' +
                  '; '.join(f'{m[1]} {m[3]} (tested: {m[2]})' for m in map(OWN.fullmatch, own)), flush=True)
    return bad

dmsh_chain.check_tools = check_tools   # dmsh_build.py imports the same module object and calls it through the module
sys.argv = [os.path.join(HERE, 'dmsh_build.py'), *sys.argv[1:]]
runpy.run_path(sys.argv[0], run_name='__main__')
