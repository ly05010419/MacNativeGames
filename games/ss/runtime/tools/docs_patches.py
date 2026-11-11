#!/usr/bin/env python3
# Writes docs/agents/game-patches.md, the index of game-specific code: per game, the rewriter's patches of its builds (CUTS, OBS, FRAME_STEP sites, MATCH_CUTS and CAPABILITIES of its
# dm/games/<id>/patches.py, with each patch's reason), the host's rows keyed by its build IDs (32-hex string literals in dm/host and dm/gfx), and each patch's binding (below); then the SDK
# cuts of dm/tools/sdk_patches.py by sdk build and the games that ship it, the SDK-wide function-matched rules, translator rules keyed by a shader's code hash, and the profile keys that switch
# host behaviour away from their default, with the games that set them. The gaps (patches bound tighter than they could be) are listed first: the work list. Frame-step sites are found by running each
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

# ---- Binding: what a patch's correctness is anchored to, from the loosest (GitHub issue #635). A patch is at its goal when its binding is the loosest it could have; the gap between the two
# is the work list. A patch file's BINDING is a list of {'what', 'now', 'goal', 'purpose', 'why'} with an optional 'blocked' (why the gap cannot close yet) or 'kept' (why it stays
# open on purpose: a decision, not an oversight), and an optional 'checked' (what was checked on game files, and when; without it the goal is a judgement); the host's build-keyed rows,
# translator rules and profile switches have their entries below, since they have no patch file. Every one must be classified: the page is not written otherwise. A bring-up need not guess
# the goal: a game's own build-bound patch may say goal 'unreviewed' (listed under Gaps, after the reviewed ones); nothing shared (sdk_patches.py, host rows, translator rules, switches) may
BINDINGS = {   # binding: (anchored to, found by, survives)
    'host': ('Horizon / NVN / Vulkan semantics', 'nothing in the binary', 'every game and update'),
    'sdk': ('Nintendo SDK code (the `sdk` module)', 'a symbol, or a fingerprint of a reviewed SDK implementation', 'any game shipping that implementation'),
    'engine': ('middleware code: Unreal, Unity, Snowdrop, KTGL, sead, PhysX', 'a registered name, a string, or a fingerprint per engine version', 'any game on that engine version'),
    'game': ("the game's own code, or its profile's data", 'a function fingerprint (`MATCH_CUTS`), or a profile key', "the game's updates that keep the function"),
    'build': ('fixed offsets', 'the NSO build ID', 'nothing: redone for every update'),
}
RANK = {b: i for i, b in enumerate(BINDINGS)}
PURPOSES = ('address space', 'memory', 'race', 'frame rate', 'power', 'rendering', 'shaders', 'runtime')
UNREVIEWED = 'unreviewed'   # a goal nobody has judged yet


def binding_error(e, own=False):   # what is wrong with a BINDING entry (own: in a game's own patches.py), or None
    if not isinstance(e, dict) or set(e) - {'what', 'now', 'goal', 'purpose', 'why', 'blocked', 'kept', 'checked'} or \
            not all(isinstance(e.get(k), str) and e[k] for k in ('what', 'now', 'goal', 'purpose', 'why')) or not all(isinstance(e[k], str) and e[k] for k in ('blocked', 'kept', 'checked') if k in e):
        return f'not {{what, now, goal, purpose, why[, blocked or kept][, checked]}} strings: {e!r}'
    if e['goal'] == UNREVIEWED:
        if not own or e['now'] != 'build': return f'{e["what"]}: only a game\'s own build-bound patch may leave its goal unreviewed'
        if 'blocked' in e or 'kept' in e: return f'{e["what"]}: an unreviewed goal has no gap to block or keep'
    elif e['now'] not in RANK or e['goal'] not in RANK: return f'{e["what"]}: bindings are {", ".join(BINDINGS)}'
    elif RANK[e['goal']] > RANK[e['now']]: return f'{e["what"]}: the goal {e["goal"]} is tighter than the binding now, {e["now"]}'
    if e['purpose'] not in PURPOSES: return f'{e["what"]}: purposes are {", ".join(PURPOSES)}'
    if ('blocked' in e or 'kept' in e) and e['now'] == e['goal']: return f'{e["what"]}: blocked and kept are reasons a gap stays open; this patch is at its goal'
    if 'blocked' in e and 'kept' in e: return f'{e["what"]}: a gap is blocked or kept, not both'
    return None


