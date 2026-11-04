#!/usr/bin/env python3
"""Merge the shader list contributions players sent as GitHub issues (label shader-list, the "Shader list contribution"
issue form) into games/totk/assets/shader_list.tsv; with --game botw, Breath of the Wild's (label shader-list-botw, its own
form) into games/botw/assets/shader_list.tsv.

A player's game writes shader-list-contribution.txt (third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h) and every 50
new rows asks the player to attach it to such an issue. "Submit automatically" (gh on the player's Mac) sends it as a
secret gist linked from an issue titled "Shader list: ..." in the form's layout; such an issue has no label (GitHub drops
labels set by people without write access), so issues are found by label or by that title. This tool downloads each
attachment or gist file, keeps only V rows (shader key + bind state) and R / B rows (render pipelines; anything else in a contribution is dropped) and
reports what each issue adds. With --seed (a prepared seed of the selected game version), rows its built-in variants
serve are dropped too: the list keeps only non-default variants (user 2026-10-22). Nothing changes unless
asked: --apply writes the list, --close also thanks each merged issue and closes it.

usage: collect_shader_issues.py [--game totk|botw] [--repo owner/name] [--seed <seed folder>] [--apply] [--close] [--issue N ...]
needs: the GitHub CLI (gh), signed in."""
import argparse
import json
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOTK / 'scripts'))
from shader_list import KEY, ShaderList, canonical, warm_row  # noqa: E402
from prepare_shader_seed import builtin_serves  # noqa: E402

# Per game: the list, the issue form's label, the title prefix of the launcher's automatic submissions (GAME_CONTRIB_TITLE)
GAMES = {'totk': (TOTK / 'assets/shader_list.tsv', 'shader-list', 'Shader list:'),
         'botw': (TOTK.parent / 'botw/assets/shader_list.tsv', 'shader-list-botw', 'Shader list (Breath of the Wild):')}
REPO = 'ly05010419/MacNativeGames'
ATTACHMENT = re.compile(r'https://github\.com/(?:user-attachments/files|[\w.-]+/[\w.-]+/files)/\d+/[^\s)\]>"]+'
                        r'|https://gist\.githubusercontent\.com/[\w-]+/[0-9a-f]+/raw/[^\s)\]>"]+')
MAX_BYTES = 8 << 20   # a contribution is a few hundred rows; anything this large is not one


def attachment_urls(body):
    """The attachment links in an issue body, in order, without repeats."""
    seen = []
    for url in ATTACHMENT.findall(body or ''):
        if url not in seen:
            seen.append(url)
    return seen


def contribution_rows(text):
    """({(key, canonical deps)}, number of lines dropped) from a contribution file's text."""
    rows, dropped = set(), 0
    for line in text.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        fields = line.split('\t')
        if fields[0] in ('R', 'B') and warm_row(fields):   # a render pipeline: contribution_pipelines
            continue
        deps = None
        if len(fields) == 3 and fields[0] == 'V' and KEY.match(fields[1]):
            try:
                deps = canonical(json.loads(fields[2]))
            except ValueError:
                deps = None
        if deps is None:
            dropped += 1
        else:
            rows.add((fields[1], deps))
    return rows, dropped


def contribution_pipelines(text):
    """The render pipeline rows (R, and B with its border recipe) of a contribution file, in order, without repeats
    (nvn_mtl.m pso_contribute; user 2026-10-22: collected with the shaders)."""
    rows = []
    for line in text.splitlines():
        fields = line.split('\t')
        row = warm_row(fields) if fields[0] in ('R', 'B') else None
        if row and row not in rows:
            rows.append(row)
    return rows


def gh(*args):
    try:
        return subprocess.run(['gh', *args], capture_output=True, text=True, check=True).stdout
    except FileNotFoundError:
        # A maintainer's tool (#85): players send their file through the game's prompt or the issue form instead
        sys.exit('needs the GitHub CLI (gh), signed in: brew install gh && gh auth login. Players do not need this tool: '
                 "attach shader-list-contribution.txt to a \"Shader list contribution\" issue instead.")


