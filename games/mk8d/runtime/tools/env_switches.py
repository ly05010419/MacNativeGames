#!/usr/bin/env python3
"""The DM_* environment switches the code reads, as one table: docs/env_switches.tsv (name, class, meaning; class and meaning kept by hand, docs/development.md says what
the classes are).
  env_switches.py           write the table: a new switch gets a row with a guessed class and no meaning, a switch nothing reads any more loses its row
  env_switches.py --check   exit 1 when the table does not list exactly the switches the sources read, or a row has no meaning or an unknown class
Either prints the names the docs quote that nothing reads (a removed switch a doc may still describe as working)."""
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
TABLE = REPO / 'docs/env_switches.tsv'
HEAD = 'name\tclass\tmeaning\n'
CLASSES = ('player', 'path', 'switch', 'diagnostic', 'test')
CODE = ('.c', '.m', '.h', '.inc', '.swift', '.py', '.sh', '.S')
NAME = r'DM_[A-Z0-9_]*[A-Z0-9]'
READ = r'(?:getenv\(|environ(?:\.get\(|\[))\s*["\'](' + NAME + r')["\']'   # a literal name in getenv(…), os.environ.get(…) or os.environ[…]


def tracked():
    out = subprocess.run(['git', 'ls-files', 'dm', 'scripts', 'docs', 'README.md'], cwd=REPO, capture_output=True, text=True, check=True).stdout.split('\n')
    return [p for p in out if p and 'third_party' not in p.split('/')]


def scan():
    """({switch: files that read it}, {name quoted in the docs: docs})"""
    reads, docs, macros, direct = {}, {}, set(), set()
    for p in tracked():
        path = REPO / p
        if not path.is_file(): continue
        text = path.read_text(errors='replace')
        if p.endswith('.md'):
            for n in set(re.findall(r'`(' + NAME + r')', text)): docs.setdefault(n, set()).add(p)
            continue
        if not p.endswith(CODE) and not p.startswith('scripts/'): continue
        # names the code defines as preprocessor macros (the generated profile header's DM_GAME_*, host constants) are not switches, unless something also reads
        # them from the environment (DM_GAME_ENV, which overrides the profile's macro of that name)
        macros |= set(re.findall(r'#\s*define\s+(' + NAME + r')', text))
        if '/tests/' in p:   # a test's own switches only; the names in its fixtures are data
            found = re.findall(READ, text)
        elif p.endswith('.sh'): found = re.findall(r'\$\{?(' + NAME + r')\b', text) + re.findall(r'["\'](' + NAME + r')["\']', text)
        else: found = re.findall(r'["\'](' + NAME + r')=?["\']', text)   # any quoted name: helpers such as envdir("DM_TEMP", …) read it for their caller
        direct |= set(re.findall(READ, text))
        for n in set(found): reads.setdefault(n, set()).add(p)
    for n in macros - direct: reads.pop(n, None); docs.pop(n, None)
    return reads, docs


def guess(name, files, player):   # a new row's class, for whoever writes its meaning to correct
    if name in player: return 'player'
    if all('/tests/' in f for f in files): return 'test'
    if re.search(r'_(DIR|PATH|FILE|ROOT)$', name): return 'path'
    return 'diagnostic'


def rows():
    """(table text, rows without a meaning or with an unknown class, names the docs quote that nothing reads)"""
    reads, docs = scan()
    player = {l.split('\t')[0] for l in (REPO / 'dm/games/settings.tsv').read_text().split('\n') if l.startswith('DM_')}
    kept = {}
    if TABLE.exists():
        for l in TABLE.read_text().split('\n'):
            f = l.split('\t')
            if len(f) == 3 and f[0].startswith('DM_'): kept[f[0]] = f[1:]
    out, bad = [], []
    for n in sorted(reads):
        cls, meaning = kept.get(n, [guess(n, reads[n], player), ''])
        if cls not in CLASSES or not meaning.strip(): bad.append(n)
        out.append(f'{n}\t{cls}\t{meaning}\n')
    unread = sorted(n for n in docs if n not in reads and not n.endswith('_'))
    return HEAD + ''.join(out), bad, unread


if __name__ == '__main__':
    text, bad, unread = rows()
    if unread: print('quoted in the docs but read nowhere (check that each mention says it was removed): ' + ' '.join(unread))
    if sys.argv[1:] == ['--check']:
        problems = []
        if not TABLE.exists() or TABLE.read_text() != text: problems.append('docs/env_switches.tsv does not list the switches the code reads: run dm/tools/env_switches.py, then write the new rows\' meanings')
        if bad: problems.append('switches without a meaning or a class (one of ' + ', '.join(CLASSES) + '): ' + ' '.join(bad))
        print('\n'.join(problems))
        sys.exit(1 if problems else 0)
    TABLE.write_text(text)
    print(f'{TABLE.relative_to(REPO)}: {text.count(chr(10)) - 1} switches, {len(bad)} without a meaning')
