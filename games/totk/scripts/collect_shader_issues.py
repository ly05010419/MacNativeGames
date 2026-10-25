#!/usr/bin/env python3
"""Merge the shader list contributions players sent as GitHub issues (labels shader-list and shader-list-botw, the two
"Shader list contribution" issue forms) into games/totk/assets/shader_list.tsv and games/botw/assets/shader_list.tsv.

A player's game writes shader-list-contribution.txt (third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h) and every 50
new rows asks the player to attach it to such an issue. "Submit automatically" (gh on the player's Mac) sends it as a
secret gist linked from an issue titled "Shader list: ..." in the form's layout; such an issue has no label (GitHub drops
labels set by people without write access), so issues are found by label or by that title. This tool downloads each
attachment or gist file (in the issue or its comments: a failed upload is often attached again in a comment), keeps
only V rows (shader key + bind state) and R / B rows (render pipelines; anything else in a contribution is dropped) and
reports what each issue adds.

One contribution file serves both games, so an issue titled for one game can carry the other's rows (2026-10-25: five
Breath of the Wild issues held Tears of the Kingdom rows; #525 was Super Mario Odyssey's list). Every row therefore goes
to the game its shader keys belong to, from the shader keys of each prepared seed (--seed <seed folder>, its
shader-keys.json; a seed of every game version collected) and of extra key lists (--keys <game>=<json list>). Rows of
keys neither game has are not merged and keep their issue open, since a key list may be missing a version; after
checking that they belong to no game here, --drop-unknown leaves them out and closes the issue. Rows the built-in
variants of a seed of the same game serve are dropped too: the list keeps only non-default variants (user 2026-10-22).
Nothing changes unless asked: --apply writes the lists, --close also thanks each merged issue and closes it.

usage: collect_shader_issues.py --seed <seed folder> ... [--keys <game>=<file> ...] [--repo owner/name] [--issue N ...]
                                [--drop-unknown] [--apply] [--close]
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
from shader_list import KEY, NAME, ShaderList, canonical, warm_row  # noqa: E402
from prepare_shader_seed import builtin_serves  # noqa: E402

# Per game: the list, the issue form's label, the start of the launcher's automatic submissions' title (GAME_CONTRIB_TITLE)
GAMES = {'totk': (TOTK / 'assets/shader_list.tsv', 'shader-list', 'Shader list:'),
         'botw': (TOTK.parent / 'botw/assets/shader_list.tsv', 'shader-list-botw', 'Shader list (Breath of the Wild')}
TITLES = {'totk': 'Tears of the Kingdom', 'botw': 'Breath of the Wild'}
TITLES_ZH = {'totk': '王国之泪', 'botw': '旷野之息'}
REPO = 'ly05010419/MacNativeGames'
ATTACHMENT = re.compile(r'https://github\.com/(?:user-attachments/files|[\w.-]+/[\w.-]+/files)/\d+/[^\s)\]>"]+'
                        r'|https://gist\.githubusercontent\.com/[\w-]+/[0-9a-f]+/raw/[^\s)\]>"]+')
MAX_BYTES = 64 << 20   # render pipelines made contributions big (#435: 10,188 rows); anything larger is not one


def attachment_urls(body):
    """The attachment links in an issue body, in order, without repeats."""
    seen = []
    for url in ATTACHMENT.findall(body or ''):
        if url not in seen:
            seen.append(url)
    return seen


def issue_urls(issue):
    """The attachment links of an issue and its comments (a file attached again after a failed upload), in order."""
    return attachment_urls('\n'.join([issue.get('body') or ''] + [c.get('body') or '' for c in issue.get('comments') or []]))


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


def key_source(spec):
    """(game, shader keys) from a prepared seed folder (its shader-keys.json, which names the version) or from
    "<game>=<file>" (a JSON list of keys, or a shader-keys.json). A seed without shader-keys.json (made before
    2026-10-22, its variants are the startup shaders only) cannot tell a key's game: ValueError."""
    game, _, path = spec.partition('=') if '=' in spec else ('', '', spec)
    if game and game not in GAMES:
        raise ValueError(f'{spec}: the game is one of {", ".join(GAMES)}')
    p = Path(path)
    if p.is_dir():
        p = p / 'shader-keys.json'
        if not p.is_file():
            raise ValueError(f'{path}: no shader-keys.json (a seed made before 2026-10-22); prepare the seed again')
    data = json.loads(p.read_text())
    keys = data['keys'] if isinstance(data, dict) else data
    if not game:
        version = data.get('version', '') if isinstance(data, dict) else ''
        game = 'botw' if version.startswith('botw') else 'totk' if version.startswith('v') else ''
        if not game:
            raise ValueError(f'{path}: the version does not say which game; give it as <game>=<file>')
    if not all(isinstance(k, str) and KEY.match(k) for k in keys):
        raise ValueError(f'{path}: not a list of shader keys')
    return game, set(keys)


