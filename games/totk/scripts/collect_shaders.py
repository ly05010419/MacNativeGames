#!/usr/bin/env python3
"""Merge the shaders and render pipelines players' apps contributed into each game's shader list
(games/<game>/assets/shader_list.tsv).

A player's game writes the shaders and pipelines its list lacks to a contribution file
(third_party/d2r_gfx/gfx/nvn/totk_shader_contrib.h), and the app sends that file's rows over HTTPS to the shader inbox
(totk_contrib_upload.h; MacNativeGamesWeb/shader-inbox, a project of its own: a Vercel function and a database), which
keeps each row once per game with the installs and game versions that sent it. This is the only way contributions come
in (user 2026-11-12: the GitHub issues players used to attach the file to are no longer read). --inbox <url> merges the
rows the inbox holds; --mark-inbox <ref> (with --apply, after the lists are committed) tells the inbox those rows are done.
The inbox's admin token is read from SHADER_INBOX_TOKEN.

Which list a row goes to is decided by what it names, never by who sent it (user 2026-11-12: "根治"). A shader key is the
SHA-256 of a shader's control section and code (the runtime's), so the keys a game's files hold say which game a shader is
of. Each game's keys come from its prepared seeds (--seed <seed folder>, their shader-keys.json) and, for a game whose seeds
do not name every shader it plays, from what its list already names. The game the app named is only the default:
  1. a row whose every shader that game holds is that game's;
  2. else a row exactly one other game holds every shader of is that game's (a file can hold another game's rows);
  3. else a row several other games hold is left out: no one can tell whose it is;
  4. else (no game holds it) it is the named game's, a shader new to its list, unless the named game's seeds name every
     shader the game has (COMPLETE): a row outside them is not that game's, whoever sent it, and stays in the inbox.
2026-11-11: an Ocarina of Time install sent 418 Tears of the Kingdom pipelines (414 of them byte for byte in Tears of the
Kingdom's list), and the merge, trusting the app's game, put them in Ocarina of Time's list. A COMPLETE game's list is
checked whole as well: rows outside its seeds leave it with --apply (to the game that holds them, when one does).

Rows the built-in variants of a seed of the same game serve are dropped (the list keeps only non-default variants; user
2026-10-22). Every game keeps its own list (user 2026-10-28: "每个游戏支持自己的着色器清单 收到自己游戏里 不要混了");
games with their own shader_list.py (OWN_SCRIPT) merge through its "merge", which keeps rows this one does not know (mesh
pipelines) and each pipeline's bind state.

usage: collect_shaders.py --inbox <url> --seed <seed folder> ... [--keys <game>=<file> ...] [--apply] [--mark-inbox <ref>]"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOTK / 'scripts'))
from shader_list import KEY, NAME, ShaderList, answers_suffix, canonical, listed_answers, warm_row  # noqa: E402
from prepare_shader_seed import builtin_serves  # noqa: E402
from ns2_shader_corpus import KEY_RULE, KEY_RULE_HEADER, list_key_rule  # noqa: E402

TITLES = {'totk': 'Tears of the Kingdom', 'botw': 'Breath of the Wild', 'odyssey': 'Super Mario Odyssey',
          'ssbu': 'Super Smash Bros. Ultimate', 'wonder': 'Super Mario Bros. Wonder', 'mvdk': 'Mario vs. Donkey Kong',
          'dkb': 'Donkey Kong Bananza', 'pokopia': 'Pokémon Pokopia', 'oot': 'Ocarina of Time'}
# Each game's list, by the game's id in the inbox (its lib/validate.js GAMES and the apps' GAME_INBOX_ID)
GAMES = {game: TOTK.parent / game / 'assets/shader_list.tsv' for game in TITLES}
GAMES['totk'] = TOTK / 'assets/shader_list.tsv'
# Games whose prepare_shader_seed.py builds a seed this script can drop default variants with (builtin_serves)
SEEDED = ('totk', 'botw')
# Games whose seeds name every shader key of their files (shader_key_scope over the whole RomFS): a row outside them is not
# theirs. 2026-11-12: all 5,784 rows Ocarina of Time installs sent in a round were inside its seed. Not Tears of the Kingdom
# or Breath of the Wild: 4,851 of 14,554 rows from 1.4.3 and 756 of 1,028 from 1.9.0 named shaders outside their seeds,
# of no other game either, so their seeds miss shaders the games play.
# Switch 2 games (prepare_ns2_seed.py): their shader-keys.json names only the shaders the seed translated; their
# game-shader-keys.json names every shader the corpus finds in the game's files (user 2026-11-12: "做a"). That makes
# Bananza COMPLETE: all 213 shader keys its installs sent were among its 22,854. Not Pokopia yet: of the 333 keys its
# installs sent (2026-11-11) only 24 were among the 131,090 its corpus finds, in no other game's either, and its game
# files are pinned (expected-v2.0.1.json), so the corpus misses shaders the game uses; its keys still name its rows.
NS2_VERSIONS = {'dkb': 'dkb', 'pkp': 'pokopia'}
COMPLETE = ('oot', 'dkb')
IDENTITY = 'game-shader-keys.json'
SEED_GAMES = SEEDED + COMPLETE   # the games --seed and --keys can name
# Games whose own shader_list.py merges a file into their list (its "merge"): Ocarina of Time's list has rows this script does
# not know (M, mesh pipelines) and keeps the bind state (A row) of every pipeline it names, which only a whole file gives.
OWN_SCRIPT = {'oot': TOTK.parent / 'oot/scripts/shader_list.py'}
_MODULES = {}


def own_module(game):
    """A game's own shader_list.py (OWN_SCRIPT), loaded once."""
    if game not in _MODULES:
        spec = importlib.util.spec_from_file_location(f'{game}_shader_list', OWN_SCRIPT[game])
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _MODULES[game] = module
    return _MODULES[game]


