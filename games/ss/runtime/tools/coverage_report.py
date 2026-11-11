#!/usr/bin/env python3
# Coverage summary for the job summary of CI's coverage job (.github/workflows/tests.yml): line coverage per area, the largest files, and the diff coverage of a change.
# It reads what the measuring tools wrote and prints Markdown; it never fails on coverage (report only), only on a missing or unreadable input it was given.
#   --host / --textures   llvm-cov's lcov export (make -C dm/host/tests GAME=<id> coverage: dm/host/tests/build/cov/coverage.lcov; make -C dm/gfx/tex/tests coverage:
#                         dm/gfx/tex/tests/cov/coverage.lcov). Its DA lines are llvm-cov's own line counts, the lines `llvm-cov show` marks, one view for totals and diff
#                         lines (`llvm-cov report` adds up functions instead, so a line two functions share, a block in a method, counts twice there)
#   --python              coverage.py's JSON (`coverage json`, after `coverage run` and `coverage combine` over the tool tests; pyproject.toml configures it)
#   --base REV            diff coverage: the lines `git diff REV...HEAD` adds or changes in C, Objective-C and Python sources, split into those the tests run and those
#                         they do not. Only lines the coverage data knows (llvm-cov's mapped lines, coverage.py's statements) count; comments and blank lines do not
# docs/development.md (Continuous integration) has the commands.
import argparse, json, os, re, subprocess, sys, pathlib

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ('.c', '.h', '.m', '.mm', '.py')
AREAS = (('host', 'Host', 'dm/host/tests'), ('textures', 'CPU textures', 'dm/gfx/tex/tests (t_tex)'), ('python', 'Python tools', 'dm/tools/tests'))

def rel(path, root):
    p = os.path.normpath(path if os.path.isabs(path) else os.path.join(root, path))
    r = os.path.relpath(p, root)
    return p if r.startswith('..') else r.replace(os.sep, '/')

def read_lcov(text, root):
    # {file: {line: covered}} from an lcov tracefile (llvm-cov export -format=lcov): SF names a file, DA:<line>,<count> each mapped line; a file listed twice is merged line by line
    files, lines = {}, None
    for line in text.splitlines():
        if line.startswith('SF:'): lines = files.setdefault(rel(line[3:], root), {})
        elif line.startswith('DA:') and lines is not None:
            ln, c = line[3:].split(',')[:2]
            lines[int(ln)] = lines.get(int(ln), False) or int(c) > 0
        elif line == 'end_of_record': lines = None
    return files

def read_python(doc, root):
    # {file: {line: covered}} from coverage.py's JSON report: its statements, executed or missing
    files = {}
    for name, f in doc['files'].items():
        lines = files.setdefault(rel(name, root), {})
        for ln in f['missing_lines']: lines.setdefault(ln, False)
        for ln in f['executed_lines']: lines[ln] = True
    return files

def changed_lines(diff):
    # {file: set of new-side line numbers} from a unified diff (-U0 is enough); deleted files and lines are not in it
    out, path = {}, None
    for line in diff.splitlines():
        if line.startswith('+++ '):
            t = line[4:].split('\t')[0]
            path = t[2:] if t.startswith('b/') else None
        elif line.startswith('@@') and path:
            m = re.match(r'@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@', line)
            if m:
                start, count = int(m[1]), int(m[2]) if m[2] is not None else 1
                if count: out.setdefault(path, set()).update(range(start, start + count))
    return out

def diff_coverage(changed, files):
    # per changed source file: (lines run, lines not run), counting only lines the coverage data knows; plus the changed sources no area measures
    rows, unmeasured = {}, []
    for path in sorted(changed):
        if not path.endswith(SOURCE) or '/tests/' in '/' + path or '/third_party/' in path: continue
        if path not in files: unmeasured.append(path); continue
        known = [ln for ln in sorted(changed[path]) if ln in files[path]]
        if known: rows[path] = ([ln for ln in known if files[path][ln]], [ln for ln in known if not files[path][ln]])
    return rows, unmeasured

def ranges(lines):
    runs: list[list[int]] = []   # [first, last] of each run of consecutive lines
    for ln in sorted(lines):
        if runs and ln == runs[-1][1] + 1: runs[-1][1] = ln
        else: runs.append([ln, ln])
    return ', '.join(f'{a}' if a == b else f'{a}-{b}' for a, b in runs)

def pct(covered, total): return f'{100 * covered / total:.1f}%' if total else '–'

