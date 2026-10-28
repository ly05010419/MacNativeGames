#!/usr/bin/env python3
# Usage manifests and change impact (scripts/regress.py usage / impact). One chain and one host serve every profile, so a contributor who changes them needs to know which games,
# also ones they do not own, use what the change touches (GitHub issue #272). A game's usage manifest, dm/games/<id>/usage.tsv, holds only names and counts, no game content:
#   shader <TAB> <feature> <TAB> shaders      features of the game's translated shaders (stages, binding kinds, image / buffer atomics and whether a result is used, transform feedback,
#                                            layer output, integer outputs, fp64 lowering, translator notes, texture types, ...), counted from the chain build's MSL and reflection
#   nvn <TAB> <NVN function> <TAB> calls      the NVN functions a smoke run called (DM_NVN_TSV call rows), summed over the run
#   hle <TAB> <symbol> <TAB> calls            the SDK functions the host implements that the run called (DM_HLECOUNT: HLE table, forced takeovers, stubs)
#   svc <TAB> <number> <TAB> calls            the SVCs the run made (DM_HLECOUNT)
#   present <TAB> <feature> <TAB> 1           the frame step and present path the log names (frame step mode, present interval changes, display link, pipeline paths)
#   key <TAB> <profile key> <TAB> 1           the profile keys in game.tsv (their names: what the host's game_profile.h takes)
#   api <TAB> nvn|vulkan|gl <TAB> 1           the graphics API
# Impact: the changed functions of a git range (hunks mapped to the C / Objective-C / Python function around them), widened to their callers in the host (a call graph made by
# a scanner, not a compiler), the NVN entry points (NVN(...) in hle_nvn.c) and HLE symbols ({"symbol", fn} tables) among those callers, and the coarse rules of
# dm/tools/impact_map.tsv (a file, or a function in it, → usage rows) → the games whose manifests hold any of them; games without a manifest are unknown.
import collections, fnmatch, json, re, subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NOTE_SKIP = ('not in the answer table',)


def norm(s):
    return re.sub(r'\d+', '#', re.sub(r'\b[0-9a-f]{16}\b', '<key>', re.sub(r'"[^"]*"', '"…"', s)))[:100]


# ---------------------------------------------------------------- shader features (static)

MSL_TAGS = [
    ('image_atomic', re.compile(r'spvImage\w*AtomicCoord\((?!int|uint|tc)')),
    ('image_atomic_result', re.compile(r'=\s*atomic_\w+_explicit\([^;]*spvImage\w*AtomicCoord')),
    ('buffer_atomic', re.compile(r'atomic_\w+_explicit\((?![^;]*spvImage)')),
    ('discard', re.compile(r'\bdiscard_fragment\(')),
    ('gather', re.compile(r'\.gather(_compare)?\(')),
    ('sample_compare', re.compile(r'\.sample_compare\(')),
    ('gradient', re.compile(r'\bgradient(2d|3d|cube)\(')),
    ('lod_query', re.compile(r'\.calculate_(un)?clamped_lod\(')),
    ('simd', re.compile(r'\bsimd_\w+\(')),
    ('threadgroup_memory', re.compile(r'\bthreadgroup\s+\w')),
    ('texture_buffer', re.compile(r'\btexture_buffer<')),
    ('texture3d', re.compile(r'\btexture3d<')),
    ('texture_cube', re.compile(r'\btexturecube(_array)?<')),
    ('texture2d_array', re.compile(r'\btexture2d_array<')),
    ('texture_ms', re.compile(r'\btexture2d_ms(_array)?<')),
    ('depth_texture', re.compile(r'\bdepth(2d|cube)(_array)?<')),
    ('texture_write', re.compile(r'access::(write|read_write)\b')),
    ('device_buffer_write', re.compile(r'\bdevice\s+(?!const)\w')),
    ('fp64', re.compile(r'\bdouble\b')),
    ('position_invariant', re.compile(r'\[\[position, invariant\]\]')),
]