def load_list(game, path):
    """A game's list, read by its own shader_list.py when it has one (OWN_SCRIPT), else by this one's."""
    return own_module(game).ShaderList.load(path) if game in OWN_SCRIPT else ShaderList.load(path)


def contribution_items(text):
    """([(line, item)], lines dropped) from a contribution's text: item ('V', (key, canonical deps)) for a shader and its
    bind state, ('P', warm row) for a render pipeline (R, B with its border recipe, T with its tile reads; nvn_mtl.m
    pso_contribute); each item once, with the first line that gave it. Comments and blank lines are not counted."""
    items, seen, dropped = [], set(), 0
    for line in text.splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        fields = line.split('\t')
        item = None
        if fields[0] in ('R', 'B', 'T'):
            row = warm_row(fields)
            item = ('P', row) if row else None
        elif len(fields) == 3 and fields[0] == 'V' and KEY.match(fields[1]):
            try:
                deps = canonical(json.loads(fields[2]))   # None: not a bind state the translator takes
            except ValueError:
                deps = None
            item = ('V', (fields[1], deps)) if deps is not None else None
        if item is None:
            dropped += 1
        elif item not in seen:
            seen.add(item)
            items.append((line, item))
    return items, dropped


def key_source(spec):
    """(game, shader keys, complete) from a prepared seed folder or from "<game>=<file>" (a JSON list of keys, or a
    shader-keys.json). complete: the keys are every shader the game ships. A seed folder's game-shader-keys.json (a Switch 2
    game's, prepare_ns2_seed.py) is that, checked against its shader-keys.json (the same version and game files); a
    shader-keys.json is that for the games whose prepare_shader_seed.py scans the whole RomFS, not for a Switch 2 game's,
    which names only what its seed translated. A seed without shader-keys.json (made before 2026-10-22, its variants are
    the startup shaders only) cannot tell a key's game: ValueError."""
    game, _, path = spec.partition('=') if '=' in spec else ('', '', spec)
    if game and game not in SEED_GAMES:
        raise ValueError(f'{spec}: the game is one of {", ".join(SEED_GAMES)}')
    p, identity = Path(path), None
    if p.is_dir():
        if (p / IDENTITY).is_file():
            identity = json.loads((p / IDENTITY).read_text())
            rule = json.loads((p / 'seed-manifest.json').read_text()).get('shader_key_rule')
            if rule != KEY_RULE:   # its keys and the lists' would name no shader in common
                raise ValueError(f'{path}: a seed of shader key rule {rule or "(none recorded)"}, the lists use {KEY_RULE}: '
                                 'make the seed again (update_runtime.py --seed)')
        p = p / 'shader-keys.json'
        if not p.is_file():
            raise ValueError(f'{path}: no shader-keys.json (a seed made before 2026-10-22); prepare the seed again')
    data = json.loads(p.read_text())
    if identity is not None:
        same = ('version', 'source_romfs_sha256')
        if not isinstance(data, dict) or any(identity.get(k) != data.get(k) for k in same):
            raise ValueError(f'{path}: {IDENTITY} is of other game files than its shader-keys.json')
        data = identity
    keys = data['keys'] if isinstance(data, dict) else data
    version = data.get('version', '') if isinstance(data, dict) else ''
    if not game:
        game = ('botw' if version.startswith('botw') else 'oot' if version == 'oot' else NS2_VERSIONS.get(version)
                or ('totk' if version.startswith('v') else ''))
        if not game:
            raise ValueError(f'{path}: the version does not say which game; give it as <game>=<file>')
    if not all(isinstance(k, str) and KEY.match(k) for k in keys):
        raise ValueError(f'{path}: not a list of shader keys')
    return game, set(keys), identity is not None or game not in NS2_VERSIONS.values()


