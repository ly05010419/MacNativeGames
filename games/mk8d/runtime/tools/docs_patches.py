#!/usr/bin/env python3
# Writes docs/agents/game-patches.md, the index of game-specific code: per game, the rewriter's patches of its builds (CUTS, OBS, FRAME_STEP sites, MATCH_CUTS and CAPABILITIES of its
# dm/games/<id>/patches.py, with each patch's reason), the host's rows keyed by its build IDs (32-hex string literals in dm/host and dm/gfx), and the GENERALIZE note of its patches.py (whether
# the patch could become a general rule, and how); then the SDK cuts of dm/tools/sdk_patches.py by sdk build and the games that ship it, the SDK-wide function-matched rules, translator rules
# keyed by a shader's code hash, and the profile keys that switch host behaviour away from their default, with the games that set them. Frame-step sites are found by running each
# FRAME_STEP function against a recording context (no game files). Run it after changing a patch file, a build-keyed host table or a profile key; --check only reports whether the page is
# current (dm/tools/tests/test_docs.py runs it).
# Usage: docs_patches.py [--check]
import importlib.util, re, subprocess, sys, pathlib, types

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/games'))
import game   # noqa: E402

OUT = REPO / 'docs' / 'agents' / 'game-patches.md'
SDK = REPO / 'dm/tools/sdk_patches.py'
HOST = ['dm/host', 'dm/gfx', 'dm/ios']   # tracked files only: dm/host/gen and other build output are git-ignored
HOST_EXT = ('.c', '.m', '.mm', '.h', '.swift')
# What a build-keyed host table does (the table's name in the source; a row of another table is listed under its own name)
TABLES = {'k_locks': 'engine frame limit set to fps_target', 'k_ue': "Unreal's allocator on a host malloc zone", 'k_nier_dr': 'frame interval and dynamic-resolution floor'}
# Profile keys that are not behaviour switches: per-game data or controls, not something the host does differently
NOT_SWITCHES = {'pad_layout', 'depth_mode', 'mem_used', 'sys_resource'}


def rel(p): return p.relative_to(REPO).as_posix()
def link(p, line=None): return f'[{p.name}{f":{line}" if line else ""}](../../{rel(p)}{f"#L{line}" if line else ""})'
def short(bid): return f'`{bid[:8]}…`'
def lineno(text, needle): i = text.find(needle); return text.count('\n', 0, i) + 1 if i >= 0 else None


def load(path, n):
    spec = importlib.util.spec_from_file_location(f'patches_doc_{n}', path)
    if spec is None or spec.loader is None: raise ImportError(f'cannot load {path}')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def games():
    return [game.Game(d) for d in sorted((REPO / 'dm/games').iterdir()) if (d / 'game.tsv').is_file()]


def build_rows(gs):   # {build ID: [(game, display version, module)]}
    out = {}
    for g in gs:
        for b in g.builds().values():
            for mod, bid in b.items():
                if mod != 'display': out.setdefault(bid, []).append((g, b['display'], mod))
    return out


def frame_step_sites(fn):   # (offset, reason) of each site a FRAME_STEP function writes, and whether it marks a render-scale stub
    pos, puts = [0x8000000], []
    def emit(w): pos[0] += 4
    ctx = types.SimpleNamespace(here=lambda: pos[0], emit=emit, site=lambda off, orig: None, put=lambda off, words, why: puts.append((off, why)), state=0x9000000, render_scale=0)
    fn(ctx)
    return sorted(puts), ctx.render_scale


def host_rows(builds):   # {build ID: [(file, line, what)]}: rows and checks of the host keyed by a main build ID
    out = {}
    tracked = subprocess.run(['git', 'ls-files', *HOST], cwd=REPO, capture_output=True, text=True, check=True).stdout.split('\n')
    for p in sorted(REPO / f for f in tracked if f):
        if p.suffix not in HOST_EXT or 'tests' in p.relative_to(REPO).parts[2:]: continue
        table = None
        for n, line in enumerate(p.read_text(errors='replace').split('\n'), 1):
            m = re.search(r'\b(k_\w+)\s*\[\]\s*=', line)
            if m: table = m.group(1)
            if line.lstrip().startswith('}'): table = None
            for m in re.finditer(r'"([0-9a-f]{32})"', line):
                strings = re.findall(r'"([^"]*)"', line)
                what = TABLES.get(table, table) if table else 'build check'
                if table == 'k_locks' and len(strings) > 1: what = f'{what}: {strings[1]}'
                out.setdefault(m.group(1), []).append((p, n, what if m.group(1) in builds else f'{what} (a build no profile registers)'))
    return out


