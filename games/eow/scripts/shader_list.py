#!/usr/bin/env python3
"""Echoes of Wisdom's shader list (assets/shader_list.tsv): bind states play met beyond the game code, identifiers only.

  V <shader key> <deps>       a bind state the default variant does not serve: the shader (SHA-256 of its NVN control
                              section and code, the runtime's key) and the deps the runtime's translation reported.
prepare_shader_seed.py translates each into the seed; its --list-from adds the bind states of a played shader cache.
"""
import hashlib
import json
import re

CHAIN = 'totk-native-v4-unverified'
HEADER = ('# Echoes of Wisdom shader list v1 (games/eow/scripts/shader_list.py; games/eow/README.md).\n'
          '# Bind states beyond the default, met in play. Identifiers only, no game data. Translation chain '
          + CHAIN + '.\n')
ANSWERS = dict(tex={}, img={}, attr={}, fout={})
DEFAULTS = dict(tex='2d', img={}, attr='float', fout='float')   # totk_shader_rt.m deps_match
KEY = re.compile(r'^[0-9a-f]{64}$')


class ListError(Exception):
    pass


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':'))


def variant_name(key, answers):
    """totk_shader_rt.m totk_rt_variant: the key and the first 6 bytes of the SHA-256 of the answers' sorted JSON."""
    return key + '-' + hashlib.sha256(canonical(answers).encode()).hexdigest()[:12]


def deps_match(deps, answers):
    """Whether a variant translated with deps serves these answers (totk_shader_rt.m deps_match): every slot the deps
    name must be answered the same, an unanswered slot counting as its kind's default."""
    for kind, default in DEFAULTS.items():
        have = answers.get(kind, {})
        for slot, want in deps.get(kind, {}).items():
            if have.get(slot, default) != want:
                return False
    return True


def answers_for(deps):
    """The bind state to translate deps with: the deps, every other kind empty."""
    return {**ANSWERS, **{k: v for k, v in deps.items() if k in ANSWERS}}


def parse_row(fields):
    """One list row -> (key, deps), or None when it is not one."""
    if len(fields) != 3 or fields[0] != 'V' or not KEY.match(fields[1]):
        return None
    try:
        deps = json.loads(fields[2])
    except ValueError:
        return None
    if not isinstance(deps, dict) or set(deps) - set(ANSWERS):
        return None
    return fields[1], deps


def read(path):
    """The list's rows [(shader key, deps)]."""
    rows = []
    if path.is_file():
        for n, line in enumerate(path.read_text().splitlines(), 1):
            if not line or line.startswith('#'):
                continue
            row = parse_row(line.split('\t'))
            if row is None:
                raise ListError(f'{path}:{n}: expected V <shader key> <deps>')
            rows.append(row)
    return rows


def write(path, rows):
    path.write_text(HEADER + ''.join(sorted(f'V\t{k}\t{canonical(d)}\n' for k, d in rows)))


def add(path, seed, caches):
    """Add each bind state of the played caches that no variant of its shader in the seed serves. -> how many."""
    rows = read(path)
    known = {(k, canonical(d)) for k, d in rows}
    added = 0
    for cache in caches:
        for index in sorted(cache.glob('*.variants')):
            key, mine = index.name[:-9], seed / index.name
            if not KEY.match(key) or not mine.is_file():
                continue   # not a shader of the seed: nothing to translate it from
            have = [r['deps'] for r in json.loads(mine.read_text())]
            for row in json.loads(index.read_text()):
                deps = {k: v for k, v in row.get('deps', {}).items() if k in ANSWERS}
                if not any(deps_match(h, answers_for(deps)) for h in have) and (key, canonical(deps)) not in known:
                    known.add((key, canonical(deps)))
                    rows.append((key, deps))
                    added += 1
    write(path, rows)
    return added
