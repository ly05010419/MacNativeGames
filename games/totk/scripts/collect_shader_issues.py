#!/usr/bin/env python3
"""Merge the shader list contributions players sent as GitHub issues (label shader-list, the "Shader list contribution"
issue form) into games/totk/assets/shader_list.tsv.

A player's game writes shader-list-contribution.txt (third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h) and every 500
new rows asks the player to attach it to such an issue. This tool downloads each attachment, keeps only V rows (shader
key + bind state; anything else in a contribution is dropped) and reports what each issue adds. Nothing changes unless
asked: --apply writes the list, --close also thanks each merged issue and closes it.

usage: collect_shader_issues.py [--repo owner/name] [--apply] [--close] [--issue N ...]
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
from shader_list import KEY, ShaderList, canonical  # noqa: E402

SHADER_LIST = TOTK / 'assets/shader_list.tsv'
REPO = 'ly05010419/MacNativeGames'
LABEL = 'shader-list'
ATTACHMENT = re.compile(r'https://github\.com/(?:user-attachments/files|[\w.-]+/[\w.-]+/files)/\d+/[^\s)\]>"]+')
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


def gh(*args):
    return subprocess.run(['gh', *args], capture_output=True, text=True, check=True).stdout


def download(url, token):
    request = urllib.request.Request(url, headers={'Authorization': f'token {token}'} if token else {})
    with urllib.request.urlopen(request, timeout=60) as reply:
        data = reply.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError('larger than a contribution can be')
    return data.decode('utf-8', errors='replace')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--repo', default=REPO)
    ap.add_argument('--issue', type=int, nargs='*', help='only these issues (default: every open issue labelled shader-list)')
    ap.add_argument('--apply', action='store_true', help='write the merged rows to the shared list')
    ap.add_argument('--close', action='store_true', help='with --apply: comment on each merged issue and close it')
    args = ap.parse_args()
    if args.close and not args.apply:
        ap.error('--close needs --apply')
    issues = json.loads(gh('issue', 'list', '--repo', args.repo, '--label', LABEL, '--state', 'open', '--limit', '200',
                           '--json', 'number,title,body,author'))
    if args.issue:
        issues = [i for i in issues if i['number'] in args.issue]
    token = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True).stdout.strip()
    shared = ShaderList.load(SHADER_LIST)
    before = len(shared.variants)
    merged = []
    for issue in issues:
        urls = attachment_urls(issue['body'])
        rows, dropped, problems = set(), 0, []
        for url in urls:
            try:
                got, bad = contribution_rows(download(url, token))
                rows |= got; dropped += bad
            except (OSError, ValueError) as error:
                problems.append(f'{url}: {error}')
        new = rows - shared.variants
        shared.variants |= rows
        print(f"#{issue['number']} {issue['author']['login']}: {len(urls)} file(s), {len(rows)} rows, {len(new)} new"
              + (f', {dropped} lines dropped' if dropped else '') + ''.join(f'\n  {p}' for p in problems))
        if rows and not problems:
            merged.append((issue['number'], len(rows), len(new)))
    print(f'shared shader list: {before} -> {len(shared.variants)} variants')
    if not args.apply:
        print('dry run: nothing written (--apply writes the list)')
        return
    shared.save(SHADER_LIST)
    print(f'written: {SHADER_LIST}')
    for number, rows, new in merged if args.close else []:
        gh('issue', 'comment', str(number), '--repo', args.repo, '--body',
           f'Thank you! {rows} rows, {new} of them new to the shared shader list; they come with the next build. '
           f'谢谢！共 {rows} 条，其中 {new} 条是公共清单里新增的，下次打包就会带上。')
        gh('issue', 'close', str(number), '--repo', args.repo)


if __name__ == '__main__':
    main()
