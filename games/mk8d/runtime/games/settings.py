#!/usr/bin/env python3
# The iOS launcher's settings for one game, from settings.tsv (its header describes the columns): the rows whose condition holds for the game, with the game's defaults, as JSON.
# build_ipa.sh writes it into the app as dm/launcher.json; Launcher.swift draws the screen from it (an app without the file starts the game at once).
# Usage: settings.py <game id> <rewritten modules dir> <control.nacp> <out.json>   (DM_GAMES_DIR finds provisional profiles, as in game.py; build:<VAR> defaults come from the environment;
#   GFXAPI is the packer's graphics API, SETTINGS_PLATFORM the app's platform: ios (default) or mac, whose launcher is the Mac front end)
import json, os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parent / 'tools'))
import game, nacp

SECTIONS = ('Game', 'Controls', 'Performance')   # headings on the single settings screen
SCHEMA = 1   # launcher.json's version (docs/launcher.schema.json); it grows only by optional fields, so a reader of 1 reads every 1
# The meta column's keys, each with the pattern its value must match. A key is added here, in docs/frontends.md and in
# docs/launcher.schema.json together (test_launcher_settings.py checks that the three agree); a reader ignores keys it does not know
META = {
    'level': r'basic',                       # a setting a front end shows first; without it, the front end decides
    'needs': r'DM_[A-Z0-9_]+=[^;=]+',        # <VAR>=<value>: a sub-setting, drawn only while the row that sets <VAR> has that value
}


def meta(text, n):
    """The meta column as {key: value}: key=value pairs separated by ;, each key from META once, its value matching its pattern"""
    out = {}
    for pair in filter(None, (p.strip() for p in text.split(';'))):
        key, eq, value = pair.partition('=')
        if not eq or key not in META: sys.exit(f'settings.tsv:{n}: unknown meta key {pair!r} (keys: {", ".join(META)})')
        if key in out: sys.exit(f'settings.tsv:{n}: meta key {key} given twice')
        if not re.fullmatch(META[key], value): sys.exit(f'settings.tsv:{n}: meta {key}={value!r} does not match {META[key]}')
        out[key] = value
    return out


def render_base(g):
    """The profile's render_base ("<w>x<h> <w>x<h>": the 3D views' base size docked, then handheld, which DM_RENDER_SCALE scales) as
    {"docked": [w, h], "handheld": [w, h]}, for a front end to show the render scale in pixels; None when the profile has none"""
    v = g.get('render_base', '')
    if not v: return None
    m = re.fullmatch(r'\s*(\d+)x(\d+)\s+(\d+)x(\d+)\s*', v)
    if not m: sys.exit(f'{g.id}: render_base is "<w>x<h> <w>x<h>" (docked, then handheld): {v}')
    return {'docked': [int(m[1]), int(m[2])], 'handheld': [int(m[3]), int(m[4])]}