def pipeline_shaders(row):
    """The shader keys a render pipeline row names: its key leads with the vertex and fragment variants, each a
    NUL-padded "<key>-<suffix>_b" of NAME bytes (shader_list.py)."""
    b = bytes.fromhex(row[1])
    names = [b[i:i + NAME].split(b'\0', 1)[0].decode('ascii', 'replace') for i in (0, NAME)]
    return [n[:64] for n in names if n]


def item_shaders(item):
    """The shader keys a contribution item names (none for a pipeline whose key names no variant)."""
    kind, value = item
    return [value[0]] if kind == 'V' else pipeline_shaders(value)


def row_shaders(game, row):
    """The shader keys a warm row of a game's list names: every kind its own shader_list.py knows (S, C, M too)."""
    if game in OWN_SCRIPT:
        return [n[:64] for n in own_module(game).row_variants(row)]
    return pipeline_shaders(row) if row[0] in ('R', 'B', 'T') else []


def list_keys(game, shared):
    """The shader keys a game's list names (its variants and its pipelines' shaders)."""
    return {key for key, _ in shared.variants} | {s for row in shared.warm for s in row_shaders(game, row)}


def owner(shaders, named, keys):
    """The game a row of these shaders goes to when the app named `named` (see the module's text): (game, why) with why
    'named', 'other', or game None with why 'shared' (several other games hold it) or 'outside' (no game holds it and the
    named game's seeds name all its shaders)."""
    if not shaders:
        return (None, 'outside') if named in COMPLETE else (named, 'named')
    holders = [g for g, k in keys.items() if all(s in k for s in shaders)]
    if named in holders:
        return named, 'named'
    if len(holders) == 1:
        return holders[0], 'other'
    if holders:
        return None, 'shared'
    return (None, 'outside') if named in COMPLETE else (named, 'named')


def merge_own_texts(texts, game, shared):
    """Texts for a game with its own merge (OWN_SCRIPT), each through the game's shader_list.py as its "merge" command:
    (rows, new rows, warm rows naming bind states no file gives)."""
    size = lambda: len(shared.variants) + len(shared.warm) + len(shared.locators)
    rows = new = refused = 0
    for text in texts:
        before, refused_before = size(), shared.refused
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            f.write(text)
        try:
            shared.add_rows(f.name, 'm')
        finally:
            os.unlink(f.name)
        rows += sum(1 for line in text.splitlines() if line and not line.startswith('#'))
        new += size() - before
        refused += shared.refused - refused_before
    return rows, new, refused