def shader_features(lib):
    """{feature: number of shaders} of one chain build's b-<V> folder"""
    c = collections.Counter()
    for j in sorted(Path(lib).glob('*_b.json')):
        try: r = json.loads(j.read_bytes())
        except ValueError: continue
        f = {'stage:' + str(r.get('stage'))}
        for b in r.get('bindings', []):
            f.add('binding:' + str(b.get('kind')))
            if b.get('rw'): f.add('binding_rw:' + str(b.get('kind')))
            if b.get('fetch1d'): f.add('fetch1d')
        for k, tag in (('entry_tf', 'transform_feedback'), ('entry_layer', 'layer_output'), ('entry_u', 'integer_outputs'), ('entry_m', 'mixed_outputs'), ('vin', 'integer_vertex_inputs'),
                       ('pull', 'pulling_vertex_inputs'), ('fetch', 'readable_tile'), ('vs_pad', 'vs_pad'), ('gs_slice', 'gs_layer_slice'), ('drv_unfillable', 'driver_read_unfillable')):
            if r.get(k): f.add(tag)
        for seg in r.get('drv_reads') or '': f.add('driver_read_c' + seg)
        if r.get('stage') == 'gs' and not r.get('gs_slice'): f.add('geometry_as_compute')
        for n in r.get('notes', []):
            if not n.startswith(NOTE_SKIP): f.add('note:' + norm(n.split(' (')[0]))
        m = j.with_name(j.name[:-5] + '.metal')
        if m.is_file():
            src = m.read_text(errors='replace')
            f.update(tag for tag, rx in MSL_TAGS if rx.search(src))
        c.update(f)
    return c


# ---------------------------------------------------------------- runtime usage (one smoke run)

PRESENT = [
    ('frame_step_vsync', re.compile(r'frame step in whole vsyncs')),
    ('frame_step_variable', re.compile(r'frame step variable')),
    ('present_interval_changes', re.compile(r'the game sets present interval')),
    ('display_link', re.compile(r'display link up to tick')),
    ('present_snapshots', re.compile(r'present snapshot', re.I)),
    ('metalfx', re.compile(r'MetalFX', re.I)),
    ('pipeline_prepare', re.compile(r'pipeline prepare: \d+ lines')),
    ('pipeline_prebuild', re.compile(r'pipeline prebuild')),
    ('hybrid_pipelines_off', re.compile(r'hybrid pipelines off')),
    ('pipeline_archive', re.compile(r'pipeline archive')),
    ('render_scale', re.compile(r'render scale', re.I)),
    ('nro_modules', re.compile(r'\bNRO\b')),
    ('audio_out', re.compile(r'OpenAudioOut|audren')),
    ('movie', re.compile(r'\bmovie\b|\bmm::|nvdec|VideoToolbox', re.I)),
]


def runtime_usage(run_dir, api):
    """[(kind, name, count)] of one run folder: frames.tsv call rows, hlecount.tsv, the log"""
    rows = collections.Counter()
    ft = Path(run_dir) / 'frames.tsv'
    if ft.is_file():
        with open(ft, errors='replace') as fh:
            for ln in fh:
                c = ln.rstrip('\n').split('\t')
                if len(c) == 3 and c[1].startswith('nvn') and c[2].isdigit(): rows['nvn', c[1]] += int(c[2])
    hc = Path(run_dir) / 'hlecount.tsv'
    if hc.is_file():
        for ln in hc.read_text(errors='replace').splitlines()[1:]:
            c = ln.split('\t')
            if len(c) == 3 and c[2].isdigit() and int(c[2]):
                rows[('svc' if c[0] == 'SVC' else 'hle'), c[1]] += int(c[2])
    log = Path(run_dir) / 'log.txt'
    text = log.read_text(errors='replace') if log.is_file() else ''
    for tag, rx in PRESENT:
        if rx.search(text): rows['present', tag] = 1
    rows['api', api] = 1
    return rows


