#!/usr/bin/env python3
# Python lint and type check with a ratchet (.github/workflows/lint-python.yml): ruff (pyproject.toml's [tool.ruff]: lint rules for bugs, no formatting) and pyright in basic
# mode ([tool.pyright]) over scripts/ and the tools under dm/. The findings the code had when the check came in are listed in dm/tools/lint_python_baseline.tsv, keyed by tool,
# file, rule and message with a count (no line numbers: they survive edits elsewhere in the file). The check fails on a finding beyond its key's count (a new one) and on a
# count above what the code has now (a fixed finding still listed: run --update). So the baseline only shrinks:
#   --update      lowers counts to what the code has now and drops fixed entries; it never adds one (a new finding is fixed, or silenced on its line with a noqa or
#                 `# pyright: ignore[<rule>]` and a reason)
#   --rebaseline  writes every current finding: only with a raised tool version in .github/requirements/lint.txt, whose findings change with it
#   --against REV fails when the baseline has an entry or a count that REV's has not, unless that checker's pinned version increased (CI uses the PR's base)
# Run it with the pinned tools, so the findings match CI's (other versions or other packages in the environment change them):
#   python3 -m venv <dir> && <dir>/bin/pip install -r .github/requirements/lint.txt && <dir>/bin/python3 dm/tools/lint_python.py [--update]
# Under GitHub Actions, new findings are annotated on their lines and a summary goes to $GITHUB_STEP_SUMMARY.
import argparse, collections, json, os, re, subprocess, sys, pathlib

REPO = pathlib.Path(__file__).resolve().parents[2]
BASELINE = REPO / 'dm/tools/lint_python_baseline.tsv'
REQS = '.github/requirements/lint.txt'
HEADER = '# tool\tfile\trule\tcount\tmessage   (dm/tools/lint_python.py; shrink with --update, never edit by hand to add)\n'

def rel(p): return pathlib.Path(p).resolve().relative_to(REPO).as_posix()
def norm(msg): return re.sub(r'\s+', ' ', msg).strip()   # pyright's messages span lines and use no-break spaces

def run(cmd):
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if not r.stdout.strip(): sys.exit(f'[RED] lint_python: {" ".join(cmd[:3])} printed nothing (exit {r.returncode}):\n{r.stderr}')
    return r.stdout

def ruff():
    out = json.loads(run([sys.executable, '-m', 'ruff', 'check', '--no-cache', '--exit-zero', '--output-format', 'json', '.']))
    return [('ruff', rel(f['filename']), f['code'] or 'syntax', norm(f['message']), f['location']['row']) for f in out]

def pyright():
    out = json.loads(run([sys.executable, '-m', 'pyright', '--outputjson', '--pythonpath', sys.executable]))
    return [('pyright', rel(d['file']), d.get('rule') or d['severity'], norm(d['message']), d['range']['start']['line'] + 1)
            for d in out['generalDiagnostics'] if d['severity'] in ('error', 'warning')]

def load(text):
    base = collections.Counter()
    for line in text.splitlines():
        if not line or line.startswith('#'): continue
        tool, path, rule, n, msg = line.split('\t', 4)
        base[(tool, path, rule, msg)] = int(n)
    return base

def write(base):
    rows = sorted(f'{k[0]}\t{k[1]}\t{k[2]}\t{n}\t{k[3]}\n' for k, n in base.items() if n > 0)
    BASELINE.write_text(HEADER + ''.join(rows))

def git(*a): return subprocess.run(['git', *a], cwd=REPO, capture_output=True, text=True)

def versions(text):
    # The checkers use three-part release pins. Reject missing, duplicate or unsupported pins instead of authorizing a baseline rewrite.
    pins = {}
    for line in text.splitlines():
        name, _, version = line.split('#', 1)[0].strip().partition('==')
        name, version = name.strip().lower(), version.strip()
        if name not in ('ruff', 'pyright'): continue
        if name in pins or not re.fullmatch(r'\d+\.\d+\.\d+', version):
            raise ValueError(f'{REQS}: {name} must have one numeric release pin (name==X.Y.Z)')
        pins[name] = tuple(map(int, version.split('.')))
    if set(pins) != {'ruff', 'pyright'}: raise ValueError(f'{REQS}: both ruff and pyright need numeric release pins (name==X.Y.Z)')
    return pins

