#!/usr/bin/env python3
"""Copy the part of an nxnative checkout that builds Donkey Kong Bananza into games/dkb (Q150).

    import_nxnative.py <nxnative checkout> --base <upstream commit>

What is copied: every file git tracks under dm/ at the checkout's HEAD, except the other games' profiles (dm/games/<title ID>/
of any other game), with the SPIR-V to Metal converter binary as nxnative ships it (build_dkb.py checks it against the fingerprint
dmsh_chain.lock records); then the three build scripts the NSP script loads (scripts/build_ipa_from_nsp.py, pack_dump.py,
pack_nup.py) and nxnative's LICENSE.
  dm/<path>        -> games/dkb/runtime/<path>
  scripts/<name>   -> games/dkb/third_party/nxnative/scripts/<name>
  LICENSE          -> games/dkb/third_party/nxnative/LICENSE
Written beside them: import-manifest.json (the base commit, each file's source, destination, SHA-256 of the copied bytes and whether
it differs from the base commit) and local-changes.patch (the checkout's changes over the base commit in the copied files).
A re-import replaces runtime/ and third_party/nxnative/ completely, so no file of an older import stays behind.
"""
import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
TITLE = '040074A01BF12000'
SCRIPTS = ('build_ipa_from_nsp.py', 'pack_dump.py', 'pack_nup.py')
# Nothing is left to the packaging any more: the SPIR-V to Metal converter is carried as nxnative ships it (2026-11-03: built here
# from its pinned source with Xcode 27.2 it came out 098403a993503b3a, dmsh_chain.lock expects e37945d8e441d34d, so a build from
# source is another shader chain and every shader would be translated again). build_dkb.py still checks the carried one.
BUILT_AT_PACKAGING = ()
OTHER_PROFILE = re.compile(r'^dm/games/([0-9A-F]{16})/')


def git(repo, *args, binary=False):
    r = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, check=True)
    return r.stdout if binary else r.stdout.decode()


def selection(repo):
    """(source paths to copy, other profiles left out, files built at packaging left out)."""
    files, others = [], set()
    for path in git(repo, 'ls-files', 'dm').splitlines():
        m = OTHER_PROFILE.match(path)
        if m and m.group(1) != TITLE:
            others.add(f'dm/games/{m.group(1)}/')
            continue
        if path in BUILT_AT_PACKAGING:
            continue
        files.append(path)
    files += [f'scripts/{name}' for name in SCRIPTS] + ['LICENSE']
    return files, sorted(others), list(BUILT_AT_PACKAGING)


def destination(src):
    if src.startswith('dm/'):
        return Path('runtime') / src[3:]
    if src.startswith('scripts/'):
        return Path('third_party/nxnative/scripts') / src[8:]
    return Path('third_party/nxnative') / src


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('checkout', type=Path)
    ap.add_argument('--base', required=True, help='the upstream nxnative commit the checkout is on (its local changes go into local-changes.patch)')
    a = ap.parse_args()
    repo = a.checkout.expanduser().resolve()
    if git(repo, 'status', '--porcelain', '--untracked-files=no').strip():
        sys.exit('error: the nxnative checkout has uncommitted changes; commit them first, so the import names one state')
    base = git(repo, 'rev-parse', a.base).strip()
    head = git(repo, 'rev-parse', 'HEAD').strip()
    if subprocess.run(['git', '-C', str(repo), 'merge-base', '--is-ancestor', base, head]).returncode != 0:
        sys.exit(f'error: {a.base} is not an ancestor of the checkout HEAD {head[:8]}')
    files, others, packaged = selection(repo)
    for d in (GAME / 'runtime', GAME / 'third_party/nxnative'):
        if d.exists():
            shutil.rmtree(d)
    entries = []
    for src in files:
        data = (repo / src).read_bytes()
        dst = GAME / destination(src)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        if (repo / src).stat().st_mode & 0o111:
            dst.chmod(0o755)
        try:
            at_base = git(repo, 'show', f'{base}:{src}', binary=True)
        except subprocess.CalledProcessError:
            at_base = None
        entries.append({'source': src, 'destination': str(destination(src)), 'sha256': hashlib.sha256(data).hexdigest(),
                        'changed_here': at_base != data})
    manifest = {'repository': 'https://github.com/seedds/nxnative', 'commit': base, 'checkout_head': head,
                'imported': datetime.date.today().isoformat(), 'files': entries,
                'left_out_other_profiles': others, 'built_at_packaging': packaged}
    nx = GAME / 'third_party/nxnative'
    (nx / 'import-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (nx / 'local-changes.patch').write_text(git(repo, 'diff', base, head, '--', *files))
    changed = sum(e['changed_here'] for e in entries)
    print(f'imported {len(entries)} files from nxnative {base[:8]} (+ {head[:8]}), {changed} changed here, '
          f'{len(others)} other profiles left out, {len(packaged)} file built at packaging')


if __name__ == '__main__':
    main()