def write_manifest(path, rows, header):
    order = {k: i for i, k in enumerate(('api', 'key', 'present', 'shader', 'nvn', 'hle', 'svc'))}
    lines = [f'# kind: usage  {header}', '# generated by scripts/regress.py usage (names and counts only); impact: scripts/regress.py impact <base>..<head>', 'kind\tname\tcount']
    lines += [f'{k}\t{n}\t{v}' for (k, n), v in sorted(rows.items(), key=lambda x: (order.get(x[0][0], 99), x[0][1]))]
    Path(path).write_text('\n'.join(lines) + '\n')


def read_manifest(path):
    rows = {}
    for ln in Path(path).read_text().splitlines():
        if not ln or ln.startswith(('#', 'kind\t')): continue
        c = ln.split('\t')
        if len(c) == 3: rows[c[0], c[1]] = int(c[2]) if c[2].isdigit() else 1
    return rows


# ---------------------------------------------------------------- the host's call graph (a scanner, not a compiler)

DEF = re.compile(r'^(?:static\s+|inline\s+|__attribute__\(\([^)]*\)\)\s+)*[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\(([^;{}]|\n)*?\)\s*\{', re.M)
NVN_DEF = re.compile(r'^NVN(?:_NOPROTO)?\((nvn\w+)\s*,', re.M)
HLE_DEF = re.compile(r'^HLE\(\s*"([^"]+)"\s*,\s*[^,]+,\s*(\w+)', re.M)   # HLE("symbol", ret, cname, ...) and HLE_FORCE("symbol", "why", ret, cname, ...) in dm/host/hle.h
HLE_FORCE_DEF = re.compile(r'^HLE_FORCE(?:_BIG)?\(\s*"([^"]+)"\s*,\s*"(?:[^"\\]|\\.)*"\s*,\s*[^,]+,\s*(\w+)', re.M)
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
TABLE = re.compile(r'\{\s*"([^"]+)"\s*,\s*(?:\(void\s*\*\)\s*)?&?([A-Za-z_]\w*)\s*\}')
KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'do', 'else', 'case', 'defined', 'typeof', '__typeof__', 'alignof'}
HOST_SRC = ('dm/host', 'dm/gfx', 'dm/gfx/nvn', 'dm/gfx/tex', 'dm/gfx/vk', 'dm/gfx/gl')
HOST_EXT = ('.c', '.m', '.inc', '.h')   # headers too: the frame step, the texture layouts and other static inline code live in them


def functions(path, text=None):
    """[(name, first line, last line)] of the functions defined at column 0 of a C / Objective-C / Python file (NVN(name, …) definitions named after the NVN function)"""
    text = text if text is not None else Path(path).read_text(errors='replace')
    if str(path).endswith('.py'):
        starts = [(m.group(1) or m.group(2), text.count('\n', 0, m.start()) + 1) for m in re.finditer(r'^(?:async\s+)?def\s+(\w+)|^class\s+(\w+)', text, re.M)]
    else:
        starts = sorted([(m.group(1), text.count('\n', 0, m.start()) + 1) for m in DEF.finditer(text) if m.group(1) not in KEYWORDS] +
                        [(m.group(1), text.count('\n', 0, m.start()) + 1) for m in NVN_DEF.finditer(text)] +
                        [(m.group(2), text.count('\n', 0, m.start()) + 1) for rx in (HLE_DEF, HLE_FORCE_DEF) for m in rx.finditer(text)], key=lambda x: x[1])
    total = len(text.splitlines())
    return [(n, l, (starts[i + 1][1] - 1 if i + 1 < len(starts) else total)) for i, (n, l) in enumerate(starts)]


def host_sources(root=REPO, rev=None):
    """[(path, text)] of the host's sources and headers: the files under root, or those of a git revision read from the object store (no checkout, no harness root)"""
    if rev is None:
        return [(str(p.relative_to(root)), p.read_text(errors='replace')) for d in HOST_SRC for p in sorted((Path(root) / d).glob('*')) if p.suffix in HOST_EXT and p.is_file()]
    ls = subprocess.run(['git', 'ls-tree', rev, '--', *[d + '/' for d in HOST_SRC]], cwd=root, capture_output=True, text=True, check=True).stdout.splitlines()
    paths = sorted(ln.split('\t', 1)[1] for ln in ls if ln.split()[1] == 'blob' and Path(ln.split('\t', 1)[1]).suffix in HOST_EXT)
    out = subprocess.run(['git', 'cat-file', '--batch'], cwd=root, input=''.join(f'{rev}:{p}\n' for p in paths).encode(), capture_output=True, check=True).stdout
    files, i = [], 0
    for p in paths:   # <sha> blob <size>\n<content>\n per path
        nl = out.index(b'\n', i); size = int(out[i:nl].split()[2]); i = nl + 1
        files.append((p, out[i:i + size].decode(errors='replace'))); i += size + 1
    return files