def shader_rules():   # (file, line, comment) of translator rules keyed by a shader's code hash (64-hex literals in the C# tools)
    out = []
    for p in sorted((REPO / 'dm/gfx/tools').rglob('*.cs')):
        lines = p.read_text(errors='replace').split('\n')
        for n, line in enumerate(lines, 1):
            if not re.search(r'"[0-9A-Fa-f]{64}"', line): continue
            k = n - 1
            while k > 0 and not lines[k - 1].lstrip().startswith('//'): k -= 1
            j = k
            while j > 1 and lines[j - 2].lstrip().startswith('//'): j -= 1
            comment = ' '.join(l.strip().lstrip('/').strip() for l in lines[j - 1:k])
            out.append((p, n, re.split(r'(?<=[^.]\.) |: ', comment)[0] if comment else ''))
    return out


def switches(gs):   # [(key, value, [games])] of every profile key set away from its default in game.py
    src = (REPO / 'dm/games/game.py').read_text(); src = src[src.find('def header('):]; src = re.split(r'\n(?=\S)', src)[0]   # the keys the host's header takes (to the next top-level line)
    defaults = {}
    for k, d in re.findall(r'g\.(?:get|words)\(\s*[\'"](\w+)[\'"]\s*,\s*[\'"]([^\'"]*)[\'"]\s*\)', src): defaults.setdefault(k, d)
    out = {}
    for g in gs:
        for k, d in defaults.items():
            if k in NOT_SWITCHES or not g.values(k): continue
            v = ' '.join(g.values(k)[0])
            if v == d: continue
            shown = '(shader keys)' if k == 'feedback_snap' else v
            out.setdefault((k, shown), []).append(g)
    return sorted(((k, v, gl) for (k, v), gl in out.items()), key=lambda r: (len(r[2]), r[0], r[1]))


DUP = set()
def name(g): return g.get('name', g.id) + (f' ({g.id})' if g.id in DUP else '')
def note(g): return f'[{name(g)}](../../dm/games/{g.id}/README.md)'


def version(builds, bid, owner=None):   # "1.4.3 main" from the build rows (of the owning profile first)
    rows = [r for r in builds.get(bid, []) if owner is None or r[0].id == owner.id] or builds.get(bid, [])
    return f'{rows[0][1]} `{rows[0][2]}`' if rows else 'unregistered build'