def pipeline_shaders(row):
    """The shader keys a render pipeline row names: its key leads with the vertex and fragment variants, each a
    NUL-padded "<key>-<suffix>_b" of NAME bytes (shader_list.py)."""
    b = bytes.fromhex(row[1])
    names = [b[i:i + NAME].split(b'\0', 1)[0].decode('ascii', 'replace') for i in (0, NAME)]
    return [n[:64] for n in names if n]


def route(rows, pipes, keys):
    """({game: (variant rows, pipeline rows)}, (unknown variant rows, unknown pipeline rows)): each row goes to the game
    whose keys hold its shaders; a pipeline whose shaders are not all one game's is unknown."""
    def game_of(shaders):
        found = {g for s in shaders for g, k in keys.items() if s in k}
        return found.pop() if len(found) == 1 and all(any(s in k for k in keys.values()) for s in shaders) else None
    games, unknown = {}, (set(), [])
    for row in rows:
        g = game_of([row[0]])
        (games.setdefault(g, (set(), []))[0] if g else unknown[0]).add(row)
    for row in pipes:
        shaders = pipeline_shaders(row)
        g = game_of(shaders) if shaders else None
        (games.setdefault(g, (set(), []))[1] if g else unknown[1]).append(row)
    return games, unknown


def reply_text(per_game, titled):
    """The thanks for a merged issue: per_game {game: (rows, new rows)}; each game named when it is not only the one the
    issue was titled for."""
    if not per_game:   # --drop-unknown: none of its shaders are these games' (another game's list)
        return ('Thank you! None of the shaders in this file belong to Tears of the Kingdom or Breath of the Wild, so '
                'nothing was merged into their shared lists.\n\n谢谢！这份文件里的着色器都不属于王国之泪或旷野之息，所以没有并进它们的公共清单。')
    if list(per_game) == [titled]:
        rows, new = per_game[titled]
        return (f'Thank you! {rows} rows, {new} of them new to the shared shader list; they come with the next build.\n\n'
                f'谢谢！共 {rows} 条，其中 {new} 条是公共清单里新增的，下次打包就会带上。')
    en = '; '.join(f'{TITLES[g]}: {r} rows, {n} new' for g, (r, n) in per_game.items())
    zh = '；'.join(f'{TITLES_ZH[g]} {r} 条，其中新增 {n} 条' for g, (r, n) in per_game.items())
    return (f"Thank you! {en}. The contribution file is shared by both games, so each row went to its own game's list; "
            f'they come with the next build.\n\n谢谢！{zh}。贡献文件是两款游戏共用的，每一行已按所属游戏分别收进对应清单，下次打包就会带上。')


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


def open_issues(repo):
    """The open shader-list issues of both games, by label or by the automatic submissions' title."""
    fields = ('--state', 'open', '--limit', '500', '--json', 'number,title,body,author,labels,comments')
    found = {}
    for _, label, _ in GAMES.values():
        found.update({i['number']: i for i in json.loads(gh('issue', 'list', '--repo', repo, '--label', label, *fields))})
    titled = json.loads(gh('issue', 'list', '--repo', repo, '--search', '"Shader list" in:title', *fields))
    found.update({i['number']: i for i in titled if i['title'].lower().startswith('shader list')})
    return [found[n] for n in sorted(found)]


def titled_game(issue):
    labels = {label['name'] for label in issue.get('labels') or []}
    return 'botw' if GAMES['botw'][1] in labels or issue['title'].startswith(GAMES['botw'][2]) else 'totk'