def host_graph(root=REPO, rev=None):
    """(callers: {(file, function): {(file, function) calling it}}, roots: {(file, function): {('nvn' | 'hle', name)}}, defs: {function: [files]}). A call resolves to the function
    of the same name in the calling file when there is one (static functions of the same name in several files), else to every file that defines it; a function defined in a
    header (static inline ones included) is visible to every file, as to the files that include it"""
    defs, bodies, roots, local = collections.defaultdict(list), {}, collections.defaultdict(set), set()
    for rel, text in host_sources(root, rev):
        lines = text.splitlines()
        for name, a, b in functions(rel, text):
            defs[name].append(rel); bodies[rel, name] = '\n'.join(lines[a - 1:b])
            if not rel.endswith('.h') and re.match(r'(static|NVN|HLE)\b', lines[a - 1]): local.add((rel, name))   # file-local: not called from other files
            if rel.endswith('/hle_nvn.c') and name.startswith('nvn'): roots[rel, name].add(('nvn', name))
        for sym, fn in TABLE.findall(text) + [(m.group(1), m.group(2)) for rx in (HLE_DEF, HLE_FORCE_DEF) for m in rx.finditer(text)]: roots[rel, fn].add(('hle', sym))
    callers = collections.defaultdict(set)
    for (rel, name), body in bodies.items():
        for c in set(CALL.findall(body)) - KEYWORDS - {name}:
            if c not in defs: continue
            for f in ([rel] if rel in defs[c] else [x for x in defs[c] if (x, c) not in local]): callers[f, c].add((rel, name))
    return callers, roots, defs


def reach(changed, callers, roots, limit=4000):
    """The NVN entry points and HLE functions whose implementation calls a changed (file, function), at any depth"""
    seen, todo, hit = set(changed), list(changed), set()
    while todo and len(seen) < limit:
        f = todo.pop()
        hit |= roots.get(f, set())
        for c in callers.get(f, ()):
            if c not in seen: seen.add(c); todo.append(c)
    return hit, len(seen)


# ---------------------------------------------------------------- what a git range changes