def against(rev, base):
    # Only an upgraded checker may gain entries or counts; dependency edits and downgrades do not change the ratchet.
    old = git('show', f'{rev}:{rel(BASELINE)}')
    if old.returncode: print(f'lint_python: no baseline at {rev}: nothing to compare'); return 0
    old_reqs = git('show', f'{rev}:{REQS}')
    if old_reqs.returncode: print(f'[RED] lint_python: cannot read {REQS} at {rev}: {old_reqs.stderr.strip()}'); return 1
    try:
        before, after = versions(old_reqs.stdout), versions((REPO / REQS).read_text())
    except (OSError, ValueError) as e:
        print(f'[RED] lint_python: {e}'); return 1
    upgraded = {t for t in before if after[t] > before[t]}
    for t in sorted(upgraded): print(f'lint_python: {t} upgraded since {rev}: its baseline may be rewritten')
    was = load(old.stdout)
    grown = [(k, n, was[k]) for k, n in base.items() if n > was[k] and k[0] not in upgraded]
    for (tool, path, rule, msg), n, w in grown: print(f'::error file={rel(BASELINE)}::baseline grew: {tool} {path} {rule} {w} -> {n}: {msg}')
    if grown: print(f'lint_python: the baseline gained {len(grown)} entries over {rev}: fix the new findings instead (it only shrinks)')
    return 1 if grown else 0

def summary(md):
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f: f.write(md + '\n')

def check(args):
    found = ruff() + pyright()
    cur = collections.Counter(f[:4] for f in found)
    base = load(BASELINE.read_text()) if BASELINE.exists() else collections.Counter()
    if args.rebaseline:
        write(cur); print(f'lint_python: wrote {sum(cur.values())} findings to {rel(BASELINE)}'); return 0
    new = {k: n - base[k] for k, n in cur.items() if n > base[k]}
    fixed = {k: base[k] - cur[k] for k in base if base[k] > cur[k]}
    if args.update:
        shrunk = collections.Counter({k: min(n, cur[k]) for k, n in base.items()})
        write(shrunk); print(f'lint_python: baseline {sum(base.values())} -> {sum(shrunk.values())} findings')
        base, fixed = shrunk, {}
    gha = os.environ.get('GITHUB_ACTIONS') == 'true'
    for tool, path, rule, msg, line in found:
        k = (tool, path, rule, msg)
        if new.get(k):   # with several of one key, every line is shown: which one is new is not known
            print(f'::error file={path},line={line},title={tool} {rule}::{msg}' if gha else f'{path}:{line}: {tool} {rule}: {msg}')
    for (tool, path, rule, msg), n in sorted(fixed.items()):
        print(f'fixed, still in the baseline: {tool} {path} {rule} x{n}: {msg}')
    n_new, n_fixed = sum(new.values()), sum(fixed.values())
    rows = ['| tool | findings | in the baseline | new | fixed, still listed |', '|---|---:|---:|---:|---:|']
    for t in ('ruff', 'pyright'):
        rows.append(f'| {t} | {sum(n for k, n in cur.items() if k[0] == t)} | {sum(n for k, n in base.items() if k[0] == t)} '
                    f'| {sum(n for k, n in new.items() if k[0] == t)} | {sum(n for k, n in fixed.items() if k[0] == t)} |')
    verdict = ('New findings: fix them (or silence one on its line with `# noqa: <rule>` / `# pyright: ignore[<rule>]` and a reason).' if n_new else
               'Fixed findings are still in the baseline: run `python3 dm/tools/lint_python.py --update` and commit it.' if n_fixed else
               'No new findings.')
    summary('### Python lint and types (ruff, pyright basic)\n\n' + '\n'.join(rows) + f'\n\n{verdict}\n')
    print(f'lint_python: {sum(cur.values())} findings, {sum(base.values())} in the baseline, {n_new} new, {n_fixed} fixed but still listed')
    if n_fixed: print('lint_python: run python3 dm/tools/lint_python.py --update to shrink the baseline')
    rc = 1 if n_new or n_fixed else 0
    if args.against: rc |= against(args.against, load(BASELINE.read_text()) if BASELINE.exists() else collections.Counter())
    return rc

def main():
    ap = argparse.ArgumentParser(description='ruff and pyright with a ratchet baseline')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--update', action='store_true', help='shrink the baseline to the findings the code still has')
    g.add_argument('--rebaseline', action='store_true', help='write every current finding (only after raising a pinned tool version)')
    ap.add_argument('--against', metavar='REV', help='also fail when the baseline grew over the one at REV')
    args = ap.parse_args()
    return check(args)

if __name__ == '__main__':
    sys.exit(main())