def page():
    gs = games(); names = [g.get('name', g.id) for g in gs]; DUP.update(g.id for g in gs if names.count(g.get('name', g.id)) > 1)
    gs.sort(key=lambda g: name(g).casefold()); builds = build_rows(gs); host = host_rows(builds)
    files = {p.parent.name: p for p in sorted((REPO / 'dm/games').glob('*/patches.py'))}
    mods = {gid: load(p, n) for n, (gid, p) in enumerate(files.items())}
    sdk = load(SDK, 'sdk'); sdk_text = SDK.read_text()
    host_games = {}
    for bid, rows in host.items():
        for g, _, _ in builds.get(bid, []): host_games.setdefault(g.id, set()).add(bid)

    summary, sections = [], []
    for g in gs:
        m = mods.get(g.id)
        if not m and g.id not in host_games: continue
        text = files[g.id].read_text() if m else ''
        cuts, obs, fs, mc, caps = (getattr(m, k, {}) for k in ('CUTS', 'OBS', 'FRAME_STEP', 'MATCH_CUTS', 'CAPABILITIES'))
        bids = list(dict.fromkeys([*cuts, *obs, *fs, *caps, *sorted(host_games.get(g.id, ()))]))
        n_fs = 0; lines = [f'### {name(g)}', '', f'{note(g)}, `{g.id}`' + (f', {link(files[g.id])}' if m else '') + '.', '']
        general = getattr(m, 'GENERALIZE', '') if m else ''
        if general: lines += [f'**Generalize?** {general}', '']
        for bid in bids:
            lines.append(f'- **{version(builds, bid, g)}** {short(bid)}' + (f' ({link(files[g.id], lineno(text, repr(bid)))})' if bid in text else ''))
            for off, ob, nb, why in cuts.get(bid, []): lines.append(f'  - cut `+{off:#x}`: {why}')
            for off, gate, word, why in obs.get(bid, []): lines.append(f'  - gate {gate} at `+{off:#x}`: {why}')
            if bid in fs:
                sites, scale = frame_step_sites(fs[bid]); n_fs += len(sites)
                fold = len(sites) > 12   # a long list (ASTRAL CHAIN's) folds
                if fold: lines += [f'  <details><summary>{len(sites)} frame-step sites</summary>', '']
                for off, why in sites: lines.append(f'  - frame step `+{off:#x}`: {why}')
                if fold: lines += ['', '  </details>', '']
                if scale: lines.append('  - render scale stub (the launcher offers DM_RENDER_SCALE)')
            if caps.get(bid): lines.append(f'  - launcher capabilities: {", ".join(caps[bid])}')
            for p, n, what in host.get(bid, []): lines.append(f'  - host {link(p, n)}: {what}')
        for module, rules in mc.items():
            for r in rules:
                fns = r.get('functions', [r]); ncut = sum(len(f['cuts']) for f in fns)
                lines.append(f'- **function-matched** (`{module}` of builds without CUTS): {r["name"]}: {len(fns)} function{"s" * (len(fns) > 1)}, {ncut} cut{"s" * (ncut > 1)}')
        sections += lines + ['']
        n_host = sum(len(host.get(b, [])) for b in bids)
        summary.append(f'| [{name(g)}](#{slug(name(g))}) | {sum(map(len, cuts.values()))} | {sum(map(len, obs.values()))} | {n_fs} | '
                       f'{sum(len(v) for v in mc.values())} | {n_host} | {"yes" if general else ""} |')

    orphans = [f'- {short(bid)}: {link(p, n)}: {what}' for bid, rows in host.items() if bid not in builds for p, n, what in rows]

    sdk_lines = []
    for bid, cuts in getattr(sdk, 'CUTS', {}).items():
        shipped = sorted({f'{name(g)} {disp}' for g, disp, mod in builds.get(bid, []) if mod == 'sdk'})
        sdk_lines.append(f'- **sdk** {short(bid)} ({link(SDK, lineno(sdk_text, repr(bid)))}): ' + (', '.join(shipped) if shipped else 'no profile registers this build: see the comment above it'))
        for off, ob, nb, why in cuts: sdk_lines.append(f'  - cut `+{off:#x}`: {why}')
    rule_lines, comment = [], ''
    block = sdk_text[sdk_text.find("MATCH_CUTS = {'sdk'"):]
    for line in block.split('\n')[1:]:
        s = line.strip()
        if s.startswith('#'): comment = (comment + ' ' if comment else '') + s.lstrip('# ')
        elif s.startswith(('node(', 'align(')):
            rule_lines.append(f'- {"node area 1 GB → 62 MB" if s.startswith("node(") else "regions of 1 GB and more aligned to 32 MB"}: {comment}'); comment = ''
        elif s.startswith(']}'): break
    n_rules = sum(len(v) for v in getattr(sdk, 'MATCH_CUTS', {}).values())
    assert len(rule_lines) == n_rules, f'sdk_patches.py MATCH_CUTS: {len(rule_lines)} commented rules of {n_rules}'

    L = ['# Game-specific patches', '',
         'Every piece of code that runs for one game and not the others: the rewriter\'s patches of a game\'s builds (`dm/games/<id>/patches.py`, keyed by NSO build ID: '
         '[rewriter](../reference/rewriter.md)), the host\'s rows keyed by a game\'s build ID, the SDK cuts of the Nintendo SDK builds the games ship, translator rules keyed by one '
         'shader\'s code, and the profile keys that switch host behaviour. Each patch\'s reason is the one in its source; why it is needed, and what was measured, is in the game\'s note. '
         '**Generalize?** is the patch file\'s `GENERALIZE` note: whether a general rule could replace it (AGENTS.md prefers general fixes). Offsets are from the module\'s start. '
         'Generated by `dm/tools/docs_patches.py` from those sources: change the source, then run it.', '',
         '## Summary', '',
         '| Game | Cuts | Gates | Frame-step sites | Function-matched rules | Host rows | Generalize note |', '|---|---|---|---|---|---|---|',
         *summary, '',
         '## Games', '', *sections,
         '## SDK cuts', '',
         f'Cuts of Nintendo SDK `sdk` builds ({link(SDK)}), applied to every game that ships the build. They fit the SDK\'s address-space layout into an iPhone\'s guest block. The '
         'games listed are those whose profile registers the build.', '',
         *sdk_lines, '',
         '### SDK-wide function-matched rules', '',
         'Applied to the `sdk` of any build without CUTS whose profile was not brought up on it, found by function fingerprint ([function-matched cuts](../reference/rewriter.md#function-matched-cuts-for-unregistered-builds)):', '',
         *rule_lines, '',
         *(['## Host rows of builds no profile registers', '', 'Dead code unless such a build is run; a candidate for removal, or for a build row in its profile.', '', *orphans, ''] if orphans else []),
         '## Translator rules keyed by a shader\'s code', '',
         *[f'- {link(p, n)}: {c}' for p, n, c in shader_rules()], '',
         '## Profile switches', '',
         'Profile keys (`game.tsv`) whose value changes what the host does, set away from their default ([keys](../../dm/games/README.md)); the rarest first. Sizes, layouts and the '
         'controller layout are left out.', '',
         '| Key and value | Games |', '|---|---|',
         *[f'| `{k} {v}` | {", ".join(note(g) for g in gl)} |' for k, v, gl in switches(gs)], '']
    return '\n'.join(L).rstrip() + '\n'


def slug(h):
    from docs_symptoms import slug as s
    return s(h)


if __name__ == '__main__':
    text = page()
    if '--check' in sys.argv[1:]:
        if OUT.read_text() != text: sys.exit(f'{rel(OUT)} is out of date: run dm/tools/docs_patches.py')
        print(f'{rel(OUT)} is current')
    else:
        OUT.write_text(text)
        print(f'{rel(OUT)}: {text.count("### ")} games')