def changed_functions(rng, root=REPO):
    """{file: set of function names whose lines the range changes ('' for lines outside any function)}"""
    out = collections.defaultdict(set)
    files = subprocess.run(['git', 'diff', '--name-only', rng], cwd=root, capture_output=True, text=True, check=True).stdout.split()
    head = rng.split('..')[-1] or 'HEAD'
    for f in files:
        diff = subprocess.run(['git', 'diff', '-U0', rng, '--', f], cwd=root, capture_output=True, text=True, errors='replace').stdout
        src = f.endswith(('.c', '.m', '.inc', '.h', '.py'))
        new = subprocess.run(['git', 'show', f'{head}:{f}'], cwd=root, capture_output=True, text=True, errors='replace') if src else None
        fns = functions(f, new.stdout) if new and new.returncode == 0 else []
        lines = set()   # every line a hunk adds or changes; a pure deletion (+c,0) is after line c, inside the function around it
        for m in re.finditer(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@', diff, re.M):
            c, n = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
            lines.update(range(c, c + n) if n else (max(c, 1),))
        if not fns: out[f].add(''); continue
        for ln in sorted(lines):
            out[f].add(next((n for n, a, b in fns if a <= ln <= b), ''))
    return out


def load_map(path=REPO / 'dm/tools/impact_map.tsv'):
    """[(file glob, function regex or None, usage pattern, why)] of the coarse map"""
    rules = []
    for ln in Path(path).read_text().splitlines():
        if not ln.strip() or ln.startswith('#'): continue
        c = ln.split('\t')
        f, _, fn = c[0].partition(':')
        rules.append((f, re.compile(fn) if fn else None, c[1], c[2] if len(c) > 2 else ''))
    return rules


BROAD = re.compile(r'^(\*:\*|api:.*|shader:stage:\*|svc:\*|hle:\*|present:\*|\*:.*)$')   # patterns every game of an API matches


def rules_for(f, fns, rules):
    """The rules a changed file reaches: per changed function the rules whose regex matches it, else the file's rules without one; a file a "-" rule matches reaches nothing"""
    mine = [r for r in rules if fnmatch.fnmatch(f, r[0])]
    if any(r[2] == '-' for r in mine): return [], True
    out = []
    for fn in fns or {''}:
        spec = [r for r in mine if r[1] is not None and fn and r[1].search(fn)]
        out += spec or [r for r in mine if r[1] is None]
    return out, bool(mine)


def impact(rng, manifests, root=REPO, rules=None):
    """→ {'changed': {file: [functions]}, 'names': [(kind, name, why)], 'patterns': [(pattern, why)], 'unmapped': [file],
          'games': {id: {'status': affected | not affected | unknown, 'matches': [(kind, name, count, why)]}}}"""
    rules = rules if rules is not None else load_map(Path(root) / 'dm/tools/impact_map.tsv')
    ch = changed_functions(rng, root)
    callers, roots, _ = host_graph(root, rev=rng.split('..')[-1] or 'HEAD')   # the call graph of the range's head, read from git
    names, hits, unmapped = {}, [], []
    for f, fns in ch.items():
        if f.startswith(HOST_SRC) and f.endswith(('.c', '.m', '.inc', '.h')):
            hit, _ = reach({(f, n) for n in fns if n}, callers, roots)
            for k, n in hit: names.setdefault((k, n), f'{f}: calls reach it from {", ".join(sorted(x for x in fns if x))[:80]}')
        rs, mapped = rules_for(f, fns, rules)
        if not mapped and not any(names.get(k) for k in names if names[k].startswith(f + ':')): unmapped.append(f)
        hits += [(f, r) for r in rs]
    strong = [(f, r) for f, r in hits if not r[3].startswith('fallback')]
    use = strong + ([(f, r) for f, r in hits if r[3].startswith('fallback')] if not strong else [])
    patterns = sorted({(r[2], f'{f}{":" + r[1].pattern if r[1] else ""}: {r[3]}') for f, r in use})
    games = {}
    for gid, rows in manifests.items():
        if rows is None: games[gid] = {'status': 'unknown', 'matches': []}; continue
        m = {}
        for (k, n), why in names.items():
            if (k, n) in rows: m[k, n] = why
        for pat, why in sorted(patterns, key=lambda x: bool(BROAD.match(x[0]))):   # a narrow pattern's reason first
            if pat == 'profile':
                if any(f'dm/games/{gid}/' in w for w in [why]): m['profile', gid] = why
                continue
            k, _, n = pat.partition(':')
            for (rk, rn) in rows:
                if (k == '*' or rk == k) and fnmatch.fnmatchcase(rn, n or '*'): m.setdefault((rk, rn), why)
        narrow = {k: w for k, w in m.items() if not any(BROAD.match(pt) and w == pw for pt, pw in patterns)}
        games[gid] = {'status': 'affected' if narrow else 'broad' if m else 'not affected', 'narrow': sorted(((k, n, rows.get((k, n), 1), w) for (k, n), w in narrow.items()), key=lambda x: ('calls reach' in x[3], -x[2], x[0], x[1])),   # feature rows first, then the entry points the call graph reaches
                      'matches': sorted(((k, n, rows.get((k, n), 1), why) for (k, n), why in m.items()), key=lambda x: (-x[2], x[0], x[1]))}
    return {'changed': {f: sorted(x for x in v if x) for f, v in ch.items()}, 'names': sorted((k, n, w) for (k, n), w in names.items()), 'patterns': patterns,
            'unmapped': unmapped, 'games': games}
