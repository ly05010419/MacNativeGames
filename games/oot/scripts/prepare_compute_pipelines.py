"""Plan all real compute variants from the verified seed, including unplayed shaders.

Task/mesh kernels translated through compute are not compute dispatch pipelines.
The emitted C rows use runtime variant identities, not source hashes or base keys.
"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re


def compute_rows(seed):
    seed = Path(seed)
    manifest = json.loads((seed / 'seed-manifest.json').read_text())
    names = [n for n in manifest['files'] if n.endswith('_b.json')]

    def inspect(name):
        if Path(name).name != name:
            raise ValueError('unsafe seed filename')
        raw = (seed / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != manifest['files'][name]:
            raise ValueError('damaged compute reflection: ' + name)
        data = json.loads(raw)
        if data.get('stage') != 'cs':
            return None
        key = name[:-5]
        if (data.get('chain') != manifest['chain'] or data.get('key') != key or
                not re.fullmatch(r'[0-9a-f]{64}-[0-9a-f]{12}_b', key) or
                data.get('msl_entry_kind') != 'kernel' or not data.get('entry')):
            raise ValueError('invalid compute reflection: ' + name)
        source = key + '.metal'
        raw_source = (seed / source).read_bytes()
        if hashlib.sha256(raw_source).hexdigest() != manifest['files'].get(source):
            raise ValueError('damaged compute source: ' + source)
        if b'[[function_constant(' in raw_source:
            raise ValueError('compute constants need an explicit specialization: ' + key)
        return 'C\t' + key

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        return sorted(set(row for row in pool.map(inspect, names) if row))