def markdown(areas, top=15, base=None, diff=None, unmeasured=()):
    # areas: [(name, tests, {file: {line: covered}} or None when not measured)]; diff: diff_coverage's rows, or None without --base
    out = ['## Coverage', '',
           'Report only: this job never fails on coverage. Line coverage of what the unit tests run, without the tests themselves and third-party code. C: the files the '
           'tests compile (others are not listed); Python: every file under `dm/tools`, `dm/gfx/tools`, `dm/games` and `scripts`, imported by a test or not.', '',
           '| Area | Tests | Files | Lines | Covered | % |', '|---|---|---:|---:|---:|---:|']
    allf = []
    for name, tests, files in areas:
        if files is None: out.append(f'| {name} | {tests} | | | | not measured |'); continue
        n = sum(len(v) for v in files.values()); c = sum(sum(v.values()) for v in files.values())
        out.append(f'| {name} | {tests} | {len(files)} | {n} | {c} | {pct(c, n)} |')
        allf += [(len(v), sum(v.values()), f, name) for f, v in files.items() if v]
    if allf:
        out += ['', f'### The {top} largest files', '', '| File | Area | Lines | Covered | % |', '|---|---|---:|---:|---:|']
        for n, c, f, name in sorted(allf, key=lambda t: (-t[0], t[2]))[:top]: out.append(f'| `{f}` | {name} | {n} | {c} | {pct(c, n)} |')
    if diff is not None:
        run = sum(len(r) for r, _ in diff.values()); miss = sum(len(m) for _, m in diff.values())
        out += ['', f'### Diff coverage against `{base}`', '']
        if not diff: out.append('No changed line in a C, Objective-C or Python source that the tests measure.')
        else:
            out += [f'{run} of {run + miss} changed lines that the tests measure are run ({pct(run, run + miss)}).', '',
                    '| File | Run | Not run | % | Lines not run |', '|---|---:|---:|---:|---|']
            for f, (r, m) in diff.items(): out.append(f'| `{f}` | {len(r)} | {len(m)} | {pct(len(r), len(r) + len(m))} | {ranges(m)} |')
        if unmeasured: out += ['', 'Changed sources no test measures: ' + ', '.join(f'`{f}`' for f in unmeasured) + '.']
    return '\n'.join(out) + '\n'

def load(path):
    try: return pathlib.Path(path).read_text()
    except OSError as e: sys.exit(f'[RED] coverage_report: cannot read {path}: {e}')

def load_json(path):
    try: return json.loads(load(path))
    except ValueError as e: sys.exit(f'[RED] coverage_report: cannot read {path}: {e}')

def main(argv=None):
    ap = argparse.ArgumentParser(description='Markdown coverage summary from llvm-cov lcov and coverage.py JSON, with diff coverage against a base revision')
    ap.add_argument('--host', help="llvm-cov lcov export of the host tests (make -C dm/host/tests coverage)")
    ap.add_argument('--textures', help="llvm-cov lcov export of the CPU texture test (make -C dm/gfx/tex/tests coverage)")
    ap.add_argument('--python', help="coverage.py's JSON report of the tool tests")
    ap.add_argument('--base', help='diff coverage of the changes since the merge base with this revision (git diff BASE...HEAD)')
    ap.add_argument('--top', type=int, default=15, help='how many of the largest files to list (default 15)')
    ap.add_argument('--root', default=str(REPO), help='the checkout the reports were made in (default: this one)')
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    areas, merged = [], {}
    for key, name, tests in AREAS:
        path = getattr(a, key)
        files = None if not path else read_python(load_json(path), root) if key == 'python' else read_lcov(load(path), root)
        areas.append((name, tests, files))
        for f, lines in (files or {}).items():
            m = merged.setdefault(f, {})
            for ln, c in lines.items(): m[ln] = m.get(ln, False) or c
    diff = unmeasured = None
    if a.base:
        r = subprocess.run(['git', 'diff', '--no-color', '--no-ext-diff', '-U0', f'{a.base}...HEAD'], cwd=root, capture_output=True, text=True)
        if r.returncode: sys.exit(f'[RED] coverage_report: git diff {a.base}...HEAD failed: {r.stderr.strip()}')
        diff, unmeasured = diff_coverage(changed_lines(r.stdout), merged)
    sys.stdout.write(markdown(areas, a.top, a.base, diff, unmeasured or ()))

if __name__ == '__main__':
    main()