def rows():
    for n, line in enumerate((HERE / 'settings.tsv').read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'): continue
        c = line.split('\t')
        if len(c) not in (7, 8, 9): sys.exit(f'settings.tsv:{n}: 7 to 9 columns expected, {len(c)} found')
        if c[5] not in SECTIONS: sys.exit(f'settings.tsv:{n}: unknown section: {c[5]}')
        yield n, c + [''] * (9 - len(c))


def fixups_keys(path):   # '# KEY<TAB>value' header rows of a rewriter table
    if not path.is_file(): return set()
    return {m[1] for m in re.finditer(r'^# (\w+)\t', path.read_text(encoding='utf-8'), re.M)}


def build(gid, patched, control):
    platform = os.environ.get('SETTINGS_PLATFORM') or 'ios'
    if platform not in ('ios', 'mac'): sys.exit(f'SETTINGS_PLATFORM must be ios or mac: {platform}')
    g = game.load(gid)
    macros = dict(re.findall(r'^#define (DM_GAME_\w+) (.*)$', game.header(g), re.M))   # the host's own defaults, as it is compiled for this game
    keys = fixups_keys(Path(patched) / 'main.fixups.tsv')
    langs = [(c, n) for c, n in nacp.LANGUAGES if c in nacp.languages(control)]

    def macro(name, n):
        if name not in macros: sys.exit(f'settings.tsv:{n}: no {name} in the game profile header (game.py header)')
        return macros[name].strip('"')

    def shown(cond, n):
        if cond == 'always': return True
        if cond == 'languages>1': return len(langs) > 1
        if cond.startswith('fixups:'): return cond[7:] in keys
        if cond.startswith('gfxapi:'): return os.environ.get('GFXAPI', '') == cond[7:]
        if cond.startswith('platform:'): return platform == cond[9:]
        m = re.fullmatch(r'(DM_GAME_\w+)(!=|==)(.*)', cond)
        if m: return (macro(m[1], n) == m[3]) == (m[2] == '==')
        sys.exit(f'settings.tsv:{n}: unknown condition {cond}')

    def value(d, n):
        if '|' in d:
            parts = d.split('|')
            if len(parts) != 2 or not all(parts): sys.exit(f'settings.tsv:{n}: platform default is <ios>|<mac>: {d}')
            d = parts[0] if platform == 'ios' else parts[1]
        if d.startswith('DM_GAME_'): return macro(d, n)
        if d.startswith('build:'):
            v = os.environ.get(d[6:], '')
            return {'YES': '1', 'NO': '0', '': '0'}.get(v, v)   # build_ipa.sh's yes / no switches
        return d

    def takes(kind, values):   # the values a row's variable can take, for needs; None when any (a range, the game's languages)
        if kind == 'toggle': return {'1', '0'} if values == '-' else set(values.split('/'))
        if kind == 'choice' and values != 'languages': return {v.split('=', 1)[0] for v in values.split(',')}
        return None

    out, table, base = [], {r[1][0]: takes(r[1][1], r[1][2]) for r in rows()}, render_base(g)
    for n, (var, kind, values, default, cond, section, label, note, extra) in rows():
        m = meta(extra, n)
        if 'needs' in m:
            need, want = m['needs'].split('=', 1)
            if need not in table: sys.exit(f'settings.tsv:{n}: needs names a variable no row sets: {m["needs"]}')
            can = table[need]
            if can is not None and want not in can: sys.exit(f'settings.tsv:{n}: needs a value {need} never takes: {want} (it takes {", ".join(sorted(can))})')
        if not shown(cond, n): continue
        s = {'env': var, 'kind': kind, 'section': section, 'label': label, 'note': note, 'default': value(default, n), **m}
        if var == 'DM_RENDER_SCALE' and base: s['base'] = base   # the render scale in pixels, for the console mode chosen
        if kind == 'choice':
            opts = [list(l) for l in langs] if values == 'languages' else [v.split('=', 1) for v in values.split(',')]
            if any(len(o) != 2 for o in opts): sys.exit(f'settings.tsv:{n}: choice values are value=label,...')
            if s['default'] not in [o[0] for o in opts]: s['default'] = opts[0][0]   # a default the game does not offer (a language it lacks): its first
            s['options'] = opts
        elif kind == 'range':
            lo, hi, st = (float(x) for x in values.split(':'))
            s.update(min=lo, max=hi, step=st)
        elif kind == 'toggle':
            if s['default'] not in ('0', '1'): sys.exit(f'settings.tsv:{n}: a toggle defaults to 0 or 1: {s["default"]}')
            on_off = ['1', '0'] if values == '-' else values.split('/')
            if len(on_off) != 2 or not all(on_off) or on_off[0] == on_off[1]: sys.exit(f'settings.tsv:{n}: toggle values are - or <on>/<off>: {values}')
            s.update(on=on_off[0], off=on_off[1], default=on_off[0] if s['default'] == '1' else on_off[1])   # the default as the value the variable takes
        else: sys.exit(f'settings.tsv:{n}: unknown kind {kind}')
        out.append(s)
    return {'schema': SCHEMA, 'game': g.id, 'settings': out}


if __name__ == '__main__':
    if len(sys.argv) != 5: sys.exit('usage: settings.py <game id> <rewritten modules dir> <control.nacp> <out.json>')
    d = build(*sys.argv[1:4])
    Path(sys.argv[4]).write_text(json.dumps(d, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    print(' '.join(s['env'] for s in d['settings']))