def merge_issue(issue, lists, keys, seeds, token, drop_unknown):
    """Merge one issue's files into lists: (report line, {game: (rows, new rows)} or None when it stays open)."""
    urls = issue_urls(issue)
    rows, pipes, dropped, problems = set(), [], 0, []
    for url in urls:
        try:
            text = download(url, token)
            got, bad = contribution_rows(text)
            rows |= got; dropped += bad
            pipes += [row for row in contribution_pipelines(text) if row not in pipes]
        except (OSError, ValueError) as error:
            problems.append(f'{url}: {error}')
    games, (unknown_rows, unknown_pipes) = route(rows, pipes, keys)
    per_game, parts = {}, []
    for game, (g_rows, g_pipes) in games.items():
        shared = lists[game]
        builtin = {r for r in g_rows if any(builtin_serves(s, *r) for s in seeds.get(game, []))}
        new = (g_rows - builtin) - shared.variants
        shared.variants |= g_rows - builtin
        new_pipes = [row for row in g_pipes if row not in shared.warm]
        for row in new_pipes:
            shared.add_warm(row, 'm')
        per_game[game] = (len(g_rows) + len(g_pipes), len(new) + len(new_pipes))
        parts.append(f'{game} {len(g_rows)} rows ({len(new)} new, {len(builtin)} default dropped), '
                     f'{len(g_pipes)} pipeline rows ({len(new_pipes)} new)')
    unknown = len(unknown_rows) + len(unknown_pipes)
    line = (f"#{issue['number']} {issue['author']['login']}: {len(urls)} file(s); " + ('; '.join(parts) or 'nothing')
            + (f'; {dropped} lines dropped' if dropped else '')
            + (f'; {unknown} rows of shaders neither game has' + (' (left out)' if drop_unknown else ' (issue stays open)')
               if unknown else '') + ''.join(f'\n  {p}' for p in problems))
    closable = (rows or pipes) and not problems and (drop_unknown or not unknown)
    return line, per_game if closable else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--repo', default=REPO)
    ap.add_argument('--issue', type=int, nargs='*', help='only these issues (default: every open shader-list issue)')
    ap.add_argument('--seed', action='append', default=[], help='a prepared seed (build/shader-seed/<version>): its '
                    'shader keys, and its built-in variants drop default rows; one for each game version collected')
    ap.add_argument('--keys', action='append', default=[], help='<game>=<file>: more shader keys of a game (a JSON list)')
    ap.add_argument('--drop-unknown', action='store_true', help='leave out rows of shaders neither game has, and close')
    ap.add_argument('--apply', action='store_true', help='write the merged rows to the shared lists')
    ap.add_argument('--close', action='store_true', help='with --apply: comment on each merged issue and close it')
    args = ap.parse_args()
    if args.close and not args.apply:
        ap.error('--close needs --apply')
    keys, seeds = {game: set() for game in GAMES}, {}
    try:
        for spec in args.seed + args.keys:
            game, found = key_source(spec)
            keys[game] |= found
            if spec in args.seed:
                seeds.setdefault(game, []).append(Path(spec))
    except (OSError, ValueError, KeyError) as error:
        ap.error(str(error))
    for game, found in keys.items():
        print(f'{game}: {len(found)} shader keys' + ('' if found else ' (none given: its rows stay unmerged)'))
    issues = open_issues(args.repo)
    if args.issue:
        issues = [i for i in issues if i['number'] in args.issue]
    token = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True).stdout.strip()
    lists = {game: ShaderList.load(path) for game, (path, _, _) in GAMES.items()}
    before = {game: (len(s.variants), sum(1 for row in s.warm if row[0] in 'RB')) for game, s in lists.items()}
    merged = []
    for issue in issues:
        line, per_game = merge_issue(issue, lists, keys, seeds, token, args.drop_unknown)
        print(line)
        if per_game is not None:   # all default or nothing new: still thanked and closed
            merged.append((issue, per_game))
    for game, s in lists.items():
        print(f'{game} shared shader list: {before[game][0]} -> {len(s.variants)} variants, '
              f'{before[game][1]} -> {sum(1 for row in s.warm if row[0] in "RB")} render pipeline rows')
    if not args.apply:
        print('dry run: nothing written (--apply writes the lists)')
        return
    for game, s in lists.items():
        if (len(s.variants), sum(1 for row in s.warm if row[0] in 'RB')) != before[game]:
            s.save(GAMES[game][0])
            print(f'written: {GAMES[game][0]}')
    for issue, per_game in merged if args.close else []:
        gh('issue', 'comment', str(issue['number']), '--repo', args.repo, '--body', reply_text(per_game, titled_game(issue)))
        gh('issue', 'close', str(issue['number']), '--repo', args.repo)


if __name__ == '__main__':
    main()
