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

Every other game with a shader list keeps its own, games/<game>/assets/shader_list.tsv (OTHER; user 2026-10-28: "每个游戏
支持自己的着色器清单 收到自己游戏里 不要混了"). Its launcher titles automatic submissions "Shader list (<game>): ..." and
the shader-list-other form asks for the game; issues sent before that (titled "Shader list: ... (Wonder 1.2.1)", or
plain "Shader list:") are recognised by the data folder in the title or by their shaders' keys in that game's list. There
is no seed of those games here: an issue titled for one of them gives it every row that is not Tears of the Kingdom's or
Breath of the Wild's, and their default variants are not dropped. Rows pasted into the issue text (a fenced TSV block)
are read like an attached file.

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
SEEDED = tuple(GAMES)   # the games whose prepared seeds name every shader key (--seed, --keys)
# The other games that write a contribution file (each its runtime/game_version.h GAME_CONTRIB_*): English name, Chinese
# name, and the data folders (GAME_FOLDER) the title of an automatic submission names, "(<folder> <version>)"
OTHER = {'odyssey': ('Super Mario Odyssey', '超级马力欧 奥德赛', ('SMO', 'Odyssey')),
         'ssbu': ('Super Smash Bros. Ultimate', '任天堂明星大乱斗 特别版', ('SSBU',)),
         'wonder': ('Super Mario Bros. Wonder', '超级马力欧兄弟 惊奇', ('Wonder',)),
         'mvdk': ('Mario vs. Donkey Kong', '马力欧 vs. 大金刚', ('MvDK',))}
OTHER_LABEL = 'shader-list-other'   # .github/ISSUE_TEMPLATE/shader-list-other.yml
for _game, (_name, _zh, _) in OTHER.items():
    GAMES[_game] = (TOTK.parent / _game / 'assets/shader_list.tsv', OTHER_LABEL, f'Shader list ({_name}')
    TITLES[_game], TITLES_ZH[_game] = _name, _zh
FIELD_GAME = re.compile(r'^### Game / 游戏\s*\n+([^\n#][^\n]*)', re.M)   # the form's game field
LEGACY_TITLE = re.compile(r'\((\w+) [\d.]+\)\s*$')   # "... shaders from Mac (Wonder 1.2.1)": the launcher before its own title
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
        if fields[0] in ('R', 'B', 'T') and warm_row(fields):   # a render pipeline: contribution_pipelines
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
    """The render pipeline rows (R, B with its border recipe, T with its tile reads) of a contribution file, in order, without repeats
    (nvn_mtl.m pso_contribute; user 2026-10-22: collected with the shaders)."""
    rows = []
    for line in text.splitlines():
        fields = line.split('\t')
        row = warm_row(fields) if fields[0] in ('R', 'B', 'T') else None
        if row and row not in rows:
            rows.append(row)
    return rows


def key_source(spec):
    """(game, shader keys) from a prepared seed folder (its shader-keys.json, which names the version) or from
    "<game>=<file>" (a JSON list of keys, or a shader-keys.json). A seed without shader-keys.json (made before
    2026-10-22, its variants are the startup shaders only) cannot tell a key's game: ValueError."""
    game, _, path = spec.partition('=') if '=' in spec else ('', '', spec)
    if game and game not in SEEDED:
        raise ValueError(f'{spec}: the game is one of {", ".join(SEEDED)}')
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


def inline_text(issue):
    """The shader list rows pasted into an issue or its comments (#1160: a fenced TSV block, the submission had no way to
    attach a file), as a contribution file's text; empty when there are none."""
    texts = [issue.get('body') or ''] + [c.get('body') or '' for c in issue.get('comments') or []]
    return '\n'.join(line for text in texts for line in text.splitlines() if line[:2] in ('V\t', 'R\t', 'B\t', 'T\t'))


def list_keys(shared):
    """The shader keys a game's list names (its variants and its pipelines' shaders): the keys of a game without a seed."""
    return {key for key, _ in shared.variants} | {s for row in shared.warm if row[0] in 'RBT' for s in pipeline_shaders(row)}


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
    if list(per_game) == [titled] and titled in SEEDED:
        rows, new = per_game[titled]
        return (f'Thank you! {rows} rows, {new} of them new to the shared shader list; they come with the next build.\n\n'
                f'谢谢！共 {rows} 条，其中 {new} 条是公共清单里新增的，下次打包就会带上。')
    if list(per_game) == [titled]:   # another game: its own list, which its build may not read yet
        rows, new = per_game[titled]
        path = f'games/{titled}/assets/shader_list.tsv'
        return (f"Thank you! {rows} rows, {new} of them new to {TITLES[titled]}'s own shader list (`{path}`).\n\n"
                f'谢谢！共 {rows} 条，其中 {new} 条是{TITLES_ZH[titled]}自己的着色器清单（`{path}`）里新增的。')
    en = '; '.join(f'{TITLES[g]}: {r} rows, {n} new' for g, (r, n) in per_game.items())
    zh = '；'.join(f'{TITLES_ZH[g]} {r} 条，其中新增 {n} 条' for g, (r, n) in per_game.items())
    return (f"Thank you! {en}. The file held more than one game's shaders, so each row went to its own game's list.\n\n"
            f'谢谢！{zh}。这份文件里有不止一个游戏的着色器，每一行已按所属游戏分别收进对应清单。')


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
    for label in dict.fromkeys(label for _, label, _ in GAMES.values()):
        try:
            found.update({i['number']: i for i in json.loads(gh('issue', 'list', '--repo', repo, '--label', label, *fields))})
        except subprocess.CalledProcessError:   # a label not made yet on GitHub (shader-list-other): the titles still find them
            pass
    titled = json.loads(gh('issue', 'list', '--repo', repo, '--search', '"Shader list" in:title', *fields))
    found.update({i['number']: i for i in titled if i['title'].lower().startswith('shader list')})
    return [found[n] for n in sorted(found)]


def titled_game(issue):
    """The game an issue was sent for: by its label or title prefix, the form's game field, or the data folder an older
    automatic submission put in its title; Tears of the Kingdom otherwise (its form and plain "Shader list:" titles)."""
    labels = {label['name'] for label in issue.get('labels') or []}
    title = issue['title']
    if GAMES['botw'][1] in labels or title.startswith(GAMES['botw'][2]):
        return 'botw'
    for game, (_, _, prefix) in GAMES.items():
        if game in OTHER and title.lower().startswith(prefix.lower()):
            return game
    field = FIELD_GAME.search(issue.get('body') or '')
    legacy = LEGACY_TITLE.search(title)
    for game, (name, zh, folders) in OTHER.items():
        if field and field.group(1).strip().lower() in {name.lower(), zh, *(f.lower() for f in folders)}:
            return game
        if legacy and legacy.group(1) in folders:
            return game
        if re.search(r'\b(?:' + '|'.join(re.escape(w) for w in (name, *folders)) + r')\b', title, re.I):   # "Shader list: Wonder"
            return game
    return 'totk'


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
    pasted = inline_text(issue)
    if pasted:
        rows |= contribution_rows(pasted)[0]
        pipes += [row for row in contribution_pipelines(pasted) if row not in pipes]
    games, (unknown_rows, unknown_pipes) = route(rows, pipes, keys)
    titled = titled_game(issue)
    # No seed of the other games: in an issue titled for one, what is no seeded game's is its own; in one titled plainly
    # "Shader list:" whose known rows name exactly one other game (a file comes from one game's install, and the
    # seeds name every Tears of the Kingdom and Breath of the Wild shader), the rest is that game's too.
    others = [g for g in games if g in OTHER]
    owner = titled if titled in OTHER else others[0] if len(others) == 1 else None
    if owner and (unknown_rows or unknown_pipes):
        own = games.setdefault(owner, (set(), []))
        own[0].update(unknown_rows); own[1].extend(unknown_pipes)
        unknown_rows, unknown_pipes = set(), []
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
        if game in OTHER:   # later issues find this game's shaders by their keys (an older one titled plain "Shader list:")
            keys[game] |= list_keys(shared)
        parts.append(f'{game} {len(g_rows)} rows ({len(new)} new, {len(builtin)} default dropped), '
                     f'{len(g_pipes)} pipeline rows ({len(new_pipes)} new)')
    unknown = len(unknown_rows) + len(unknown_pipes)
    line = (f"#{issue['number']} {issue['author']['login']} ({titled}): {len(urls)} file(s)" + (', pasted rows' if pasted else '')
            + '; ' + ('; '.join(parts) or 'nothing')
            + (f'; {dropped} lines dropped' if dropped else '')
            + (f'; {unknown} rows of shaders neither game has' + (' (left out)' if drop_unknown else ' (issue stays open)')
               if unknown else '') + ''.join(f'\n  {p}' for p in problems))
    closable = (rows or pipes) and not problems and (drop_unknown or not unknown)
    return line, per_game if closable else None


def save_list(game, shared):
    """Write a game's list. Tears of the Kingdom's and Breath of the Wild's take shader_list.py's header; another game's
    keeps the header its file has (its own translation chain), or gets one naming the game when the list is new."""
    path = GAMES[game][0]
    if game in SEEDED:
        shared.save(path)
        return
    old = path.read_text().splitlines(keepends=True) if path.exists() else []
    header = ''.join(line for line in old[:next((i for i, line in enumerate(old) if not line.startswith('#')), len(old))])
    header = header or (f'# {TITLES[game]} shader list v1 (games/totk/scripts/shader_list.py; docs/shader-list.md).\n'
                        '# Identifiers and pipeline descriptors only, no game data. Merged from shader-list issues.\n')
    path.parent.mkdir(parents=True, exist_ok=True)
    shared.save(path)
    body = [line for line in path.read_text().splitlines(keepends=True) if not line.startswith('#')]
    path.write_text(header + ''.join(body))


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
        if game in SEEDED:
            print(f'{game}: {len(found)} shader keys' + ('' if found else ' (none given: its rows stay unmerged)'))
    issues = open_issues(args.repo)
    if args.issue:
        issues = [i for i in issues if i['number'] in args.issue]
    issues.sort(key=lambda i: (titled_game(i) not in OTHER, i['number']))   # other games' first: they teach their keys
    token = subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True).stdout.strip()
    lists = {game: ShaderList.load(path) for game, (path, _, _) in GAMES.items()}
    for game in OTHER:
        keys[game] = list_keys(lists[game])
    before = {game: (len(s.variants), sum(1 for row in s.warm if row[0] in 'RBT')) for game, s in lists.items()}
    merged = []
    for issue in issues:
        line, per_game = merge_issue(issue, lists, keys, seeds, token, args.drop_unknown)
        print(line)
        if per_game is not None:   # all default or nothing new: still thanked and closed
            merged.append((issue, per_game))
    for game, s in lists.items():
        print(f'{game} shared shader list: {before[game][0]} -> {len(s.variants)} variants, '
              f'{before[game][1]} -> {sum(1 for row in s.warm if row[0] in "RBT")} render pipeline rows')
    if not args.apply:
        print('dry run: nothing written (--apply writes the lists)')
        return
    for game, s in lists.items():
        if (len(s.variants), sum(1 for row in s.warm if row[0] in 'RBT')) != before[game]:
            save_list(game, s)
            print(f'written: {GAMES[game][0]}')
    for issue, per_game in merged if args.close else []:
        gh('issue', 'comment', str(issue['number']), '--repo', args.repo, '--body', reply_text(per_game, titled_game(issue)))
        gh('issue', 'close', str(issue['number']), '--repo', args.repo)


if __name__ == '__main__':
    main()
