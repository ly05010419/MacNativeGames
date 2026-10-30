#!/usr/bin/env python3
# Answer table for translation queries (dm/games/<id>/shader_facts.tsv in the game profile folder): one row per Switch shader key, recording the few facts the translator asks for but that cannot be read from the Switch machine code -
# dimension type of textures / images, image format, component types of vertex attributes and fragment outputs, thread group size and shared memory bytes of compute shaders.
# shader_split.py writes the table from what the game files show (merging the profile's shader_facts_manual.tsv over it); for keys not in the table, translation queries are answered with defaults
# (listed separately in the build summary).
# Usage: dmsh_facts.py            print the table's row count and the coverage count of each fact
# Row format: key <TAB> JSON (keys in lexicographic order, compact form; the same input gives a byte-identical table)
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../games')); import game as _game   # the tables are in the current game profile folder (DM_GAME)
TABLE = str(_game.load().table('shader_facts.tsv'))
def load():
    """key → fact dict; returns an empty dict if the table does not exist"""
    if not os.path.exists(TABLE): return {}
    return {k: json.loads(v) for k, v in (l.rstrip('\n').split('\t', 1) for l in open(TABLE) if l.strip() and not l.startswith('#'))}

def row(key, facts): return key + '\t' + json.dumps(facts, sort_keys=True, separators=(',', ':'), ensure_ascii=False) + '\n'

if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        t = load(); print(f'{len(t)} keys: ' + ', '.join(f'{n} {sum(bool(f.get(n)) for f in t.values())}' for n in ('tex', 'img', 'attr', 'fout', 'local', 'shared')))
    else: sys.exit('usage: see the file header')