def download(url, token):
    own = url.startswith('https://github.com/')   # the token goes to github.com only; gist raw files are public
    request = urllib.request.Request(url, headers={'Authorization': f'token {token}'} if token and own else {})
    with urllib.request.urlopen(request, timeout=60) as reply:
        data = reply.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError('larger than a contribution can be')
    return data.decode('utf-8', errors='replace')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--game', choices=sorted(GAMES), default='totk', help='whose list and issues: totk (default) or botw')
    ap.add_argument('--repo', default=REPO)
    ap.add_argument('--issue', type=int, nargs='*', help="only these issues (default: every open issue with the game's label)")
    ap.add_argument('--seed', type=Path, help='a prepared seed (build/shader-seed/<version>): drop rows its built-in variants serve')
    ap.add_argument('--apply', action='store_true', help='write the merged rows to the shared list')
    ap.add_argument('--close', action='store_true', help='with --apply: comment on each merged issue and close it')
    args = ap.parse_args()
    if args.close and not args.apply:
        ap.error('--close needs --apply')
    shader_list, label, title_prefix = GAMES[args.game]
    fields = ('--state', 'open', '--limit', '200', '--json', 'number,title,body,author')
    labelled = json.loads(gh('issue', 'list', '--repo', args.repo, '--label', label, *fields))
    titled = json.loads(gh('issue', 'list', '--repo', args.repo, '--search', f'"{title_prefix}" in:title', *fields))
    issues = list({i['number']: i for i in labelled + [i for i in titled if i['title'].startswith(title_prefix)]}.values())
    if args.issue:
        issues = [i for i in issues if i['number'] in args.issue]
    token = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True).stdout.strip()
    shared = ShaderList.load(shader_list)
    before, before_pipes = len(shared.variants), sum(1 for row in shared.warm if row[0] in 'RB')
    merged = []
    for issue in issues:
        urls = attachment_urls(issue['body'])
        rows, pipes, dropped, problems = set(), [], 0, []
        for url in urls:
            try:
                text = download(url, token)
                got, bad = contribution_rows(text)
                rows |= got; dropped += bad
                pipes += [row for row in contribution_pipelines(text) if row not in pipes]
            except (OSError, ValueError) as error:
                problems.append(f'{url}: {error}')
        builtin = {r for r in rows if builtin_serves(args.seed, *r)} if args.seed else set()
        rows -= builtin
        new = rows - shared.variants
        shared.variants |= rows
        new_pipes = [row for row in pipes if row not in shared.warm]
        for row in new_pipes:
            shared.add_warm(row, 'm')
        print(f"#{issue['number']} {issue['author']['login']}: {len(urls)} file(s), {len(rows)} rows, {len(new)} new"
              + (f', {len(pipes)} pipeline rows, {len(new_pipes)} new' if pipes else '')
              + (f', {dropped} lines dropped' if dropped else '')
              + (f', {len(builtin)} default rows the seed makes dropped' if builtin else '') + ''.join(f'\n  {p}' for p in problems))
        if (rows or builtin or pipes) and not problems:   # all default: still thanked and closed
            merged.append((issue['number'], len(rows) + len(pipes), len(new) + len(new_pipes)))
    print(f'shared shader list: {before} -> {len(shared.variants)} variants, '
          f'{before_pipes} -> {sum(1 for row in shared.warm if row[0] in "RB")} render pipeline rows')
    if not args.apply:
        print('dry run: nothing written (--apply writes the list)')
        return
    shared.save(shader_list)
    print(f'written: {shader_list}')
    for number, rows, new in merged if args.close else []:
        gh('issue', 'comment', str(number), '--repo', args.repo, '--body',
           f'Thank you! {rows} rows, {new} of them new to the shared shader list; they come with the next build. '
           f'谢谢！共 {rows} 条，其中 {new} 条是公共清单里新增的，下次打包就会带上。')
        gh('issue', 'close', str(number), '--repo', args.repo)


if __name__ == '__main__':
    main()