def same(e): return (e['what'], e['now'], e['goal'], e['purpose'])   # one patch, wherever it is listed (several games, or a cut and its host row)
def status(e):
    if e['goal'] == UNREVIEWED: return UNREVIEWED
    return 'at goal' if e['now'] == e['goal'] else f'blocked: {e["blocked"]}' if 'blocked' in e else f'kept: {e["kept"]}' if 'kept' in e else 'gap'
def checked(e): return f'checked: {e["checked"]}' if 'checked' in e else 'judgement'
def reason(e): return e["why"][:1].upper() + e["why"][1:]
def arrow(e): return f'`{e["now"]}`' if e['now'] == e['goal'] else f'`{e["now"]}` → {UNREVIEWED}' if e['goal'] == UNREVIEWED else f'`{e["now"]}` → `{e["goal"]}`'
def payoff(gap):   # the Gaps order: open gaps, then blocked, kept and unreviewed ones; in each, the most games reached first, then the farthest from the goal
    e, _, ids = gap
    group = 3 if e['goal'] == UNREVIEWED else 1 if 'blocked' in e else 2 if 'kept' in e else 0
    return group, -max(1, len(ids)), RANK.get(e['goal'], 0) - RANK[e['now']], e['what'].casefold()


# The host's build-keyed rows: (table, the start of the row's label ('' = any), main build ('' = any), entry); the first that fits classifies a row
HOST_BINDING = [
    ('k_locks', 'Unity', '', {'what': "Unity's Application.targetFrameRate at a fixed address", 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'why': 'an IL2CPP player registers the setter by its icall name, so the field can be found in any Unity game; whether to raise it stays the profile\'s'}),
    ('k_locks', 'RE Engine', '', {'what': "RE Engine's frame limit at a fixed pointer", 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'why': 'engine code other RE Engine games ship', 'blocked': 'one RE Engine game here'}),
    ('k_locks', 'NLG', '', {'what': "Next Level Games' frame limiter target at a fixed address", 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'why': "the studio's engine, which its other games ship", 'blocked': 'one game on that engine here'}),
    ('k_locks', 'Criterion', '', {'what': "Criterion's 60 fps mode, frame ticks and game step at fixed addresses", 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'why': "the studio's engine, which its other games ship", 'blocked': 'one game on that engine here'}),
    ('k_locks', 'FIFA', '', {'what': "Frostbite's frame regulator at a fixed pointer", 'now': 'build', 'goal': 'engine', 'purpose': 'frame rate',
     'why': "engine code other Frostbite games ship (the simulation's own rate is already set by name)", 'blocked': 'one Frostbite game here'}),
    ('k_ue', '', 'feae83dc9e9fb34c77d0a62f1fa9ae9f', {'what': "UE3's GMalloc setup skipped, the host allocator in (hle_uemalloc.c)", 'now': 'build', 'goal': 'game', 'purpose': 'address space',
     'why': "UE3 inlines the allocator's constructor into the GMalloc setup and has its own vtable layout (25 slots, 32-bit sizes), so the Unreal 4/5 discovery does not apply"}),
    ('k_nier_dr', '', '', {'what': "NieR's frame interval and dynamic-resolution floor at fixed addresses", 'now': 'build', 'goal': 'game', 'purpose': 'frame rate',
     'checked': "both releases' mains, where these rows were found",
     'why': "the game's own display object; the Asian release's function is the global one's 0x13c0 further on, so the rewriter's function match could give the host both releases' addresses"}),
]
# Translator rules keyed by one shader's code hash: (the start of the rule's comment, entry)
SHADER_BINDING = [
    ('Tears of the Kingdom 1.4.3 visibility counter', {'what': 'one vertex shader\'s visibility counter (VertexCounterLowering)', 'now': 'game', 'goal': 'host', 'purpose': 'shaders',
     'why': 'a general lowering of subgroup ballots in vertex shaders would cover any shader that counts that way'}),
]
# Profile switches: (key, the start of its value ('' = any), entry). A key is per-game data, so `game`; its goal is `host` where it is the semantics every game should get
DEFAULT_SOON = 'a default for every game after `regress.py check` on the games'
SWITCH_BINDING = [
    ('render_cond', '', {'what': 'NVN conditional rendering', 'now': 'game', 'goal': 'host', 'purpose': 'rendering',
     'why': f'what the GPU does; off by default only because other games drew everything before: {DEFAULT_SOON}'}),
    ('gpu_timestamps', '', {'what': 'timestamp reports per segment', 'now': 'game', 'goal': 'host', 'purpose': 'rendering', 'why': f'what hardware records: {DEFAULT_SOON}'}),
    ('mem_map_limit', '', {'what': 'MapPhysicalMemory limited to the application pool', 'now': 'game', 'goal': 'host', 'purpose': 'memory',
     'why': "Horizon's limit; off by default because Ace Attorney Investigations Collection maps a little more than its profile's 1 GB pool, which is the thing to fix"}),
    ('alloc_big_align', '', {'what': "StandardAllocator blocks of 64 KB and more aligned to 64 KB", 'now': 'game', 'goal': 'host', 'purpose': 'memory',
     'why': "the cause is a memory pool's GPU address, big-page aligned on hardware and the CPU address here: pool GPU addresses aligned as hardware does, or this alignment for every game"}),
    ('vconv_range', '', {'what': 'CPU vertex conversion over the vertices a draw reads', 'now': 'game', 'goal': 'host', 'purpose': 'power',
     'why': f'the same vertices as the whole range, for less work; DM_NVN_VCONV_CHECK checks it: {DEFAULT_SOON}'}),
    ('tex_flush_upload', '', {'what': 'CPU-written textures uploaded after a flush', 'now': 'game', 'goal': 'host', 'purpose': 'rendering',
     'why': 'unified memory semantics; off by default because a flushed range could cover an image a shader writes: tracking GPU writes would make it safe for every game'}),
    ('tex_dirty_upload', '', {'what': 'CPU-written textures uploaded without a flush', 'now': 'game', 'goal': 'host', 'purpose': 'rendering',
     'why': 'as tex_flush_upload, for pools the CPU writes without a flush'}),
    ('tex_dyn_slots', '', {'what': "buffer-texture handles indexed through c2's tables", 'now': 'game', 'goal': 'host', 'purpose': 'shaders',
     'why': "right only where the handle comes from c2's tables: the translator could prove that per shader"}),
    ('feedback_snap', '', {'what': 'draws that sample their own colour attachment read a snapshot', 'now': 'game', 'goal': 'host', 'purpose': 'rendering',
     'why': 'the replay could find such draws itself, at the cost of a pass split each'}),
    ('enc_thread', '', {'what': 'Metal encoding on its own thread on the Mac', 'now': 'game', 'goal': 'host', 'purpose': 'frame rate',
     'why': f'what iOS does for every game: {DEFAULT_SOON}'}),
    ('present_snap', '', {'what': 'present snapshots', 'now': 'game', 'goal': 'host', 'purpose': 'rendering', 'why': f'a setting the game was tested with: {DEFAULT_SOON}'}),
    ('pso_mtl4', '', {'what': 'MTL4Compiler pipelines on iOS', 'now': 'game', 'goal': 'host', 'purpose': 'rendering', 'why': f'a setting the game was tested with on an iPhone: {DEFAULT_SOON}'}),
    ('game_env', 'DOTNET_GCRegionRange=', {'what': ".NET's garbage-collector region range", 'now': 'game', 'goal': 'game', 'purpose': 'address space', 'why': "measured for the game's regions"}),
    ('stdalloc_cap', '', {'what': "StandardAllocator's address space", 'now': 'game', 'goal': 'game', 'purpose': 'address space',
     'why': "what the regions after it need, measured by the NSP script's region check; the host's cap is the default"}),
    ('shader_key_skip', '', {'what': 'control-section bytes left out of shader keys', 'now': 'game', 'goal': 'game', 'purpose': 'shaders',
     'why': "the engine shares one control section among shaders; leaving GLSLC's compile information out of every game's keys would change every shader key"}),
    ('ue_texture_pool', '', {'what': "Unreal's texture streaming pool", 'now': 'game', 'goal': 'game', 'purpose': 'memory', 'why': "the game's own budget, raised to its Windows value"}),
    ('present_interval', '', {'what': 'one frame per display tick', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate', 'why': 'right only for games whose logic does not count frames'}),
    ('fps_hold', '', {'what': 'no frame held past the cap', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate', 'why': "the game's pacing"}),
    ('fps_target', '', {'what': 'frame rate target', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate', 'why': 'a choice per game'}),
    ('ue_fps_cvar', '', {'what': "the game's own Unreal frame-limit variable", 'now': 'game', 'goal': 'game', 'purpose': 'frame rate', 'why': "the game's own variable, which its settings apply to t.MaxFPS; the rewriter finds it by name (ue_cvar.py)"}),
    ('perf_mode', '', {'what': 'Boost whatever the operation mode', 'now': 'game', 'goal': 'game', 'purpose': 'runtime', 'why': 'a combination a Switch 1 never reports: a choice per game'}),
    ('op_mode', '', {'what': 'the operation mode at start', 'now': 'game', 'goal': 'game', 'purpose': 'runtime', 'why': "the game's controls (handheld: the touch screen instead of a gyro pointer)"}),
]


def classify(table, *keys):   # the entry of the first row whose keys each match (equal, or a prefix of the value, or '' for any)
    for r in table:
        if all(k == '' or (v or '').startswith(k) for k, v in zip(r[:-1], keys)): return r[-1]
    return None


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
    for p in sorted(REPO / f for f in set(tracked) if f):   # a set: ls-files lists a file in a merge conflict once per stage
        if p.suffix not in HOST_EXT or 'tests' in p.relative_to(REPO).parts[2:]: continue
        table = None
        for n, line in enumerate(p.read_text(errors='replace').split('\n'), 1):
            m = re.search(r'\b(k_\w+)\s*\[\]\s*=', line)
            if m: table = m.group(1)
            if line.lstrip().startswith('}'): table = None
            for m in re.finditer(r'"([0-9a-f]{32})"', line):
                strings = re.findall(r'"([^"]*)"', line)
                what = TABLES.get(table, table) if table else 'build check'
                label = strings[1] if table == 'k_locks' and len(strings) > 1 else ''
                if label: what = f'{what}: {label}'
                e = classify(HOST_BINDING, table or '', label, m.group(1))
                if e is None: raise SystemExit(f'{rel(p)}:{n}: a build-keyed host row docs_patches.py HOST_BINDING does not classify')
                out.setdefault(m.group(1), []).append((p, n, what if m.group(1) in builds else f'{what} (a build no profile registers)', e))
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
            c = re.split(r'(?<=[^.]\.) |: ', comment)[0] if comment else ''
            e = classify(SHADER_BINDING, c)
            if e is None: raise SystemExit(f'{rel(p)}:{n}: a translator rule docs_patches.py SHADER_BINDING does not classify')
            out.append((p, n, c, e))
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
            out.setdefault((k, v), []).append(g)
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
    gaps = []   # (entry, where, the IDs of the games it reaches)
    def gap(e):
        x = next((t for t in gaps if same(t[0]) == same(e)), None)
        if x is None: x = (e, [], set()); gaps.append(x)
        return x
    def bound(m, where, own=False):
        es = getattr(m, 'BINDING', None)
        if not es: raise SystemExit(f'{where}: no BINDING')
        for e in es:
            err = binding_error(e, own)
            if err: raise SystemExit(f'{where}: BINDING {err}')
        return es

    summary, sections = [], []
    for g in gs:
        m = mods.get(g.id)
        if not m and g.id not in host_games: continue
        text = files[g.id].read_text() if m else ''
        cuts, obs, fs, mc, caps = (getattr(m, k, {}) for k in ('CUTS', 'OBS', 'FRAME_STEP', 'MATCH_CUTS', 'CAPABILITIES'))
        bids = list(dict.fromkeys([*cuts, *obs, *fs, *caps, *sorted(host_games.get(g.id, ()))]))
        n_fs = 0; lines = [f'### {name(g)}', '', f'{note(g)}, `{g.id}`' + (f', {link(files[g.id])}' if m else '') + '.', '']
        es = list(bound(m, rel(files[g.id]), own=True)) if m else []
        for b in sorted(host_games.get(g.id, ())):
            for _, _, _, e in host.get(b, []):
                if not any(same(x) == same(e) for x in es): es.append(e)
        for e in es:
            lines.append(f'- **{arrow(e)}**, {e["purpose"]}, {status(e)}, {checked(e)}: {e["what"]}. {reason(e)}')
            if e['now'] != e['goal']:
                _, where, ids = gap(e); ids.add(g.id)
                if note(g) not in where: where.append(note(g))
        if es: lines.append('')
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
            for p, n, what, _ in host.get(bid, []): lines.append(f'  - host {link(p, n)}: {what}')
        for module, rules in mc.items():
            for r in rules:
                fns = r.get('functions', [r]); ncut = sum(len(f['cuts']) for f in fns)
                lines.append(f'- **function-matched** (`{module}` of builds without CUTS): {r["name"]}: {len(fns)} function{"s" * (len(fns) > 1)}, {ncut} cut{"s" * (ncut > 1)}')
        sections += lines + ['']
        n_host = sum(len(host.get(b, [])) for b in bids)
        kinds = {}
        for e in es: kinds[arrow(e)] = kinds.get(arrow(e), 0) + 1
        summary.append(f'| [{name(g)}](#{slug(name(g))}) | {sum(map(len, cuts.values()))} | {sum(map(len, obs.values()))} | {n_fs} | '
                       f'{sum(len(v) for v in mc.values())} | {n_host} | {", ".join(k + (f" ×{n}" if n > 1 else "") for k, n in kinds.items())} |')

    orphans = [f'- {short(bid)}: {link(p, n)}: {what}' for bid, rows in host.items() if bid not in builds for p, n, what, _ in rows]
    sdk_bind = bound(sdk, rel(SDK))
    for e in sdk_bind:   # the games that ship the builds an entry names (by their first 8 hex digits)
        if e['now'] == e['goal']: continue
        _, where, ids = gap(e); where.append(link(SDK))
        for b in re.findall(r'\b[0-9a-f]{8}\b', e['what']): ids.update(g.id for bid, rows in builds.items() if bid.startswith(b) for g, _, mod in rows if mod == 'sdk')
    shaders = shader_rules()
    for p, n, c, e in shaders:
        if e['now'] != e['goal']: gap(e)[1].append(link(p, n))
    sw = []
    for k, v, gl in switches(gs):
        e = classify(SWITCH_BINDING, k, v)
        if e is None: raise SystemExit(f'profile key {k} {v}: a switch docs_patches.py SWITCH_BINDING does not classify')
        sw.append((k, v, gl, e))
        if e['now'] != e['goal']:
            _, where, ids = gap(e); ids.update(g.id for g in gl)
            where.append(f'`{k}`: ' + ', '.join(note(g) for g in gl))
    gaps.sort(key=payoff)
    n_unreviewed = sum(e['goal'] == UNREVIEWED for e, _, _ in gaps)

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
            rule_lines.append(f'- {"node area 1 GB → 62 MB" if s.startswith("node(") else "regions of 4 MB and more aligned to 2 MB, as SDK 20"}: {comment}'); comment = ''
        elif s.startswith(']}'): break
    n_rules = sum(len(v) for v in getattr(sdk, 'MATCH_CUTS', {}).values())
    assert len(rule_lines) == n_rules, f'sdk_patches.py MATCH_CUTS: {len(rule_lines)} commented rules of {n_rules}'

    L = ['# Game-specific patches', '',
         'Every piece of code that runs for one game and not the others: the rewriter\'s patches of a game\'s builds (`dm/games/<id>/patches.py`, keyed by NSO build ID: '
         '[rewriter](../reference/rewriter.md)), the host\'s rows keyed by a game\'s build ID, the SDK cuts of the Nintendo SDK builds the games ship, translator rules keyed by one '
         'shader\'s code, and the profile keys that switch host behaviour. Each patch\'s reason is the one in its source; why it is needed, and what was measured, is in the game\'s note. '
         'Each patch states its binding: what it depends on now, the loosest it could depend on (its goal), its purpose and why ([Binding](#binding)); the [gaps](#gaps) are the work list. '
         'Offsets are from the module\'s start. Generated by `dm/tools/docs_patches.py` from those sources (a patch file\'s `BINDING`; the host rows\', translator rules\' and profile '
         'switches\' entries in `docs_patches.py`): change the source, then run it.', '',
         '## Binding', '',
         'A patch\'s binding is what its correctness is anchored to, from the loosest. A patch is good design when its binding is the loosest that is correct (AGENTS.md prefers general '
         'fixes); the rules are in GitHub issue #635.', '',
         '| Binding | Anchored to | Found by | Survives |', '|---|---|---|---|',
         *[f'| `{b}` | {a} | {f} | {s} |' for b, (a, f, s) in BINDINGS.items()], '',
         '- Status: **at goal** (the binding now is the goal), **gap** (it could be looser: the work list), **blocked** (a gap with the reason it cannot close yet), **kept** (a gap left '
         'open on purpose, with the decision) or **unreviewed** (a game\'s own build-bound patch whose goal nobody has judged yet, which a bring-up may leave rather than guess).',
         '- Locators fail closed: a patch that finds its place in the binary (a function fingerprint, a registered name, a string) applies only where it finds exactly one match with '
         'its assumptions checked, and changes nothing otherwise, with a red line where it was expected to match (a profile\'s own `MATCH_CUTS`, a host search) and a plain one where a '
         'module may not need it (an SDK-wide rule). That is what makes a looser binding safe on builds nobody has seen ([function-matched cuts](../reference/rewriter.md#function-matched-cuts-for-unregistered-builds)).',
         '- Evidence: **checked** (what was checked on game files, and when) or **judgement** (the goal is reasoned from the code and notes, not checked on game files).',
         f'- Purposes: {", ".join(PURPOSES)}.', '',
         '## Gaps', '',
         'Every patch bound tighter than it could be: the open gaps, then the blocked, kept and unreviewed ones; in each, those reaching the most games first (the games whose profiles '
         'register the builds it touches), then those farthest from their goal.' + (f' {n_unreviewed} patch{"es have" if n_unreviewed > 1 else " has"} no reviewed goal yet.' if n_unreviewed else ''), '',
         '| Now → goal | Patch | Purpose | Status | Evidence | Games | Where | Why |', '|---|---|---|---|---|---|---|---|',
         *[f'| {arrow(e)} | {e["what"]} | {e["purpose"]} | {status(e)} | {checked(e)} | {len(ids) or "—"} | {"; ".join(w)} | {e["why"]} |' for e, w, ids in gaps], '',
         '## Summary', '',
         '| Game | Cuts | Gates | Frame-step sites | Function-matched rules | Host rows | Binding |', '|---|---|---|---|---|---|---|',
         *summary, '',
         '## Games', '', *sections,
         '## SDK cuts', '',
         f'Cuts of Nintendo SDK `sdk` builds ({link(SDK)}), applied to every game that ships the build. They fit the SDK\'s address-space layout into an iPhone\'s guest block. The '
         'games listed are those whose profile registers the build.', '',
         *[f'- **{arrow(e)}**, {e["purpose"]}, {status(e)}, {checked(e)}: {e["what"]}. {reason(e)}' for e in sdk_bind], '',
         *sdk_lines, '',
         '### SDK-wide function-matched rules', '',
         'Applied to the `sdk` of any build without CUTS, found by function fingerprint ([function-matched cuts](../reference/rewriter.md#function-matched-cuts-for-unregistered-builds)):', '',
         *rule_lines, '',
         *(['## Host rows of builds no profile registers', '', 'Dead code unless such a build is run; a candidate for removal, or for a build row in its profile.', '', *orphans, ''] if orphans else []),
         '## Translator rules keyed by a shader\'s code', '',
         *[f'- {link(p, n)}: {c} (**{arrow(e)}**, {status(e)}: {e["why"]})' for p, n, c, e in shaders], '',
         '## Profile switches', '',
         'Profile keys (`game.tsv`) whose value changes what the host does, set away from their default ([keys](../../dm/games/README.md)); the rarest first. Sizes, layouts and the '
         'controller layout are left out.', '',
         '| Key and value | Games | Binding | Why |', '|---|---|---|---|',
         *[f'| `{k} {v}` | {", ".join(note(g) for g in gl)} | {arrow(e)}, {status(e)} | {e["why"]} |' for k, v, gl, e in sw], '']
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