def merge_items(game, items, lists, seeds):
    """Merge one game's items into its list: its report part."""
    shared = lists[game]
    if game in OWN_SCRIPT:
        rows, new, refused = merge_own_texts(['\n'.join(line for line, _ in items) + '\n'], game, shared)
        return (f'{game} {rows} rows ({new} new'
                + (f', {refused} warm rows naming bind states no file gives' if refused else '') + ')')
    variants = {value for (_, (kind, value)) in items if kind == 'V'}
    pipes = [value for (_, (kind, value)) in items if kind == 'P']
    builtin = {r for r in variants if any(builtin_serves(s, *r) for s in seeds.get(game, []))}
    new = (variants - builtin) - shared.variants
    for key, deps in variants - builtin:
        shared.add_variant(key, deps)
    for _, deps in builtin:   # the bind states of the variants a seed serves (no V row): the list keeps them as A rows
        answers = listed_answers(deps)
        shared.answers[answers_suffix(answers)] = answers
    # Every game's list keeps the bind state of every variant its pipelines name (shader_list.py A rows): a row naming
    # one no file gave stops packaging in resolve (2026-11-11, #2020: two such rows in Pokopia's list).
    refused = [row for row in pipes if not shared.knows(row)]
    new_pipes = [row for row in pipes if row not in shared.warm and row not in refused]
    for row in new_pipes:
        shared.add_warm(row, 'm')
    return (f'{game} {len(variants)} rows ({len(new)} new, {len(builtin)} default dropped), '
            f'{len(pipes)} pipeline rows ({len(new_pipes)} new'
            + (f', {len(refused)} naming bind states the file does not give: an older game version wrote it' if refused else '')
            + ')')


def merge_text(label, text, named, lists, keys, seeds):
    """Merge one contribution text the app of game `named` sent: (report line, set of its lines left where they are)."""
    items, dropped = contribution_items(text)
    to, left, counts = {}, set(), {'other': 0, 'shared': 0, 'outside': 0}
    for line, item in items:
        game, why = owner(item_shaders(item), named, keys)
        if why in counts:
            counts[why] += 1
        if game is None:
            if why == 'outside':
                left.add(line)
            continue
        to.setdefault(game, []).append((line, item))
    parts = [merge_items(game, game_items, lists, seeds) for game, game_items in to.items()]
    for game in to:
        if game not in COMPLETE:   # what a game without a full seed merged names its shaders for later rows
            keys[game] |= list_keys(game, lists[game])
    line = (label + '; ' + ('; '.join(parts) or 'nothing')
            + (f'; {counts["other"]} rows of another game\'s shaders went to it' if counts['other'] else '')
            + (f'; {counts["shared"]} rows several games share (left out)' if counts['shared'] else '')
            + (f'; {counts["outside"]} rows of shaders no game here has and outside {named}\'s full seed (left in the inbox)'
               if counts['outside'] else '')
            + (f'; {dropped} lines dropped' if dropped else ''))
    return line, left


def foreign_rows(game, shared, seed_keys):
    """The rows of a COMPLETE game's list naming a shader outside its seeds: (variants, warm rows)."""
    variants = {v for v in shared.variants if v[0] not in seed_keys}
    warm = [row for row in shared.warm if any(s not in seed_keys for s in row_shaders(game, row))]
    return variants, warm


def drop_foreign(game, lists, keys, seed_keys):
    """Take a COMPLETE game's rows outside its seeds out of its list; a pipeline exactly one other game holds every
    shader of goes to that game's list when that list knows its bind states. Returns the report line, or '' when clean."""
    shared = lists[game]
    variants, warm = foreign_rows(game, shared, seed_keys)
    if not variants and not warm:
        return ''
    moved = 0
    for row in warm:
        del shared.warm[row]
        holders = [g for g, k in keys.items() if g != game and all(s in k for s in row_shaders(game, row))]
        other = lists[holders[0]] if len(holders) == 1 else None
        if other is not None and row[0] in ('R', 'B', 'T') and other.knows(row):
            moved += row not in other.warm
            other.add_warm(row, 'm')
    shared.variants -= variants
    return (f'{game}: {len(variants)} variants and {len(warm)} pipeline rows of its list named shaders outside its full seed: '
            f'taken out ({moved} new to the game that holds them)')


