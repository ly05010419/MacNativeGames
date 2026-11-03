#!/usr/bin/env python3
# shader_regressions.py <library dir> [<chain version>]: the shaders that translated in the game's previous build and fail in this one. The translation chain is one for every game,
# so a translator or host change is checked only on the games its author owns; on another game the first sign was a crash at run time (GitHub issue #272: c3a0880 broke the image
# atomics of SHIN MEGAMI TENSEI V and Yoshi's Crafted World). This compares <library dir>/b-<V>/_b_build.json (default: the current chain, dmsh_chain.py) with the newest other
# b-*/_b_build.json in the same folder, skipping one stamped with another game. It reads only the build reports dmsh_build.py writes, so it is not part of the chain.
# The NSP script runs it after each shader build and stops on a regression unless told to go on (ALLOW_SHADER_REGRESSIONS / --allow-shader-regressions).
# Exit status: 0 = none, or no earlier build to compare with; 1 = regressions, one [RED] line each; 2 = no report for this chain.
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmsh_chain

REPORT = '_b_build.json'


def load(path):
    with open(path) as f: return json.load(f)


def previous(lib_dir, chain):
    """The newest b-*/ beside b-<chain>/ with a build report, of the same game when both are stamped; None when there is none"""
    game = dmsh_chain.read_stamp(os.path.join(lib_dir, 'b-' + chain)).get('game')
    found = []
    for name in os.listdir(lib_dir):
        report = os.path.join(lib_dir, name, REPORT)
        if not name.startswith('b-') or name == 'b-' + chain or not os.path.isfile(report): continue
        other = dmsh_chain.read_stamp(os.path.join(lib_dir, name)).get('game')
        if game and other and other != game: continue
        found.append((os.path.getmtime(report), name))
    return max(found)[1] if found else None


def compare(lib_dir, chain):
    """(previous b-* name or None, [(key, status, reason)] for the shaders that were ok there and are not now). Keys missing from either build are new or not rebuilt: not counted"""
    now = load(os.path.join(lib_dir, 'b-' + chain, REPORT))
    prev = previous(lib_dir, chain)
    if prev is None: return None, []
    return prev, lost(load(os.path.join(lib_dir, prev, REPORT)), now)


def lost(prev_rows, now_rows):
    """[(key, status, reason)] for the shaders ok in prev_rows and not in now_rows (two build reports' rows); scripts/regress.py compares two revisions' builds with this too"""
    was_ok = {r['key'] for r in prev_rows if r['st'] == 'ok'}
    return [(r['key'], r['st'], r['err']) for r in now_rows if r['st'] != 'ok' and r['key'] in was_ok]


def main(argv):
    if len(argv) not in (2, 3): sys.exit('usage: shader_regressions.py <library dir> [<chain version>]')
    lib_dir, chain = argv[1], argv[2] if len(argv) == 3 else dmsh_chain.version()
    if not os.path.isfile(os.path.join(lib_dir, 'b-' + chain, REPORT)):
        print(f'[RED] no build report {os.path.join(lib_dir, "b-" + chain, REPORT)}: dmsh_build.py has not built chain {chain} there'); return 2
    prev, lost = compare(lib_dir, chain)
    if prev is None:
        print(f'shader regressions: no earlier build in {lib_dir} to compare b-{chain} with'); return 0
    print(f'shader regressions against {prev}: {len(lost)}')
    for key, st, err in lost: print(f'[RED] {key}: translated in {prev}, {st} in b-{chain}: {err}')
    return 1 if lost else 0


if __name__ == '__main__': sys.exit(main(sys.argv))