def curl_json(url, token, payload=None):
    """One HTTPS call with the system curl (python.org's Python has no CA certificates until its Install Certificates runs):
    the parsed JSON answer. The token goes in a header file only this user can read, never on the command line."""
    with tempfile.TemporaryDirectory() as tmp:
        header = Path(tmp) / 'h'
        fd = os.open(header, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write(f'Authorization: Bearer {token}\nContent-Type: application/json\n')
        cmd = ['/usr/bin/curl', '-sS', '--max-time', '120', '-H', f'@{header}', '-w', '\n%{http_code}']
        if payload is not None:
            body = Path(tmp) / 'b'
            body.write_text(json.dumps(payload))
            cmd += ['-X', 'POST', '--data-binary', f'@{body}']
        run = subprocess.run(cmd + [url], capture_output=True, text=True)
    if run.returncode:
        raise ValueError(f'{url}: curl failed ({run.returncode}): {run.stderr.strip()[:200]}')
    text, _, code = run.stdout.rpartition('\n')
    try:
        answer = json.loads(text)
    except ValueError:
        raise ValueError(f'{url}: HTTP {code}, not JSON') from None
    if not code.startswith('2') or not answer.get('success'):
        raise ValueError(f'{url}: HTTP {code} {answer.get("error")}')
    return answer['data']


INBOX_PAGE = 5000   # rows per export request (the inbox's lib/handlers.js allows up to 20,000)


def inbox_call(base, token, path, payload=None):
    """One call to the shader inbox's admin API: its data, or ValueError with the server's reason."""
    return curl_json(f"{base.rstrip('/')}{path}", token, payload)


def inbox_pending(base, token, game):
    """Every row of a game the inbox holds and no merge has taken yet: [{id, kind, body, players, versions, platforms}]."""
    rows, after = [], 0
    while True:
        data = inbox_call(base, token, f'/v1/export?game={game}&after={after}&limit={INBOX_PAGE}')
        rows += data['rows']
        if not data['next']:
            return rows
        after = data['next']


NVN_CTL = TOTK / 'third_party/d2r_gfx/gfx/nvn/nvn_ctl.h'
_BUILD_RULE = {}


def key_rule_commit():
    """The commit that made KEY_RULE the runtime's Switch 2 shader key rule (nvn_ctl.h NVN2_KEY_RULE), or None while it is
    not committed."""
    out = subprocess.run(['git', '-C', str(TOTK), 'log', '--format=%H', '-S', f'#define NVN2_KEY_RULE "{KEY_RULE}"', '--',
                          str(NVN_CTL)], capture_output=True, text=True)
    commits = out.stdout.split() if out.returncode == 0 else []
    return commits[-1] if commits else None


def build_rule(build, since):
    """Whether an app made from source commit `build` (its MNGSourceCommit, the inbox's app_build) names Switch 2 shaders
    by KEY_RULE: 'yes' (`since` is among its commits), 'no' (a commit of this repository without it) or 'unknown' (not a
    commit here: a fork, a build outside git, or the rule not committed yet)."""
    if (build, since) not in _BUILD_RULE:
        found = 128
        if since and re.fullmatch(r'[0-9a-f]{7,40}', build or ''):
            found = subprocess.run(['git', '-C', str(TOTK), 'merge-base', '--is-ancestor', since, build],
                                   capture_output=True).returncode
        _BUILD_RULE[build, since] = {0: 'yes', 1: 'no'}.get(found, 'unknown')
    return _BUILD_RULE[build, since]


def split_by_key_rule(pending, since):
    """A Switch 2 game's inbox rows as (taken, old, unknown): sent by an app of the current key rule, only by apps of an
    older one (their keys name no shader of the lists and seeds: marked done unmerged, never merged), or by apps this
    repository cannot place (left in the inbox). An export without builds (an inbox before 2026-11-12) places none."""
    taken, old, unknown = [], [], []
    for row in pending:
        rules = {build_rule(b, since) for b in row.get('builds') or []}
        (taken if 'yes' in rules else old if rules == {'no'} else unknown).append(row)
    return taken, old, unknown


def merge_inbox(base, token, lists, keys, seeds):
    """The inbox's rows of every game here: [(report line, game, ids of the rows taken)]. A row left where it is (outside a
    full seed and no game's) is not taken, so it stays in the inbox; a game whose export failed takes none."""
    done = []
    # games without seeds first: what they merge names their shaders for the rows the other games' apps sent
    for game in sorted(GAMES, key=lambda g: (g in SEED_GAMES, g)):
        try:
            pending = inbox_pending(base, token, game)
        except (OSError, ValueError) as error:
            done.append((f'inbox {game}: {error}', game, []))
            continue
        old, unknown = [], []
        if game in NS2_VERSIONS.values():   # rows of apps with other shader keys (nvn_ctl.h NVN2_KEY_RULE) never merge
            pending, old, unknown = split_by_key_rule(pending, key_rule_commit())
        if not pending and not old and not unknown:
            continue
        players = max((r['players'] for r in pending), default=0)
        label = (f"inbox {game}: {len(pending)} rows (most-sent row from {players} installs)"
                 + (f', {len(old)} only from apps of an older shader key rule (marked done, not merged)' if old else '')
                 + (f', {len(unknown)} from app builds not in this repository (left in the inbox)' if unknown else ''))
        line, left = merge_text(label, ''.join(r['body'] + '\n' for r in pending), game, lists, keys, seeds)
        done.append((line, game, [r['id'] for r in pending if r['body'] not in left] + [r['id'] for r in old]))
    return done


def save_list(game, shared):
    """Write a game's list. Tears of the Kingdom's and Breath of the Wild's take shader_list.py's header; another game's
    keeps the header its file has (its own translation chain), or gets one naming the game when the list is new."""
    path = GAMES[game]
    if game in SEEDED:
        shared.save(path)
        return
    old = path.read_text().splitlines(keepends=True) if path.exists() else []
    header = ''.join(line for line in old[:next((i for i, line in enumerate(old) if not line.startswith('#')), len(old))])
    header = header or (f'# {TITLES[game]} shader list v1 (games/totk/scripts/shader_list.py; docs/shader-list.md).\n'
                        '# Identifiers and pipeline descriptors only, no game data. Merged from the shader inbox.\n'
                        + (f'{KEY_RULE_HEADER}{KEY_RULE}\n' if game in NS2_VERSIONS.values() else ''))
    path.parent.mkdir(parents=True, exist_ok=True)
    shared.save(path)
    body = [line for line in path.read_text().splitlines(keepends=True) if not line.startswith('#')]
    path.write_text(header + ''.join(body))


def counts(shared):
    """(variants, render pipeline rows) of a list."""
    return len(shared.variants), sum(1 for row in shared.warm if row[0] in 'RBT')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--inbox', default=os.environ.get('SHADER_INBOX_URL'),
                    help='the shader inbox (https://...; default $SHADER_INBOX_URL)')
    ap.add_argument('--seed', action='append', default=[], help='a prepared seed (build/shader-seed/<version>): its '
                    'shader keys, and its built-in variants drop default rows; one for each game version collected')
    ap.add_argument('--keys', action='append', default=[], help='<game>=<file>: more shader keys of a game (a JSON list)')
    ap.add_argument('--apply', action='store_true', help='write the merged rows to the lists')
    ap.add_argument('--mark-inbox', metavar='REF', help='with --apply: tell the inbox the merged rows are done, naming the '
                    'merge (the commit of the lists)')
    args = ap.parse_args()
    token = os.environ.get('SHADER_INBOX_TOKEN', '')
    if not args.inbox or not token:
        ap.error('needs --inbox (or SHADER_INBOX_URL) and SHADER_INBOX_TOKEN (the inbox\'s INBOX_ADMIN_TOKEN)')
    if args.mark_inbox and not args.apply:
        ap.error('--mark-inbox needs --apply')
    keys, seeds, complete = {game: set() for game in GAMES}, {}, {game: set() for game in COMPLETE}
    try:
        for spec in args.seed + args.keys:
            game, found, whole = key_source(spec)
            if game in COMPLETE and not whole:   # a Switch 2 seed packaged before it named every shader
                ap.error(f'{spec}: names only the shaders its seed translated, not every shader of the game '
                         f'(no {IDENTITY}): package {game} again')
            keys[game] |= found
            if game in COMPLETE:
                complete[game] |= found
            if spec in args.seed and game in SEEDED:
                seeds.setdefault(game, []).append(Path(spec))
    except (OSError, ValueError, KeyError) as error:
        ap.error(str(error))
    missing = [g for g in COMPLETE if not complete[g]]
    if missing:   # without its seed the game's rows cannot be told from another game's
        ap.error(f'no seed of {", ".join(missing)} (--seed): its full seed decides which rows are its own')
    for game in NS2_VERSIONS.values():   # a list of another key rule would lose every row to the full seed's check
        if GAMES[game].exists() and list_key_rule(GAMES[game]) != KEY_RULE:
            ap.error(f'{GAMES[game]}: names its shaders by key rule {list_key_rule(GAMES[game]) or "(none said)"}, not '
                     f'{KEY_RULE}: move it with ns2_rekey.py --apply first')
    lists = {game: load_list(game, path) for game, path in GAMES.items()}
    for game in GAMES:   # a full seed is all a COMPLETE game holds; the others hold what their lists name too
        if game not in COMPLETE:
            keys[game] |= list_keys(game, lists[game])
    for game in SEED_GAMES:
        print(f'{game}: {len(keys[game])} shader keys')
    before = {game: counts(s) for game, s in lists.items()}
    state = lambda s: (len(s.variants), len(s.warm), len(s.answers), len(getattr(s, 'locators', ())))
    unchanged = {game: state(s) for game, s in lists.items()}
    for game in COMPLETE:
        line = drop_foreign(game, lists, keys, complete[game])
        if line:
            print(line)
    inbox = merge_inbox(args.inbox, token, lists, keys, seeds)
    for line, _, _ in inbox:
        print(line)
    for game, s in lists.items():   # a later source may bring the bind state an earlier one's warm rows name (OoT's own merge)
        took = s.retry_refused() if hasattr(s, 'retry_refused') else 0
        if took:
            print(f'{game}: {took} refused warm rows went in once every source was merged')
    for game in COMPLETE:   # nothing outside the full seed may stay (a bug above shows here, not in a build)
        variants, warm = foreign_rows(game, lists[game], complete[game])
        if variants or warm:
            sys.exit(f'{game}: {len(variants)} variants and {len(warm)} pipeline rows outside its full seed after the merge; '
                     'nothing written')
    for game, s in lists.items():
        print(f'{game} shader list: {before[game][0]} -> {counts(s)[0]} variants, '
              f'{before[game][1]} -> {counts(s)[1]} render pipeline rows')
    if not args.apply:
        print('dry run: nothing written (--apply writes the lists)')
        return
    for game, s in lists.items():
        if state(s) != unchanged[game]:
            save_list(game, s)
            print(f'written: {GAMES[game]}')
    for _, game, ids in inbox if args.mark_inbox else []:
        for start in range(0, len(ids), 50000):
            data = inbox_call(args.inbox, token, '/v1/mark',
                              {'game': game, 'ids': ids[start:start + 50000], 'ref': args.mark_inbox})
            print(f"inbox {game}: {data['marked']} rows marked done ({args.mark_inbox})")


if __name__ == '__main__':
    main()
